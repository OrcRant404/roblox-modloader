#pragma once
#include "docking.hpp"
#include "view_stack.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rml::workspace {
using OwnerId = std::uint64_t;
struct PanelRegistration { std::string id; std::string suffix; OwnerId owner{}; };
struct ViewRegistration { std::string id; std::string name; OwnerId owner{}; };
struct RequestInfo {
    std::string id;
    std::string status; // queued, applied, noop, failed, recovery-required
    std::string error;
    std::string report;
};
struct PublicViewState {
    ViewRegistration view;
    std::string state; // registering, inactive, active, suspended, unregistered
    std::string request_id;
    std::string status;
    std::string error;
    bool recovery_required{};
};
struct Delivery { OwnerId owner{}; Event event; std::string request_id; std::uint64_t receipt{}; };

// Lua threads call these copy-only APIs. All docking/controller work is queued
// onto the GUI thread. No Lua state or Qt pointer is stored in this host.
void start_host(const std::filesystem::path& report_directory);
void stop_host();
bool host_active();
PanelRegistration associate_panel(OwnerId owner, std::string suffix);
std::string builtin_panel(std::string_view name);
ViewRegistration register_view(OwnerId owner, std::string name, std::shared_ptr<const SplitNode> layout);
ViewRegistration get_view(std::string_view name_or_id);
PublicViewState get_state(std::string_view id);
std::vector<ViewRegistration> get_stack();
RequestInfo enter_view(std::string_view id);
RequestInfo exit_view(std::string_view id);
RequestInfo unregister_view(std::string_view id);
RequestInfo open_panel(std::string_view id);
RequestInfo close_panel(std::string_view id);
RequestInfo inspect_panels();
RequestInfo recover_layout();
RequestInfo get_request(std::string_view id);
std::vector<PanelInfo> latest_panels();
std::vector<Delivery> take_events(const std::vector<OwnerId>& owners);
void complete_events(const std::vector<Delivery>& events);
// Release owner registrations; remove its pending events without Lua callbacks.
void release_owner(OwnerId owner);
}
