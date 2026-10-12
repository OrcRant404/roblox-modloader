#include "workspace_lua.hpp"
#include "workspace_host.hpp"
#include <RobloxModLoader/luau/engine_api.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rml::workspace {
namespace {
constexpr std::size_t kOwnerLimit = 32;
constexpr std::size_t kListenerLimit = 128;
constexpr std::size_t kReferenceLimit = 1024;
constexpr unsigned kSplitDepthLimit = 8;
constexpr int kWorkspaceRegistryIndex = -10000;
constexpr char kReferenceStorageKey[] = "__rml_workspace_refs_v1";
constexpr char kReferenceOwnerKey[] = "__owner";
std::array<char, 33> reference_owner_token{};
std::atomic<bool> active{false};
std::atomic<const EngineApi*> engine{nullptr};
std::atomic<std::uint64_t> owner_serial{1}, listener_serial{1};
std::atomic<std::uint64_t> reference_serial{1};

struct PanelRecord { std::string local_id; std::string id; int reference{}; };
struct OwnerRecord {
    OwnerId id{};
    int plugin_reference{};
    int unloading_connection{};
    int heartbeat_connection{};
    std::vector<PanelRecord> panels;
};
struct Listener {
    std::string id;
    OwnerId owner{};
    std::string view_id;
    EventKind kind{};
    int reference{};
};
struct VmRecord {
    // This is an identity key, never a retained callback execution state.
    lua_State* identity{};
    std::vector<std::shared_ptr<OwnerRecord>> owners;
    std::vector<Listener> listeners;
    std::unordered_set<int> references;
    bool delivering{};
};
std::mutex records_mutex;
std::unordered_map<lua_State*, std::shared_ptr<VmRecord>> records;

int signal_connect(lua_State*);
int connection_disconnect(lua_State*);
int heartbeat(lua_State*);
int delayed_poll(lua_State*);
int plugin_unloading(lua_State*);

enum class OpKind {
    GetField, SetField, Method, Invoke, Reference, Unreference, GetReference,
    RawGet, String, Boolean, Number, Closure, Request, View, Connection, State,
    Event, Stack, Inspection, Error,
};
struct Operation {
    OpKind kind{};
    const char* key{};
    const char* text{};
    const char* failure{};
    std::size_t length{};
    int number{};
    int results{};
    int status{};
    double numeric{};
    bool executed{};
    EngineCFunction callback{};
    const RequestInfo* request{};
    const ViewRegistration* view{};
    const PublicViewState* state{};
    const Delivery* delivery{};
    const std::vector<ViewRegistration>* stack{};
    const std::vector<PanelInfo>* panels{};
};
// Synchronous protected operations may reenter via a listener. Restore the
// previous operation after each pcall; no borrowed operation outlives that call.
thread_local Operation* current_operation{};

void put_string(const EngineApi* api, lua_State* state, int table,
                const char* key, const char* text, std::size_t length) {
    api->pushlstring(state, text, length);
    api->setfield(state, table, key);
}
void put_string(const EngineApi* api, lua_State* state, int table,
                const char* key, const std::string& text) {
    put_string(api, state, table, key, text.data(), text.size());
}
void put_literal(const EngineApi* api, lua_State* state, int table,
                 const char* key, const char* text) {
    put_string(api, state, table, key, text, std::char_traits<char>::length(text));
}
void put_boolean(const EngineApi* api, lua_State* state, int table,
                 const char* key, bool value) {
    api->pushboolean(state, value);
    api->setfield(state, table, key);
}
void put_number(const EngineApi* api, lua_State* state, int table,
                const char* key, double value) {
    api->pushnumber(state, value);
    api->setfield(state, table, key);
}
void make_signal(const EngineApi* api, lua_State* state,
                 const std::string& id, const char* kind) {
    api->createtable(state, 0, 3);
    const int table = api->gettop(state);
    put_string(api, state, table, "Id", id);
    put_literal(api, state, table, "Kind", kind);
    api->pushcclosurek(state, &signal_connect, "Workspace.Signal.Connect", 0, nullptr);
    api->setfield(state, table, "Connect");
    api->setreadonly(state, table, 1);
}
void make_view(const EngineApi* api, lua_State* state, const ViewRegistration* view,
               const char* view_state = nullptr) {
    api->createtable(state, 0, 5);
    const int table = api->gettop(state);
    put_string(api, state, table, "Id", view->id);
    put_string(api, state, table, "Name", view->name);
    make_signal(api, state, view->id, "Entered");
    api->setfield(state, table, "Entered");
    make_signal(api, state, view->id, "Exited");
    api->setfield(state, table, "Exited");
    if (view_state) put_literal(api, state, table, "state", view_state);
    api->setreadonly(state, table, 1);
}
void make_request(const EngineApi* api, lua_State* state, const RequestInfo* request) {
    api->createtable(state, 0, 5);
    const int table = api->gettop(state);
    put_string(api, state, table, "id", request->id);
    put_string(api, state, table, "status", request->status);
    put_string(api, state, table, "error", request->error);
    put_string(api, state, table, "report", request->report);
}

// The engine's private ref API dereferences an uninitialized table in ordinary
// Studio VMs. Root our own values in the verified public registry through CAPI
// table operations instead. Do not adopt or replace a foreign occupied key.
// All locals here are POD, and this function runs only beneath lua_pcall.
int reference_table(const EngineApi* api, lua_State* state, Operation* op, bool create) {
    if (api->type(state, kWorkspaceRegistryIndex) != kEngineTable) {
        op->failure = "the engine registry is not an initialized table"; return 0;
    }
    api->getfield(state, kWorkspaceRegistryIndex, kReferenceStorageKey);
    int table = api->gettop(state);
    if (api->type(state, table) == kEngineNil) {
        if (!create) { op->failure = "workspace reference storage is missing"; return 0; }
        if (api->getreadonly(state, kWorkspaceRegistryIndex)) {
            op->failure = "the engine registry is readonly; reference storage cannot be created"; return 0;
        }
        api->createtable(state, 0, 2);
        table = api->gettop(state);
        put_string(api, state, table, kReferenceOwnerKey, reference_owner_token.data(), 32);
        api->pushvalue(state, table);
        api->setfield(state, kWorkspaceRegistryIndex, kReferenceStorageKey);
    } else {
        if (api->type(state, table) != kEngineTable) {
            op->failure = "workspace reference storage key is occupied by another value"; return 0;
        }
        api->getfield(state, table, kReferenceOwnerKey);
        if (api->type(state, -1) != kEngineString) {
            op->failure = "workspace reference storage is owned by another provider"; return 0;
        }
        std::size_t size{};
        const char* token = api->tolstring(state, -1, &size);
        if (!token || size != 32 || std::char_traits<char>::compare(token, reference_owner_token.data(), 32) != 0) {
            op->failure = "workspace reference storage is owned by another provider"; return 0;
        }
        api->settop(state, table);
    }
    if (api->getreadonly(state, table)) {
        op->failure = "workspace reference storage became readonly"; return 0;
    }
    return table;
}

// Only POD locals/references are used in this trampoline and its writers.
// Lua errors from allocations, userdata metamethods or method dispatch return
// through the outer pcall instead of bypassing native C++ object destructors.
int protected_operation(lua_State* state) {
    const EngineApi* api = engine.load(std::memory_order_acquire);
    Operation* op = current_operation;
    if (!active.load(std::memory_order_acquire) || !api || !op) return 0;
    op->executed = true;
    switch (op->kind) {
    case OpKind::GetField:
        api->getfield(state, 1, op->key); return 1;
    case OpKind::SetField:
        api->pushvalue(state, 2); api->setfield(state, 1, op->key); return 0;
    case OpKind::Method: {
        const int count = api->gettop(state);
        api->getfield(state, 1, op->key);
        api->pushvalue(state, 1);
        for (int index = 2; index <= count; ++index) api->pushvalue(state, index);
        op->status = api->pcall(state, count, op->results, 0);
        return op->status ? 1 : op->results;
    }
    case OpKind::Invoke: {
        const int count = api->gettop(state);
        for (int index = 1; index <= count; ++index) api->pushvalue(state, index);
        op->status = api->pcall(state, count - 1, op->results, 0);
        return op->status ? 1 : op->results;
    }
    case OpKind::Reference: {
        const int table = reference_table(api, state, op, true);
        if (!table) return 0;
        api->pushvalue(state, 1); api->rawseti(state, table, op->number); return 0;
    }
    case OpKind::Unreference: {
        const int table = reference_table(api, state, op, false);
        if (!table) return 0;
        api->pushnil(state); api->rawseti(state, table, op->number); return 0;
    }
    case OpKind::GetReference: {
        const int table = reference_table(api, state, op, false);
        if (!table) return 0;
        api->rawgeti(state, table, op->number); return 1;
    }
    case OpKind::RawGet:
        api->rawgeti(state, 1, op->number); return 1;
    case OpKind::String:
        api->pushlstring(state, op->text, op->length); return 1;
    case OpKind::Boolean:
        api->pushboolean(state, op->number); return 1;
    case OpKind::Number:
        api->pushnumber(state, op->numeric); return 1;
    case OpKind::Closure:
        if (op->number) api->pushlstring(state, op->text, op->length);
        api->pushcclosurek(state, op->callback, op->key, op->number, nullptr); return 1;
    case OpKind::Request:
        make_request(api, state, op->request); api->setreadonly(state, -1, 1); return 1;
    case OpKind::View:
        make_view(api, state, op->view); return 1;
    case OpKind::Connection: {
        api->createtable(state, 0, 2);
        const int table = api->gettop(state);
        put_string(api, state, table, "Id", op->text, op->length);
        api->pushcclosurek(state, &connection_disconnect, "Workspace.Connection.Disconnect", 0, nullptr);
        api->setfield(state, table, "Disconnect");
        api->setreadonly(state, table, 1); return 1;
    }
    case OpKind::State: {
        api->createtable(state, 0, 7);
        const int table = api->gettop(state);
        put_string(api, state, table, "id", op->state->view.id);
        put_string(api, state, table, "name", op->state->view.name);
        put_string(api, state, table, "state", op->state->state);
        put_string(api, state, table, "requestId", op->state->request_id);
        put_string(api, state, table, "status", op->state->status);
        put_string(api, state, table, "error", op->state->error);
        put_boolean(api, state, table, "recoveryRequired", op->state->recovery_required);
        api->setreadonly(state, table, 1); return 1;
    }
    case OpKind::Event: {
        api->createtable(state, 0, 4);
        const int table = api->gettop(state);
        put_string(api, state, table, "viewId", op->delivery->event.view_id);
        put_literal(api, state, table, "kind", to_string(op->delivery->event.kind));
        put_literal(api, state, table, "reason", to_string(op->delivery->event.reason));
        put_string(api, state, table, "requestId", op->delivery->request_id);
        api->setreadonly(state, table, 1); return 1;
    }
    case OpKind::Stack: {
        api->createtable(state, static_cast<int>(op->stack->size()), 0);
        const int table = api->gettop(state);
        for (std::size_t index = 0; index < op->stack->size(); ++index) {
            make_view(api, state, &(*op->stack)[index], index + 1 == op->stack->size() ? "active" : "suspended");
            api->rawseti(state, table, static_cast<int>(index + 1));
        }
        api->setreadonly(state, table, 1); return 1;
    }
    case OpKind::Inspection: {
        make_request(api, state, op->request);
        const int request_table = api->gettop(state);
        api->createtable(state, static_cast<int>(op->panels->size()), 0);
        const int array = api->gettop(state);
        for (std::size_t index = 0; index < op->panels->size(); ++index) {
            const PanelInfo* panel = &(*op->panels)[index];
            api->createtable(state, 0, 10);
            const int table = api->gettop(state);
            put_string(api, state, table, "id", panel->id);
            put_boolean(api, state, table, "document", panel->document);
            put_boolean(api, state, table, "closed", panel->closed);
            put_boolean(api, state, table, "visible", panel->visible);
            put_number(api, state, table, "x", panel->x);
            put_number(api, state, table, "y", panel->y);
            put_number(api, state, table, "width", panel->width);
            put_number(api, state, table, "height", panel->height);
            put_number(api, state, table, "minimumWidth", panel->minimum_width);
            put_number(api, state, table, "minimumHeight", panel->minimum_height);
            api->setreadonly(state, table, 1);
            api->rawseti(state, array, static_cast<int>(index + 1));
        }
        api->setreadonly(state, array, 1);
        api->setfield(state, request_table, "panels");
        api->setreadonly(state, request_table, 1); return 1;
    }
    case OpKind::Error:
        api->pushnil(state); api->pushlstring(state, op->text, op->length); return 2;
    }
    return 0;
}

class Lua {
public:
    lua_State* state;
    const EngineApi& api;
    Lua(lua_State* value, const EngineApi& functions) : state(value), api(functions) {}
    int top() const { return api.gettop(state); }
    int absolute(int index) const { return index < 0 && index > -10000 ? top() + index + 1 : index; }
    void reset(int value) const { api.settop(state, value); }
    std::string string(int index, const char* description, std::size_t limit = 512) const {
        if (api.type(state, index) != kEngineString) throw std::runtime_error(std::string(description) + " must be a string");
        std::size_t size{};
        const char* text = api.tolstring(state, index, &size); // Already a string: no coercion/allocation.
        if (!text || size == 0 || size > limit || std::char_traits<char>::find(text, size, '\0'))
            throw std::runtime_error(std::string(description) + " must be nonempty, bounded, and contain no NUL");
        return std::string(text, size);
    }
    std::string error_text() const {
        if (api.type(state, -1) != kEngineString) return "engine call failed with a non-string error";
        std::size_t size{}; const char* text = api.tolstring(state, -1, &size);
        return text ? std::string(text, std::min<std::size_t>(size, 4096)) : "engine call failed";
    }
    void run(Operation& op, const std::vector<int>& inputs = {}) const {
        const int before = top();
        if (inputs.size() > 32 || !api.checkstack(state, static_cast<int>(inputs.size()) + 32))
            throw std::runtime_error("workspace could not reserve the engine stack");
        // The first helper closure allocation is not protected against engine
        // memory exhaustion. This bounded prototype does not promise that.
        api.pushcclosurek(state, &protected_operation, "Workspace.ProtectedOperation", 0, nullptr);
        for (const int index : inputs) api.pushvalue(state, index);
        Operation* previous = current_operation;
        current_operation = &op;
        const int status = api.pcall(state, static_cast<int>(inputs.size()), -1, 0);
        current_operation = previous;
        if (status || op.status || op.failure || !op.executed) {
            const std::string message = op.failure ? op.failure : (op.executed ? error_text() : "workspace API is inactive");
            reset(before);
            throw std::runtime_error(message);
        }
    }
    void field(int object, const char* key) const {
        Operation op{}; op.kind = OpKind::GetField; op.key = key; run(op, {absolute(object)});
    }
    void set_field(int object, const char* key, int value) const {
        Operation op{}; op.kind = OpKind::SetField; op.key = key;
        run(op, {absolute(object), absolute(value)});
    }
    void method(int object, const char* name, const std::vector<int>& arguments, int results = 1) const {
        std::vector<int> inputs{absolute(object)};
        for (const int index : arguments) inputs.push_back(absolute(index));
        Operation op{}; op.kind = OpKind::Method; op.key = name; op.results = results; run(op, inputs);
    }
    void invoke(int function, const std::vector<int>& arguments, int results = 0) const {
        std::vector<int> inputs{absolute(function)};
        for (const int index : arguments) inputs.push_back(absolute(index));
        Operation op{}; op.kind = OpKind::Invoke; op.results = results; run(op, inputs);
    }
    void push_string(std::string_view text) const {
        Operation op{}; op.kind = OpKind::String; op.text = text.data(); op.length = text.size(); run(op);
    }
    void push_boolean(bool value) const {
        Operation op{}; op.kind = OpKind::Boolean; op.number = value; run(op);
    }
    void push_number(double value) const {
        Operation op{}; op.kind = OpKind::Number; op.numeric = value; run(op);
    }
    int reference(int index) const {
        if (api.type(state, index) == kEngineNil) return 0;
        const auto serial = reference_serial.fetch_add(1, std::memory_order_relaxed);
        if (serial > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("workspace reference ID space is exhausted");
        const int id = static_cast<int>(serial);
        const auto identity = api.mainthread(state);
        std::shared_ptr<VmRecord> vm;
        {
            std::lock_guard lock(records_mutex);
            const auto found = records.find(identity);
            if (found == records.end()) throw std::runtime_error("workspace reference VM is not registered");
            vm = found->second;
            std::size_t count{};
            for (const auto& item : records) count += item.second->references.size();
            if (count >= kReferenceLimit) throw std::runtime_error("workspace live reference limit reached (1024)");
            vm->references.insert(id);
        }
        try {
            Operation op{}; op.kind = OpKind::Reference; op.number = id; run(op, {absolute(index)});
        } catch (...) {
            std::lock_guard lock(records_mutex); vm->references.erase(id);
            if (vm->owners.empty() && vm->references.empty()) records.erase(identity);
            throw;
        }
        return id;
    }
    void unreference(int reference) const {
        if (reference <= 0) return;
        const auto identity = api.mainthread(state);
        std::shared_ptr<VmRecord> vm;
        {
            std::lock_guard lock(records_mutex);
            const auto found = records.find(identity);
            if (found == records.end() || !found->second->references.contains(reference)) return;
            vm = found->second;
        }
        Operation op{}; op.kind = OpKind::Unreference; op.number = reference; run(op);
        {
            std::lock_guard lock(records_mutex); vm->references.erase(reference);
            if (vm->owners.empty() && vm->references.empty()) records.erase(identity);
        }
    }
    void get_reference(int reference) const {
        {
            std::lock_guard lock(records_mutex);
            const auto found = records.find(api.mainthread(state));
            if (found == records.end() || !found->second->references.contains(reference))
                throw std::runtime_error("workspace reference belongs to a different or released VM");
        }
        Operation op{}; op.kind = OpKind::GetReference; op.number = reference; run(op);
    }
    void closure(EngineCFunction callback, const char* name, std::string_view owner_id) const {
        Operation op{}; op.kind = OpKind::Closure; op.callback = callback; op.key = name; op.number = 1;
        op.text = owner_id.data(); op.length = owner_id.size(); run(op);
    }
    std::string handle_id(int index, const char* key = "Id") const {
        if (api.type(state, index) == kEngineString) return string(index, "handle ID");
        if (api.type(state, index) != kEngineTable) throw std::runtime_error("expected a handle or its ID string");
        const int before = top(); field(index, key);
        const std::string id = string(-1, "handle ID"); reset(before); return id;
    }
};

struct StackGuard {
    const Lua& lua;
    int before;
    explicit StackGuard(const Lua& value) : lua(value), before(value.top()) {}
    ~StackGuard() { lua.reset(before); }
};

std::shared_ptr<VmRecord> vm_for(const Lua& lua, bool create) {
    lua_State* identity = lua.api.mainthread(lua.state);
    if (!identity) throw std::runtime_error("engine VM identity is unavailable");
    std::lock_guard lock(records_mutex);
    const auto found = records.find(identity);
    if (found != records.end()) return found->second;
    if (!create) return {};
    auto vm = std::make_shared<VmRecord>(); vm->identity = identity;
    records.emplace(identity, vm); return vm;
}
std::shared_ptr<OwnerRecord> find_owner(const std::shared_ptr<VmRecord>& vm, OwnerId id) {
    if (!vm) return {};
    std::lock_guard lock(records_mutex);
    for (const auto& owner : vm->owners) if (owner->id == id) return owner;
    return {};
}
void safe_unref(const Lua& lua, int reference) noexcept {
    try { lua.unreference(reference); }
    catch (const std::exception& error) { std::fprintf(stderr, "[workspace] unref failed: %s\n", error.what()); }
    catch (...) { std::fprintf(stderr, "[workspace] unref failed\n"); }
}
void disconnect_engine_connection(const Lua& lua, int reference) noexcept {
    if (reference <= 0) return;
    try {
        StackGuard restore(lua);
        lua.get_reference(reference);
        lua.method(-1, "Disconnect", {}, 0);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[workspace] signal disconnect failed: %s\n", error.what());
    } catch (...) { std::fprintf(stderr, "[workspace] signal disconnect failed\n"); }
    safe_unref(lua, reference);
}
void drop_owner(const Lua& lua, const std::shared_ptr<VmRecord>& vm, OwnerId id) {
    std::shared_ptr<OwnerRecord> removed;
    std::vector<int> references;
    {
        std::lock_guard lock(records_mutex);
        const auto found = std::find_if(vm->owners.begin(), vm->owners.end(),
            [id](const auto& owner) { return owner->id == id; });
        if (found == vm->owners.end()) return;
        removed = *found; vm->owners.erase(found);
        for (auto iterator = vm->listeners.begin(); iterator != vm->listeners.end();) {
            if (iterator->owner == id) {
                references.push_back(iterator->reference);
                iterator = vm->listeners.erase(iterator); // Remove before clearing its registry slot.
            } else ++iterator;
        }
        // Keep the VM ledger until its remaining values have been unrooted.
    }
    try { release_owner(id); }
    catch (const std::exception& error) { std::fprintf(stderr, "[workspace] native owner release failed: %s\n", error.what()); }
    catch (...) { std::fprintf(stderr, "[workspace] native owner release failed\n"); }
    disconnect_engine_connection(lua, removed->heartbeat_connection);
    disconnect_engine_connection(lua, removed->unloading_connection);
    for (const int reference : references) safe_unref(lua, reference);
    for (const auto& panel : removed->panels) safe_unref(lua, panel.reference);
    safe_unref(lua, removed->plugin_reference);
}
OwnerId callback_owner(const Lua& lua) {
    if (lua.api.type(lua.state, kFirstUpvalueIndex) != kEngineString) return 0;
    std::size_t size{};
    const char* text = lua.api.tolstring(lua.state, kFirstUpvalueIndex, &size);
    OwnerId id{};
    if (!text || size == 0) return 0;
    const auto parsed = std::from_chars(text, text + size, id);
    return parsed.ec == std::errc{} && parsed.ptr == text + size ? id : 0;
}
void schedule_poll(const Lua& lua, OwnerId owner) {
    StackGuard restore(lua);
    lua.field(kGlobalsIndex, "task");
    lua.field(-1, "delay"); const int delay = lua.top();
    if (lua.api.type(lua.state, delay) != kEngineFunction)
        throw std::runtime_error("task.delay is unavailable for edit-mode event delivery");
    lua.push_number(0.05); const int duration = lua.top();
    const std::string id = std::to_string(owner);
    lua.closure(&delayed_poll, "Workspace.DelayedPoll", id);
    lua.invoke(delay, {duration, lua.top()}, 1);
    // The engine scheduler owns the scheduled closure/thread. Neither it nor
    // its execution state is kept in the native records.
}

std::shared_ptr<OwnerRecord> owner_for(const Lua& lua, int plugin) {
    if (lua.api.type(lua.state, plugin) != kEngineUserdata)
        throw std::runtime_error("plugin must be the real Studio Plugin object");
    const int absolute_plugin = lua.absolute(plugin);
    auto vm = vm_for(lua, true);
    std::vector<std::shared_ptr<OwnerRecord>> existing;
    { std::lock_guard lock(records_mutex); existing = vm->owners; }
    for (const auto& owner : existing) {
        StackGuard restore(lua); lua.get_reference(owner->plugin_reference);
        if (lua.api.rawequal(lua.state, absolute_plugin, -1)) return owner;
    }
    auto owner = std::make_shared<OwnerRecord>();
    owner->id = owner_serial.fetch_add(1, std::memory_order_relaxed);
    owner->plugin_reference = lua.reference(absolute_plugin);
    try {
        {
            std::lock_guard lock(records_mutex);
            std::size_t count{};
            for (const auto& item : records) count += item.second->owners.size();
            if (count >= kOwnerLimit) throw std::runtime_error("workspace owner limit reached (32)");
            vm->owners.push_back(owner);
        }
        const std::string id = std::to_string(owner->id);
        {
            StackGuard restore(lua);
            lua.field(absolute_plugin, "Unloading"); const int signal = lua.top();
            lua.closure(&plugin_unloading, "Workspace.Plugin.Unloading", id);
            lua.method(signal, "Connect", {lua.top()});
            owner->unloading_connection = lua.reference(-1);
        }
        try {
            StackGuard restore(lua);
            lua.field(kGlobalsIndex, "game"); const int game = lua.top();
            lua.push_string("RunService");
            lua.method(game, "GetService", {lua.top()}); const int service = lua.top();
            lua.field(service, "Heartbeat"); const int signal = lua.top();
            lua.closure(&heartbeat, "Workspace.Heartbeat", id);
            lua.method(signal, "Connect", {lua.top()});
            owner->heartbeat_connection = lua.reference(-1);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[workspace] Heartbeat fallback unavailable: %s\n", error.what());
        }
        schedule_poll(lua, owner->id);
    } catch (...) {
        if (find_owner(vm, owner->id)) drop_owner(lua, vm, owner->id);
        else safe_unref(lua, owner->plugin_reference);
        throw;
    }
    return owner;
}

double number_field(const Lua& lua, int object, const char* key) {
    StackGuard restore(lua); lua.field(object, key);
    if (lua.api.type(lua.state, -1) != kEngineNumber)
        throw std::runtime_error(std::string("Size.") + key + " must be an ordinary number");
    int valid{}; const double value = lua.api.tonumberx(lua.state, -1, &valid);
    if (!valid || !std::isfinite(value)) throw std::runtime_error("split size must be finite");
    return value;
}
std::shared_ptr<const SplitNode> read_split(const Lua& lua, int index, unsigned depth = 0) {
    if (depth > kSplitDepthLimit) throw std::runtime_error("split tree exceeds maximum depth 8");
    const int value = lua.absolute(index);
    auto node = std::make_shared<SplitNode>();
    if (lua.api.type(lua.state, value) == kEngineString) {
        node->panel_id = lua.string(value, "panel ID"); return node;
    }
    if (lua.api.type(lua.state, value) != kEngineTable)
        throw std::runtime_error("split leaf must be a panel ID; a split must be a table");
    {
        StackGuard restore(lua); lua.field(value, "Direction");
        const auto direction = lua.string(-1, "split Direction", 16);
        if (direction == "Horizontal") node->direction = Direction::Horizontal;
        else if (direction == "Vertical") node->direction = Direction::Vertical;
        else throw std::runtime_error("split Direction must be Horizontal or Vertical");
    }
    {
        StackGuard restore(lua); lua.field(value, "Size"); const int size = lua.top();
        const int type = lua.api.type(lua.state, size);
        if (type != kEngineTable && type != kEngineUserdata)
            throw std::runtime_error("split Size must be a UDim or {Scale, Offset} table");
        node->size.scale = number_field(lua, size, "Scale");
        const double offset = number_field(lua, size, "Offset");
        if (offset < std::numeric_limits<int>::min() || offset > std::numeric_limits<int>::max() || std::trunc(offset) != offset)
            throw std::runtime_error("split Offset must fit a signed integer");
        if (node->size.scale < 0 || node->size.scale > 1)
            throw std::runtime_error("split Scale must be between 0 and 1");
        node->size.offset = static_cast<int>(offset);
    }
    { StackGuard restore(lua); lua.field(value, "First"); node->first = read_split(lua, -1, depth + 1); }
    { StackGuard restore(lua); lua.field(value, "Second"); node->second = read_split(lua, -1, depth + 1); }
    return node;
}

int callback_error(const Lua& lua, int before, const char* message) noexcept {
    lua.reset(before);
    try {
        Operation op{}; op.kind = OpKind::Error; op.text = message;
        op.length = std::char_traits<char>::length(message); lua.run(op); return 2;
    } catch (...) { lua.reset(before); return 0; }
}
int signal_connect(lua_State* state) {
    const auto* api = engine.load(std::memory_order_acquire);
    if (!active.load(std::memory_order_acquire) || !api) return 0;
    Lua lua(state, *api); const int before = lua.top();
    try {
        if (before != 2 || api->type(state, 1) != kEngineTable || api->type(state, 2) != kEngineFunction)
            throw std::runtime_error("signal:Connect expects self and one nonyielding function");
        const auto view_id = lua.handle_id(1);
        std::string kind;
        { StackGuard restore(lua); lua.field(1, "Kind"); kind = lua.string(-1, "signal Kind", 16); }
        if (kind != "Entered" && kind != "Exited") throw std::runtime_error("invalid workspace signal Kind");
        const auto view = get_view(view_id);
        const auto view_state = get_state(view.id);
        auto vm = vm_for(lua, false);
        if (!find_owner(vm, view.owner)) throw std::runtime_error("view owner belongs to a different or released VM");
        if (view_state.state == "unregistered") throw std::runtime_error("view is unregistered");
        Listener listener;
        listener.id = "WorkspaceConnection" + std::to_string(listener_serial.fetch_add(1, std::memory_order_relaxed));
        listener.owner = view.owner; listener.view_id = view.id;
        listener.kind = kind == "Entered" ? EventKind::Entered : EventKind::Exited;
        listener.reference = lua.reference(2);
        bool inserted{};
        try {
            { std::lock_guard lock(records_mutex);
              std::size_t count{};
              for (const auto& record : records) count += record.second->listeners.size();
              if (count >= kListenerLimit) throw std::runtime_error("workspace listener limit reached (128)");
              vm->listeners.push_back(listener); inserted = true; }
            Operation op{}; op.kind = OpKind::Connection; op.text = listener.id.data(); op.length = listener.id.size(); lua.run(op);
        } catch (...) {
            if (inserted) {
                std::lock_guard lock(records_mutex);
                std::erase_if(vm->listeners, [&](const auto& entry) { return entry.id == listener.id; });
            }
            safe_unref(lua, listener.reference); throw;
        }
        return 1;
    } catch (const std::exception& error) { return callback_error(lua, before, error.what()); }
      catch (...) { return callback_error(lua, before, "workspace signal connection failed"); }
}
int connection_disconnect(lua_State* state) {
    const auto* api = engine.load(std::memory_order_acquire);
    if (!active.load(std::memory_order_acquire) || !api) return 0;
    Lua lua(state, *api); const int before = lua.top();
    try {
        if (before != 1) throw std::runtime_error("connection:Disconnect expects only self");
        const auto id = lua.handle_id(1); auto vm = vm_for(lua, false);
        int reference{};
        if (vm) {
            std::lock_guard lock(records_mutex);
            const auto found = std::find_if(vm->listeners.begin(), vm->listeners.end(),
                [&](const auto& listener) { return listener.id == id; });
            if (found != vm->listeners.end()) { reference = found->reference; vm->listeners.erase(found); }
        }
        safe_unref(lua, reference); return 0;
    } catch (const std::exception& error) { return callback_error(lua, before, error.what()); }
      catch (...) { return callback_error(lua, before, "workspace disconnect failed"); }
}

void remove_unregistered_listeners(const Lua& lua, const std::shared_ptr<VmRecord>& vm) {
    std::vector<Listener> snapshot;
    { std::lock_guard lock(records_mutex); snapshot = vm->listeners; }
    for (const auto& listener : snapshot) {
        if (get_state(listener.view_id).state != "unregistered") continue;
        int reference{};
        { std::lock_guard lock(records_mutex);
          const auto found = std::find_if(vm->listeners.begin(), vm->listeners.end(),
              [&](const auto& value) { return value.id == listener.id; });
          if (found != vm->listeners.end()) { reference = found->reference; vm->listeners.erase(found); } }
        safe_unref(lua, reference);
    }
}
int heartbeat(lua_State* state) {
    const auto* api = engine.load(std::memory_order_acquire);
    if (!active.load(std::memory_order_acquire) || !api) return 0;
    Lua lua(state, *api); StackGuard stack_restore(lua);
    try {
        auto vm = vm_for(lua, false);
        if (!vm || !find_owner(vm, callback_owner(lua))) return 0;
        std::vector<OwnerId> owners;
        {
            std::lock_guard lock(records_mutex);
            if (vm->delivering) return 0;
            for (const auto& owner : vm->owners) owners.push_back(owner->id);
            vm->delivering = true;
        }
        struct DeliveryGuard {
            std::shared_ptr<VmRecord> vm;
            ~DeliveryGuard() { std::lock_guard lock(records_mutex); vm->delivering = false; }
        } delivery_guard{vm};
        const auto deliveries = take_events(owners);
        struct AcknowledgeGuard {
            const std::vector<Delivery>& values;
            ~AcknowledgeGuard() {
                try { complete_events(values); }
                catch (...) { std::fprintf(stderr, "[workspace] event acknowledgement failed\n"); }
            }
        } acknowledge{deliveries};
        for (const auto& delivery : deliveries) {
            std::vector<std::string> ids;
            { std::lock_guard lock(records_mutex);
              for (const auto& listener : vm->listeners)
                  if (listener.owner == delivery.owner && listener.view_id == delivery.event.view_id && listener.kind == delivery.event.kind)
                      ids.push_back(listener.id); }
            for (const auto& id : ids) {
                int reference{};
                { std::lock_guard lock(records_mutex);
                  const auto found = std::find_if(vm->listeners.begin(), vm->listeners.end(), [&](const auto& entry) { return entry.id == id; });
                  if (found != vm->listeners.end()) reference = found->reference; }
                if (!reference) continue; // Another callback may have disconnected it.
                try {
                    StackGuard restore(lua);
                    lua.get_reference(reference); const int function = lua.top();
                    if (api->type(state, function) != kEngineFunction) throw std::runtime_error("saved listener is no longer a function");
                    Operation op{}; op.kind = OpKind::Event; op.delivery = &delivery; lua.run(op);
                    lua.invoke(function, {lua.top()}, 0); // Plain pcall: yielding is intentionally unsupported.
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "[workspace] listener %s failed: %s\n", id.c_str(), error.what());
                } catch (...) { std::fprintf(stderr, "[workspace] listener failed\n"); }
            }
        }
        remove_unregistered_listeners(lua, vm);
    } catch (const std::exception& error) { std::fprintf(stderr, "[workspace] Heartbeat delivery failed: %s\n", error.what()); }
      catch (...) { std::fprintf(stderr, "[workspace] Heartbeat delivery failed\n"); }
    return 0;
}
int plugin_unloading(lua_State* state) {
    const auto* api = engine.load(std::memory_order_acquire);
    if (!active.load(std::memory_order_acquire) || !api) return 0;
    Lua lua(state, *api); StackGuard restore(lua);
    try { auto vm = vm_for(lua, false); if (vm) drop_owner(lua, vm, callback_owner(lua)); }
    catch (const std::exception& error) { std::fprintf(stderr, "[workspace] plugin unload cleanup failed: %s\n", error.what()); }
    catch (...) { std::fprintf(stderr, "[workspace] plugin unload cleanup failed\n"); }
    return 0;
}
int delayed_poll(lua_State* state) {
    const auto* api = engine.load(std::memory_order_acquire);
    if (!active.load(std::memory_order_acquire) || !api) return 0;
    Lua lua(state, *api); StackGuard restore(lua);
    try {
        const OwnerId owner = callback_owner(lua);
        auto vm = vm_for(lua, false);
        if (!owner || !find_owner(vm, owner)) return 0;
        heartbeat(state);
        if (active.load(std::memory_order_acquire) && find_owner(vm, owner)) schedule_poll(lua, owner);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[workspace] edit-mode poll stopped: %s\n", error.what());
    } catch (...) { std::fprintf(stderr, "[workspace] edit-mode poll stopped\n"); }
    return 0;
}

enum class Export {
    CreatePanel, RegisterPanel, GetBuiltinPanel, OpenPanel, ClosePanel, RegisterView, GetView,
    EnterView, ExitView, UnregisterView, GetState, GetViewStack, GetRequest,
    InspectPanels, RecoverLayout,
};
struct ExportDefinition { const char* name; Export function; };
constexpr ExportDefinition exports[] = {
    {"CreatePanel", Export::CreatePanel}, {"RegisterPanel", Export::RegisterPanel},
    {"GetBuiltinPanel", Export::GetBuiltinPanel},
    {"OpenPanel", Export::OpenPanel}, {"ClosePanel", Export::ClosePanel},
    {"RegisterView", Export::RegisterView}, {"GetView", Export::GetView},
    {"EnterView", Export::EnterView}, {"ExitView", Export::ExitView},
    {"UnregisterView", Export::UnregisterView}, {"GetState", Export::GetState},
    {"GetViewStack", Export::GetViewStack}, {"GetRequest", Export::GetRequest},
    {"InspectPanels", Export::InspectPanels}, {"RecoverLayout", Export::RecoverLayout},
};
void arguments(const Lua& lua, int count, const char* name) {
    if (lua.top() != count) throw std::runtime_error(std::string(name) + " received the wrong number of arguments");
}
int invoke_export(Export function, const Lua& lua) {
    switch (function) {
    case Export::CreatePanel: {
        arguments(lua, 3, "CreatePanel");
        // Both public constructors yield on the pinned Studio build. Invoking
        // either beneath plain pcall can schedule a later resume after this C
        // frame is gone, so reject before calling any engine constructor.
        throw std::runtime_error("CreatePanel is unsupported on this Studio build; create the widget with plugin:CreateDockWidgetPluginGuiAsync(pluginGuiId, widgetInfo), then call RegisterPanel(plugin, pluginGuiId, widget)");
    }
    case Export::RegisterPanel: {
        arguments(lua, 3, "RegisterPanel");
        const auto local_id = lua.string(2, "pluginGuiId", 128);
        if (lua.api.type(lua.state, 3) != kEngineUserdata)
            throw std::runtime_error("widget must be an existing DockWidgetPluginGui");
        {
            StackGuard restore(lua);
            lua.push_string("DockWidgetPluginGui");
            lua.method(3, "IsA", {lua.top()});
            if (lua.api.type(lua.state, -1) != kEngineBoolean || !lua.api.toboolean(lua.state, -1))
                throw std::runtime_error("widget must be an existing DockWidgetPluginGui");
        }
        // A widget may have only one workspace identity in its VM, even if a
        // caller supplies another Plugin object or another creation ID.
        const auto vm = vm_for(lua, false);
        std::vector<int> existing_widgets;
        if (vm) {
            std::lock_guard lock(records_mutex);
            for (const auto& owner : vm->owners) for (const auto& panel : owner->panels)
                existing_widgets.push_back(panel.reference);
        }
        for (const int reference : existing_widgets) {
            StackGuard restore(lua); lua.get_reference(reference);
            if (lua.api.rawequal(lua.state, 3, -1))
                throw std::runtime_error("widget is already registered with the workspace mod");
        }
        auto owner = owner_for(lua, 1);
        { std::lock_guard lock(records_mutex);
          for (const auto& panel : owner->panels) if (panel.local_id == local_id) throw std::runtime_error("local panel ID is already registered by this plugin"); }
        const int reference = lua.reference(3);
        bool recorded{};
        try {
            // The caller supplies the exact public creation ID. Studio does
            // not expose a getter; native docking resolves this suffix and
            // rejects missing or ambiguous matches when a request is applied.
            const auto panel = associate_panel(owner->id, local_id);
            { std::lock_guard lock(records_mutex); owner->panels.push_back({local_id, panel.id, reference}); recorded = true; }
            lua.push_string(panel.id);
        } catch (...) {
            // An association already recorded by the host must keep its live
            // reference until owner cleanup, even if constructing the return
            // value subsequently fails. The ordinary plugin owns its widget.
            if (!recorded) safe_unref(lua, reference);
            throw;
        }
        return 1;
    }
    case Export::GetBuiltinPanel:
        arguments(lua, 1, "GetBuiltinPanel"); lua.push_string(builtin_panel(lua.string(1, "built-in panel name", 128))); return 1;
    case Export::OpenPanel: {
        arguments(lua, 1, "OpenPanel"); const auto id = lua.handle_id(1);
        auto vm = vm_for(lua, false); int reference{};
        if (vm) {
            std::lock_guard lock(records_mutex);
            for (const auto& owner : vm->owners) for (const auto& panel : owner->panels)
                if (panel.id == id) reference = panel.reference;
        }
        if (reference) {
            StackGuard restore(lua); lua.get_reference(reference); const int widget = lua.top();
            lua.push_boolean(true); lua.set_field(widget, "Enabled", lua.top());
        }
        const auto request = open_panel(id); Operation op{}; op.kind = OpKind::Request; op.request = &request; lua.run(op); return 1;
    }
    case Export::ClosePanel: {
        arguments(lua, 1, "ClosePanel"); const auto request = close_panel(lua.handle_id(1));
        Operation op{}; op.kind = OpKind::Request; op.request = &request; lua.run(op); return 1;
    }
    case Export::RegisterView: {
        arguments(lua, 3, "RegisterView"); const auto name = lua.string(2, "view name", 128);
        const auto layout = read_split(lua, 3); const auto owner = owner_for(lua, 1);
        const auto view = register_view(owner->id, name, layout);
        Operation op{}; op.kind = OpKind::View; op.view = &view; lua.run(op); return 1;
    }
    case Export::GetView: {
        arguments(lua, 1, "GetView"); const auto view = get_view(lua.string(1, "view name or ID"));
        Operation op{}; op.kind = OpKind::View; op.view = &view; lua.run(op); return 1;
    }
    case Export::EnterView: {
        arguments(lua, 1, "EnterView"); const auto request = enter_view(lua.handle_id(1));
        Operation op{}; op.kind = OpKind::Request; op.request = &request; lua.run(op); return 1;
    }
    case Export::ExitView: {
        if (lua.top() > 1) throw std::runtime_error("ExitView expects zero or one view argument");
        std::string id;
        if (lua.top()) id = lua.handle_id(1);
        else { const auto stack = get_stack(); if (stack.empty()) throw std::runtime_error("there is no active view"); id = stack.back().id; }
        const auto request = exit_view(id); Operation op{}; op.kind = OpKind::Request; op.request = &request; lua.run(op); return 1;
    }
    case Export::UnregisterView: {
        arguments(lua, 1, "UnregisterView"); const auto request = unregister_view(lua.handle_id(1));
        Operation op{}; op.kind = OpKind::Request; op.request = &request; lua.run(op); return 1;
    }
    case Export::GetState: {
        arguments(lua, 1, "GetState"); const auto state = get_state(lua.handle_id(1));
        Operation op{}; op.kind = OpKind::State; op.state = &state; lua.run(op); return 1;
    }
    case Export::GetViewStack: {
        arguments(lua, 0, "GetViewStack"); const auto stack = get_stack();
        Operation op{}; op.kind = OpKind::Stack; op.stack = &stack; lua.run(op); return 1;
    }
    case Export::GetRequest: {
        arguments(lua, 1, "GetRequest"); const auto request = get_request(lua.handle_id(1, "id"));
        Operation op{}; op.kind = OpKind::Request; op.request = &request; lua.run(op); return 1;
    }
    case Export::InspectPanels: {
        arguments(lua, 0, "InspectPanels"); const auto request = inspect_panels(); const auto panels = latest_panels();
        Operation op{}; op.kind = OpKind::Inspection; op.request = &request; op.panels = &panels; lua.run(op); return 1;
    }
    case Export::RecoverLayout: {
        arguments(lua, 0, "RecoverLayout"); const auto request = recover_layout();
        Operation op{}; op.kind = OpKind::Request; op.request = &request; lua.run(op); return 1;
    }
    }
    throw std::runtime_error("unknown workspace export");
}

RmlNativeApiCallbackStatus export_callback(void* userdata,
    const RmlNativeApiEngineContext* context, int* result_count,
    RmlNativeApiOwnedString* error_message) {
    if (result_count) *result_count = 0;
    if (!context || context->abi_version != 1 || !context->state || !context->engine_api || !userdata || !result_count || !error_message)
        return RML_NATIVE_API_CALLBACK_FAILURE;
    auto* state = static_cast<lua_State*>(context->state);
    const auto* api = static_cast<const EngineApi*>(context->engine_api);
    const int before = api->gettop(state);
    try {
        if (!active.load(std::memory_order_acquire) || !host_active()) throw std::runtime_error("workspace mod is inactive");
        if (!api->rawseti || !api->rawgeti || !api->getfield || !api->setfield || !api->getreadonly || !api->pcall || !api->mainthread || !api->rawequal)
            throw std::runtime_error("workspace requires the complete verified engine API");
        const EngineApi* expected = nullptr;
        if (!engine.compare_exchange_strong(expected, api, std::memory_order_acq_rel) && expected != api)
            throw std::runtime_error("workspace engine context changed");
        const Lua lua(state, *api);
        *result_count = invoke_export(static_cast<const ExportDefinition*>(userdata)->function, lua);
        return RML_NATIVE_API_CALLBACK_SUCCESS;
    } catch (const std::exception& error) {
        api->settop(state, before);
        rml_native_api_owned_string_assign(error_message, error.what(), std::char_traits<char>::length(error.what()));
    } catch (...) {
        api->settop(state, before);
        constexpr char message[] = "workspace native adapter failed";
        rml_native_api_owned_string_assign(error_message, message, sizeof(message) - 1);
    }
    *result_count = 0;
    return RML_NATIVE_API_CALLBACK_FAILURE;
}
} // namespace

void register_lua_exports(RmlNativeApiOwner* owner) {
    if (!owner) throw std::runtime_error("workspace namespace owner is missing");
    if (!reference_owner_token[0]) {
        // A resident provider's nonce distinguishes its registry table from an
        // unrelated mod's occupied key. It contains no native pointer or token
        // from Studio, and is never exposed through the public workspace API.
        std::random_device entropy;
        constexpr char hex[] = "0123456789abcdef";
        for (std::size_t index = 0; index < 16; ++index) {
            const unsigned byte = entropy() & 255;
            reference_owner_token[index * 2] = hex[byte >> 4];
            reference_owner_token[index * 2 + 1] = hex[byte & 15];
        }
    }
    for (const auto& definition : exports) {
        RmlNativeApiExportSlot slot{};
        const auto status = rml_native_api_register_engine_export(owner, definition.name,
            &export_callback, const_cast<ExportDefinition*>(&definition), &slot);
        if (status != RML_NATIVE_API_OK) throw std::runtime_error(std::string("could not register workspace export ") + definition.name);
    }
    active.store(true, std::memory_order_release);
}
void deactivate_lua_exports() {
    // No valid execution state is available on mod/GUI teardown. Resident
    // closures become inert; bounded rooted Lua values survive until VM destruction.
    // Normal plugin unloading releases refs while its matching state is live.
    active.store(false, std::memory_order_release);
    std::vector<OwnerId> owners;
    { std::lock_guard lock(records_mutex);
      for (const auto& entry : records) for (const auto& owner : entry.second->owners) owners.push_back(owner->id);
      records.clear(); }
    for (const auto owner : owners) release_owner(owner);
}
} // namespace rml::workspace
