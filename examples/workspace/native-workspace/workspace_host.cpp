#include "workspace_host.hpp"
#include <RobloxModLoader/qt/qt_integration.hpp>
#include <algorithm>
#include <atomic>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace rml::workspace {
namespace {
struct ViewRecord {
    ViewRegistration registration;
    std::shared_ptr<const SplitNode> layout;
    PublicViewState published;
    bool registration_pending{};
};
struct Job { std::string request; std::function<void()> run; OwnerId owner{}; };
struct Host {
    std::atomic_bool active{false};
    std::mutex mutex;
    std::filesystem::path reports;
    DockingBackend docking; // Stateless; GUI-thread only.
    std::unique_ptr<ViewStack> controller; // GUI-thread only.
    std::map<std::string, PanelRegistration> panels;
    std::map<std::string, ViewRecord> views;
    std::map<std::string, std::string> names;
    std::map<std::string, RequestInfo> requests;
    std::deque<std::string> request_order;
    std::deque<Job> jobs;
    std::vector<Delivery> events;
    std::vector<ViewRegistration> stack;
    std::vector<PanelInfo> inventory;
    std::set<OwnerId> released;
    std::uint64_t sequence{};
    std::map<std::uint64_t, Delivery> delivering;
    bool stopping{};
    bool pump_scheduled{};
};
Host& host() { static auto* state = new Host; return *state; }
void schedule();

void write_report(std::string_view text) {
    auto& state = host();
    std::ofstream output(state.reports / "workspace-runtime.txt", std::ios::app);
    output << "pid=" << getpid() << ' ' << text << '\n';
}
std::string next_id_locked(std::string_view prefix) {
    return std::string(prefix) + "-" + std::to_string(++host().sequence);
}
RequestInfo enqueue(std::function<void()> function, std::string view_id = {}, bool cleanup = false, OwnerId owner_id = 0) {
    auto& state = host();
    if (!state.active) throw std::runtime_error("Workspace mod is inactive");
    RequestInfo info;
    {
        std::scoped_lock lock(state.mutex);
        if (state.stopping) throw std::runtime_error("Workspace is stopping");
        if (!cleanup && state.jobs.size() >= 128) throw std::runtime_error("Workspace request queue is full");
        info = {next_id_locked("request"), "queued", {}, {}};
        state.requests.emplace(info.id, info);
        state.request_order.push_back(info.id);
        while (state.request_order.size() > 256) {
            const auto& old = state.request_order.front();
            if (state.requests.at(old).status == "queued") break;
            state.requests.erase(old); state.request_order.pop_front();
        }
        OwnerId owner{owner_id};
        if(auto it=state.views.find(view_id);it!=state.views.end()) owner=it->second.registration.owner;
        Job job{info.id,std::move(function),owner};
        if(cleanup) state.jobs.push_front(std::move(job)); else state.jobs.push_back(std::move(job));
        if (auto it = state.views.find(view_id); it != state.views.end()) {
            it->second.published.request_id = info.id;
            it->second.published.status = "queued";
            it->second.published.error.clear();
        }
    }
    try { schedule(); }
    catch(const std::exception& exception) {
        std::scoped_lock lock(state.mutex);
        // Owner cleanup is independent of its now-unloading Lua VM. Retain it
        // at the front of the queue so a later available dispatcher can retry.
        if(!cleanup) std::erase_if(state.jobs,[&](const Job& job){return job.request==info.id;});
        auto& failed=state.requests.at(info.id); failed.status=cleanup?"queued":"failed"; failed.error=exception.what();
        throw;
    }
    return info;
}
void publish(std::string_view request, const TransitionResult& result,
             std::string_view completed_registration = {}) {
    auto& state = host();
    const auto stack = state.controller->get_view_stack();
    std::scoped_lock lock(state.mutex);
    auto& info = state.requests.at(std::string(request));
    info.status = result.succeeded() ? (result.status == Status::NoOp ? "noop" : "applied")
                                    : result.recovery ? "recovery-required" : "failed";
    info.error = result.succeeded() ? "" : result.message;
    state.stack.clear();
    for (const auto& frame : stack) state.stack.push_back(state.views.at(frame.id).registration);
    if (!completed_registration.empty())
        state.views.at(std::string(completed_registration)).registration_pending = false;
    for (auto& [id, view] : state.views) {
        // A different GUI job may finish while this view is only staged in the
        // host. Keep it discoverable for Connect until its registration runs;
        // the controller's temporary absence is not an unregister operation.
        if (!view.registration_pending)
            view.published.state = to_string(state.controller->get_state(id));
        view.published.recovery_required = state.controller->last_recovery().has_value();
        const bool notified=std::any_of(result.events.begin(),result.events.end(),[&](const Event& event){return event.view_id==id;});
        if (view.published.request_id == request || notified) {
            view.published.request_id=request;
            view.published.status = info.status;
            view.published.error = info.error;
        }
    }
    for (const auto& event : result.events) {
        auto it = state.views.find(event.view_id);
        if (it != state.views.end() && !state.released.contains(it->second.registration.owner))
            state.events.push_back({it->second.registration.owner, event, std::string(request), ++state.sequence});
        write_report("event=" + std::string(to_string(event.kind)) + " view=" + event.view_id + " reason=" + to_string(event.reason));
    }
    write_report("request=" + std::string(request) + " status=" + info.status + " stack=" + std::to_string(stack.size()));
}
// The executing request is a GUI-thread-only value. It never escapes to Lua.
std::string current_request;
void append_request_report(std::string_view report) noexcept {
    if (current_request.empty() || report.empty()) return;
    try {
        auto& state = host();
        std::scoped_lock lock(state.mutex);
        const auto request = state.requests.find(current_request);
        if (request == state.requests.end()) return;

        constexpr std::size_t kMaxRequestReportBytes = 64 * 1024;
        constexpr std::string_view kTruncated = "\n[native report truncated]";
        auto& destination = request->second.report;
        if (destination.size() >= kMaxRequestReportBytes) return;
        if (!destination.empty() && destination.back() != '\n') destination.push_back('\n');
        const auto remaining = kMaxRequestReportBytes - destination.size();
        if (report.size() <= remaining) {
            destination.append(report);
        } else if (remaining > kTruncated.size()) {
            destination.append(report.substr(0, remaining - kTruncated.size()));
            destination.append(kTruncated);
        } else {
            destination.append(report.substr(0, remaining));
        }
    } catch (...) {
        // Request diagnostics must never turn a successful native operation
        // into a failed view transition.
    }
}
void pump() {
    auto& state = host();
    Job job;
    {
        std::scoped_lock lock(state.mutex);
        state.pump_scheduled = false;
        if (!state.active || state.jobs.empty() || !state.events.empty() || !state.delivering.empty()) return;
        job = std::move(state.jobs.front()); state.jobs.pop_front();
    }
    current_request = job.request;
    try {
        job.run();
        try {
            auto inventory = state.docking.list_panels();
            std::scoped_lock lock(state.mutex); state.inventory = std::move(inventory);
        } catch(const std::exception& exception) {
            write_report("inventory_unavailable="+std::string(exception.what()));
        }
        { std::scoped_lock lock(state.mutex); auto& info=state.requests.at(job.request); if(info.status=="queued") info.status="applied"; }
    } catch (const std::exception& exception) {
        std::scoped_lock lock(state.mutex);
        auto& info = state.requests.at(job.request);
        info.status = "failed"; info.error = exception.what();
        for (auto& [id, view] : state.views) if (view.published.request_id == job.request) {
            view.published.status = "failed"; view.published.error = info.error;
        }
        write_report("request=" + job.request + " failed=" + info.error);
    }
    current_request.clear();
    schedule();
}
void schedule() {
    auto& state = host();
    {
        std::scoped_lock lock(state.mutex);
        if (!state.active || state.pump_scheduled || state.jobs.empty() || !state.events.empty() || !state.delivering.empty()) return;
        state.pump_scheduled = true;
    }
    auto* integration = rml::qt::QtIntegration::instance();
    if (!integration) {
        std::scoped_lock lock(state.mutex); state.pump_scheduled=false;
        throw std::runtime_error("Studio GUI dispatcher is unavailable");
    }
    try { integration->run_on_gui_thread([] { pump(); }); }
    catch(...) { std::scoped_lock lock(state.mutex); state.pump_scheduled=false; throw; }
}
std::shared_ptr<const SplitNode> resolve_tree(const SplitNode& node, OwnerId owner, int depth = 0) {
    if (depth > 8) throw std::runtime_error("Workspace view is too deep");
    auto copy = std::make_shared<SplitNode>(node);
    if (!node.panel_id.empty()) {
        if (node.panel_id.starts_with("builtin:")) copy->panel_id = node.panel_id.substr(8);
        else {
            auto& state = host(); std::scoped_lock lock(state.mutex);
            const auto it = state.panels.find(node.panel_id);
            if (it == state.panels.end() || it->second.owner != owner)
                throw std::runtime_error("View may place only its owner's panels and built-in panels");
            copy->panel_id = it->second.suffix;
        }
    } else {
        if (!node.first || !node.second) throw std::runtime_error("Workspace split needs both children");
        copy->first = resolve_tree(*node.first, owner, depth + 1);
        copy->second = resolve_tree(*node.second, owner, depth + 1);
    }
    return copy;
}
std::string panel_selector(std::string_view id, OwnerId* owner = nullptr) {
    if (id.starts_with("builtin:")) return std::string(id.substr(8));
    auto& state = host(); std::scoped_lock lock(state.mutex);
    auto it = state.panels.find(std::string(id));
    if (it == state.panels.end() || state.released.contains(it->second.owner)) throw std::runtime_error("Unknown or released panel ID");
    if(owner) *owner=it->second.owner;
    return it->second.suffix;
}
RequestInfo transition(std::string_view id, int action) {
    const auto view = get_view(id);
    return enqueue([id = view.id, action] {
        auto& state = host();
        TransitionResult result = action == 0 ? state.controller->enter(id)
                                : action == 1 ? state.controller->exit(id)
                                              : state.controller->unregister_view(id);
        publish(current_request, result);
    }, view.id);
}
void retry_released_views() {
    auto& state = host();
    std::vector<std::string> ids;
    {
        std::scoped_lock lock(state.mutex);
        for (const auto& [id, view] : state.views)
            if (state.released.contains(view.registration.owner)) ids.push_back(id);
    }
    std::erase_if(ids, [&](const std::string& id) {
        if (state.controller->get_state(id) != ViewState::Unregistered) return false;
        std::scoped_lock lock(state.mutex);
        if (auto it = state.views.find(id); it != state.views.end())
        {
            it->second.registration_pending = false;
            it->second.published.state = "unregistered";
        }
        return true;
    });
    const auto mark_request = [&state](const std::string& id) {
        std::scoped_lock lock(state.mutex);
        if (auto it = state.views.find(id); it != state.views.end()) {
            it->second.published.request_id = current_request;
            it->second.published.status = "queued";
            it->second.published.error.clear();
        }
    };
    if (const auto recovery = state.controller->last_recovery()) {
        for (const auto& id : ids) mark_request(id);
        publish(current_request, TransitionResult{
            Status::RecoveryPending,
            "restore the retained recovery layout before cleaning up released views",
            {},
            recovery});
        return;
    }

    // Remove suspended views first. This splices their saved layout into the
    // next frame, so unregistering an active released view cannot restore a
    // dead predecessor on the way back to the normal layout.
    const auto stack = state.controller->get_view_stack();
    const auto rank = [&stack](const std::string& id) {
        for (std::size_t index = 0; index < stack.size(); ++index)
            if (stack[index].id == id) return index + 1;
        return std::size_t{};
    };
    std::stable_sort(ids.begin(), ids.end(), [&](const auto& left, const auto& right) {
        return rank(left) < rank(right);
    });

    for (const auto& id : ids) {
        mark_request(id);
        const auto result = state.controller->unregister_view(id);
        publish(current_request, result);
        // Leave remaining released views registered after any failed removal.
        // RecoverLayout retries them when a snapshot is retained; when rollback
        // succeeded, the same API's no-op recovery also acts as a safe retry.
        if (!result.succeeded()) return;
    }
}
}

void start_host(const std::filesystem::path& reports) {
    auto& state = host();
    if (state.active || state.controller) throw std::runtime_error("Workspace restart is required for reload");
    state.reports = reports;
    std::filesystem::create_directories(reports);
    state.controller = std::make_unique<ViewStack>(Backend{
        [&](std::string& error)->std::optional<std::string> { try { const auto result=state.docking.capture(state.reports); write_report(result.report); append_request_report(result.report); return result.path.string(); } catch(const std::exception& ex) { error=ex.what(); return {}; } },
        [&](std::string_view path, std::string& error) { try { const auto result=state.docking.restore(std::string(path)); write_report(result.report); append_request_report(result.report); return true; } catch(const std::exception& ex) { error=ex.what(); return false; } },
        [&](std::string_view descriptor, std::string& error) { try {
            std::shared_ptr<const SplitNode> tree;
            { std::scoped_lock lock(state.mutex); tree=state.views.at(std::string(descriptor)).layout; }
            const auto result=state.docking.apply(*tree); write_report(result.report); append_request_report(result.report); return true;
        } catch(const std::exception& ex) { error=ex.what(); return false; } }
    });
    state.active=true;
    write_report("workspace=loaded");
}
void stop_host() {
    auto& state=host();
    // Keep the process-pinned image alive while the GUI queue restores baseline.
    if (!state.active) return;
    std::string shutdown_id;
    {
        std::scoped_lock lock(state.mutex);
        if(state.stopping) return;
        state.stopping=true;
        state.events.clear(); state.delivering.clear();
        for(const auto& job:state.jobs) { auto& request=state.requests.at(job.request); request.status="cancelled"; request.error="Workspace stopped"; }
        state.jobs.clear();
        const auto id=next_id_locked("shutdown"); shutdown_id=id; state.requests.emplace(id,RequestInfo{id,"queued",{},{}});
        state.jobs.push_back({id,[] {
        auto& state=host();
        const auto retain_recovery=[&](const TransitionResult& result) {
            std::scoped_lock lock(state.mutex);
            auto& request=state.requests.at(current_request);
            const bool recovery=result.recovery.has_value() || state.controller->last_recovery().has_value();
            request.status=recovery?"recovery-required":"failed"; request.error=result.message;
            state.stopping=false;
            state.stack.clear();
            for(const auto& frame:state.controller->get_view_stack()) state.stack.push_back(state.views.at(frame.id).registration);
            for(auto& [id,view]:state.views) {
                view.registration_pending=false;
                view.published.state=to_string(state.controller->get_state(id));
                view.published.recovery_required=recovery;
                view.published.request_id=current_request;
                view.published.status=request.status; view.published.error=request.error;
            }
            write_report("shutdown_restore="+request.status+" recovery_snapshot_retained="+(recovery?"true":"false"));
        };
        if(state.controller->last_recovery()) {
            const auto recovered=state.controller->recover_layout();
            if(!recovered.succeeded()) { retain_recovery(recovered); return; }
        }
        for (auto stack=state.controller->get_view_stack(); !stack.empty(); stack=state.controller->get_view_stack()) {
            auto result=state.controller->exit(stack.back().id);
            if(!result.succeeded()) { retain_recovery(result); return; }
        }
        { std::scoped_lock lock(state.mutex);
          state.stack.clear();
          for(auto& [id,view]:state.views) {
              view.registration_pending=false;
              view.published.state=to_string(state.controller->get_state(id));
              view.published.recovery_required=false;
          }
          state.active=false;
        }
        write_report("workspace=stopped");
        }});
    }
    try { schedule(); }
    catch(const std::exception& exception) {
        std::scoped_lock lock(state.mutex);
        std::erase_if(state.jobs,[&](const Job& job){return job.request==shutdown_id;});
        auto& request=state.requests.at(shutdown_id);
        request.status="failed"; request.error=exception.what(); state.stopping=false;
        throw;
    }
}
bool host_active() { return host().active; }
PanelRegistration associate_panel(OwnerId owner, std::string suffix) {
    auto& state=host(); std::scoped_lock lock(state.mutex);
    if (!owner || suffix.empty() || state.released.contains(owner)) throw std::runtime_error("Invalid workspace panel owner");
    PanelRegistration panel{next_id_locked("panel"),std::move(suffix),owner};
    state.panels.emplace(panel.id,panel); return panel;
}
std::string builtin_panel(std::string_view name) {
    static const std::set<std::string_view> allowed{"Viewport","Output","Explorer","Properties","Toolbox"};
    if (!allowed.contains(name)) throw std::runtime_error("Unsupported built-in panel");
    return "builtin:"+std::string(name);
}
ViewRegistration register_view(OwnerId owner, std::string name, std::shared_ptr<const SplitNode> layout) {
    if (!layout) throw std::runtime_error("View layout is missing");
    const auto resolved=resolve_tree(*layout,owner);
    auto& state=host(); ViewRegistration view;
    {
        std::scoped_lock lock(state.mutex);
        if (!owner || name.empty() || name.size()>128 || state.released.contains(owner)) throw std::runtime_error("Invalid workspace view owner/name");
        if (auto it=state.names.find(name); it!=state.names.end() && state.views.at(it->second).published.state!="unregistered" && !state.released.contains(state.views.at(it->second).registration.owner))
            throw std::runtime_error("View name is already registered");
        view={next_id_locked("view"),std::move(name),owner};
        ViewRecord record{view,resolved,{view,"registering",{},"queued",{},false},true};
        state.views.emplace(view.id,std::move(record)); state.names[view.name]=view.id;
    }
    try {
        enqueue([view] {
            try {
                const auto result = host().controller->register_view({view.id,view.name,{},view.id});
                publish(current_request, result, view.id);
            } catch (...) {
                // A thrown registration failure is terminal too. The pump
                // records its failed request; do not leave a pending handle.
                auto& state = host();
                std::scoped_lock lock(state.mutex);
                auto& record = state.views.at(view.id);
                record.registration_pending = false;
                record.published.state = to_string(state.controller->get_state(view.id));
                throw;
            }
        },view.id);
    }
    catch(...) {
        std::scoped_lock lock(state.mutex); state.views.erase(view.id);
        if(auto it=state.names.find(view.name);it!=state.names.end() && it->second==view.id) state.names.erase(it);
        throw;
    }
    return view;
}
ViewRegistration get_view(std::string_view name) {
    auto& state=host(); std::scoped_lock lock(state.mutex);
    std::string id(name);
    if (auto it=state.names.find(id); it!=state.names.end()) id=it->second;
    auto it=state.views.find(id);
    if (it==state.views.end() || state.released.contains(it->second.registration.owner) || it->second.published.state=="unregistered") throw std::runtime_error("View is not registered");
    return it->second.registration;
}
PublicViewState get_state(std::string_view id) {
    auto& state=host(); std::scoped_lock lock(state.mutex);
    auto it=state.views.find(std::string(id));
    if(it==state.views.end()) throw std::runtime_error("Unknown view handle");
    return it->second.published;
}
std::vector<ViewRegistration> get_stack() { auto& state=host(); std::scoped_lock lock(state.mutex); return state.stack; }
RequestInfo enter_view(std::string_view id) { return transition(id,0); }
RequestInfo exit_view(std::string_view id) { return transition(id,1); }
RequestInfo unregister_view(std::string_view id) { return transition(id,2); }
RequestInfo open_panel(std::string_view id) { OwnerId owner{}; (void)panel_selector(id,&owner); return enqueue([id=std::string(id)] {
    auto result=host().docking.open_panel(panel_selector(id)); std::scoped_lock lock(host().mutex); host().requests.at(current_request).report=result.report;
}, {}, false, owner); }
RequestInfo close_panel(std::string_view id) { OwnerId owner{}; (void)panel_selector(id,&owner); return enqueue([id=std::string(id)] {
    auto result=host().docking.close_panel(panel_selector(id)); std::scoped_lock lock(host().mutex); host().requests.at(current_request).report=result.report;
}, {}, false, owner); }
RequestInfo inspect_panels() { return enqueue([] {
    const auto result=host().docking.inspect(); std::scoped_lock lock(host().mutex); host().requests.at(current_request).report=result;
}); }
RequestInfo recover_layout() { return enqueue([] {
    const auto result = host().controller->recover_layout();
    publish(current_request, result);
    if (result.succeeded()) retry_released_views();
}); }
RequestInfo get_request(std::string_view id) { auto& state=host(); std::scoped_lock lock(state.mutex); auto it=state.requests.find(std::string(id)); if(it==state.requests.end()) throw std::runtime_error("Request is unknown or expired"); return it->second; }
std::vector<PanelInfo> latest_panels() { auto& state=host(); std::scoped_lock lock(state.mutex); return state.inventory; }
std::vector<Delivery> take_events(const std::vector<OwnerId>& owners) {
    auto& state=host(); std::scoped_lock lock(state.mutex); std::vector<Delivery> result;
    if(!state.delivering.empty()) return result;
    for(auto it=state.events.begin();it!=state.events.end();) {
        if(std::find(owners.begin(),owners.end(),it->owner)!=owners.end()) { result.push_back(*it); state.delivering.emplace(it->receipt,*it); it=state.events.erase(it); }
        else break; // Preserve outgoing-before-incoming ordering across VMs.
    }
    return result;
}
void complete_events(const std::vector<Delivery>& events) {
    { auto& state=host(); std::scoped_lock lock(state.mutex);
      for(const auto& event:events) { auto it=state.delivering.find(event.receipt); if(it==state.delivering.end()) continue;
        const auto& expected=it->second;
        if(expected.owner!=event.owner || expected.request_id!=event.request_id || expected.event.view_id!=event.event.view_id || expected.event.kind!=event.event.kind || expected.event.reason!=event.event.reason)
            throw std::runtime_error("Event delivery acknowledgement mismatch");
        state.delivering.erase(it);
      }
    }
    schedule();
}
void release_owner(OwnerId owner) {
    auto& state=host();
    {
        std::scoped_lock lock(state.mutex); state.released.insert(owner);
        std::erase_if(state.events,[owner](const Delivery& delivery){return delivery.owner==owner;});
        std::erase_if(state.delivering,[owner](const auto& item){return item.second.owner==owner;});
        std::erase_if(state.jobs,[&](const Job& job){
            if(job.owner!=owner) return false;
            auto& request=state.requests.at(job.request); request.status="cancelled"; request.error="Plugin owner unloaded";
            return true;
        });
        if(state.stopping) return;
    }
    enqueue([] { retry_released_views(); },{},true);
}
}
