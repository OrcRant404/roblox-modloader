// Standalone host lifecycle test. It uses a queued fake GUI dispatcher and a
// stub DockingBackend; it does not load native Studio or the RML library.
#include "../native-workspace/workspace_host.hpp"
#include <RobloxModLoader/qt/qt_integration.hpp>

#include <algorithm>
#include <deque>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
using namespace rml::workspace;

[[noreturn]] void fail(const char* expression, const int line)
{
	std::cerr << "FAIL line " << line << ": " << expression << '\n';
	std::exit(1);
}

#define CHECK(expression) ((expression) ? static_cast<void>(0) : fail(#expression, __LINE__))

std::deque<std::function<void()>> gui_jobs;
bool fail_next_dispatch{};

struct FakeDocking {
	int opens{};
	int closes{};
	int captures{};
	int restores{};
	int applies{};
	int apply_failures{};
	int restore_failures{};
	std::vector<std::string> opened;
	std::vector<std::string> closed;
} fake_docking;

void run_one_gui_job()
{
	CHECK(!gui_jobs.empty());
	auto job = std::move(gui_jobs.front());
	gui_jobs.pop_front();
	job();
}

void run_gui_jobs()
{
	std::size_t count{};
	while (!gui_jobs.empty()) {
		if (++count > 100) fail("GUI job queue drained", __LINE__);
		run_one_gui_job();
	}
}

void acknowledge(const OwnerId owner)
{
	const auto events = take_events({owner});
	complete_events(events);
}

std::shared_ptr<const SplitNode> viewport_layout()
{
	auto node = std::make_shared<SplitNode>();
	node->panel_id = builtin_panel("Viewport");
	return node;
}

RequestInfo latest_shutdown()
{
	RequestInfo latest;
	for (unsigned index = 1; index < 1000; ++index) {
		try {
			auto request = get_request("shutdown-" + std::to_string(index));
			latest = std::move(request);
		} catch (const std::runtime_error&) {
		}
	}
	CHECK(!latest.id.empty());
	return latest;
}

void test_pending_registrations_survive_other_view_publication()
{
	constexpr OwnerId owner_b = 81;
	constexpr OwnerId owner_a = 82;
	const auto view_b = register_view(owner_b, "probe.register-B", viewport_layout());
	const auto view_a = register_view(owner_a, "probe.register-A", viewport_layout());
	const auto request_a = get_state(view_a.id).request_id;
	CHECK(get_state(view_a.id).state == "registering");

	// B's registration publishes before A reaches the controller. A must stay
	// discoverable for signal:Connect and must not lose existing listeners.
	run_one_gui_job();
	CHECK(get_state(view_b.id).state == "inactive");
	CHECK(get_state(view_a.id).state == "registering");
	CHECK(get_state(view_a.id).request_id == request_a);
	CHECK(get_request(request_a).status == "queued");
	CHECK(get_view(view_a.id).id == view_a.id);
	CHECK(get_view(view_a.name).id == view_a.id);

	run_gui_jobs();
	CHECK(get_state(view_a.id).state == "inactive");
	CHECK(get_request(request_a).status == "applied");
	release_owner(owner_a);
	release_owner(owner_b);
	run_gui_jobs();
}

void test_registration_failure_resolves_pending_state()
{
	constexpr OwnerId owner = 91;
	// Host insertion accepts this name; the controller correctly rejects its
	// control character. A terminal failure must not leave it registering.
	const auto invalid = register_view(owner, "probe.invalid\nname", viewport_layout());
	const auto request = get_state(invalid.id).request_id;
	CHECK(get_state(invalid.id).state == "registering");
	run_gui_jobs();
	CHECK(get_state(invalid.id).state == "unregistered");
	CHECK(get_request(request).status == "failed");
	CHECK(!get_request(request).error.empty());
	bool rejected{};
	try { (void)get_view(invalid.id); }
	catch (const std::runtime_error&) { rejected = true; }
	CHECK(rejected);

	// Failure to dispatch the registration removes the staged record/name so
	// a later attempt can register that same name normally.
	fail_next_dispatch = true;
	bool dispatch_failed{};
	try { (void)register_view(owner, "probe.register-retry", viewport_layout()); }
	catch (const std::runtime_error&) { dispatch_failed = true; }
	CHECK(dispatch_failed);
	const auto retry = register_view(owner, "probe.register-retry", viewport_layout());
	run_gui_jobs();
	CHECK(get_state(retry.id).state == "inactive");
	release_owner(owner);
	run_gui_jobs();
}

void test_panel_jobs_follow_owner_lifetime()
{
	constexpr OwnerId owner = 101;
	const auto panel = associate_panel(owner, "Panel");
	const auto open = open_panel(panel.id);
	const auto close = close_panel(panel.id);

	bool dispatch_threw{};
	try {
		release_owner(owner);
	} catch (const std::runtime_error&) {
		dispatch_threw = true;
	}
	CHECK(!dispatch_threw);
	CHECK(get_request(open.id).status == "cancelled");
	CHECK(get_request(close.id).status == "cancelled");
	run_gui_jobs();
	CHECK(fake_docking.opens == 0);
	CHECK(fake_docking.closes == 0);

	bool open_rejected{};
	try { (void)open_panel(panel.id); }
	catch (const std::runtime_error&) { open_rejected = true; }
	CHECK(open_rejected);
	bool close_rejected{};
	try { (void)close_panel(panel.id); }
	catch (const std::runtime_error&) { close_rejected = true; }
	CHECK(close_rejected);
}

void test_layout_backend_reports_are_attached_to_requests()
{
	constexpr OwnerId owner = 151;
	const auto view = register_view(owner, "probe.request-reports", viewport_layout());
	run_gui_jobs();

	const auto enter = enter_view(view.id);
	run_gui_jobs();
	const auto enter_report = get_request(enter.id).report;
	CHECK(enter_report.find("fake_capture=verified") != std::string::npos);
	CHECK(enter_report.find("fake_apply=geometry_verified") != std::string::npos);
	acknowledge(owner);

	const auto exit = exit_view(view.id);
	run_gui_jobs();
	const auto exit_report = get_request(exit.id).report;
	CHECK(exit_report.find("fake_capture=verified") != std::string::npos);
	CHECK(exit_report.find("fake_restore=applied") != std::string::npos);
	acknowledge(owner);

	// Force a retained recovery snapshot, then verify the successful recovery
	// restore report is available from the RecoverLayout request itself.
	fake_docking.apply_failures = 1;
	fake_docking.restore_failures = 1;
	const auto failed_enter = enter_view(view.id);
	run_gui_jobs();
	CHECK(get_request(failed_enter.id).status == "recovery-required");
	const auto recovery = recover_layout();
	run_gui_jobs();
	CHECK(get_request(recovery.id).status == "applied");
	CHECK(get_request(recovery.id).report.find("fake_restore=applied") != std::string::npos);
}

void test_owner_cleanup_survives_dispatch_failure()
{
	constexpr OwnerId owner = 202;
	const auto view = register_view(owner, "probe.cleanup", viewport_layout());
	run_gui_jobs();
	CHECK(get_state(view.id).state == "inactive");
	(void)enter_view(view.id);
	run_gui_jobs();
	CHECK(get_state(view.id).state == "active");
	acknowledge(owner);

	fail_next_dispatch = true;
	bool dispatch_threw{};
	try { release_owner(owner); }
	catch (const std::runtime_error&) { dispatch_threw = true; }
	CHECK(dispatch_threw);
	CHECK(get_stack().size() == 1);
	CHECK(gui_jobs.empty());

	// An empty acknowledgement is the normal heartbeat retry point when no
	// event callback remains for the unloading owner.
	complete_events({});
	CHECK(!gui_jobs.empty());
	run_gui_jobs();
	CHECK(get_stack().empty());
	CHECK(get_state(view.id).state == "unregistered");
	const auto cleanup = get_request(get_state(view.id).request_id);
	CHECK(cleanup.status == "applied");
}

void test_released_view_cleanup_retries_after_recovery()
{
	constexpr OwnerId owner = 252;
	const auto view = register_view(owner, "probe.cleanup-recovery", viewport_layout());
	run_gui_jobs();
	(void)enter_view(view.id);
	run_gui_jobs();
	CHECK(get_state(view.id).state == "active");
	acknowledge(owner);

	// Active unregister needs to restore the baseline. Fail that restore and
	// its rollback so the controller must retain a recovery snapshot.
	fake_docking.restore_failures = 2;
	release_owner(owner);
	run_gui_jobs();
	CHECK(get_stack().size() == 1);
	CHECK(get_state(view.id).state == "active");
	CHECK(get_state(view.id).recovery_required);
	CHECK(take_events({owner}).empty());

	// RecoverLayout restores the retained snapshot, then retries unregistering
	// released owners on the same GUI thread. No callback is delivered to the
	// released Lua owner, even though unregister commits an Exited event.
	const auto recovery = recover_layout();
	run_gui_jobs();
	CHECK(get_request(recovery.id).status == "applied");
	CHECK(get_stack().empty());
	CHECK(get_state(view.id).state == "unregistered");
	CHECK(!get_state(view.id).recovery_required);
	CHECK(take_events({owner}).empty());
}

void test_released_view_cleanup_retries_after_successful_rollback()
{
	constexpr OwnerId owner = 262;
	const auto view = register_view(owner, "probe.cleanup-rollback", viewport_layout());
	run_gui_jobs();
	(void)enter_view(view.id);
	run_gui_jobs();
	acknowledge(owner);

	// The requested restore fails, but rollback succeeds. There is no retained
	// recovery snapshot; RecoverLayout's no-op result must still retry cleanup.
	fake_docking.restore_failures = 1;
	release_owner(owner);
	run_gui_jobs();
	CHECK(get_stack().size() == 1);
	CHECK(get_state(view.id).state == "active");
	CHECK(!get_state(view.id).recovery_required);
	CHECK(get_request(get_state(view.id).request_id).status == "failed");

	const auto retry = recover_layout();
	run_gui_jobs();
	CHECK(get_request(retry.id).status == "applied");
	CHECK(get_stack().empty());
	CHECK(get_state(view.id).state == "unregistered");
	CHECK(take_events({owner}).empty());
}

void test_multiple_released_views_are_removed_suspended_first()
{
	constexpr OwnerId owner_a = 272;
	constexpr OwnerId owner_b = 273;
	const auto view_a = register_view(owner_a, "probe.cleanup-stack-A", viewport_layout());
	run_gui_jobs();
	(void)enter_view(view_a.id);
	run_gui_jobs();
	acknowledge(owner_a);

	const auto view_b = register_view(owner_b, "probe.cleanup-stack-B", viewport_layout());
	run_gui_jobs();
	(void)enter_view(view_b.id);
	run_gui_jobs();
	acknowledge(owner_a);
	acknowledge(owner_b);
	CHECK(get_stack().size() == 2);

	const int restores_before = fake_docking.restores;
	// Both owners are released before the first GUI cleanup runs. Removing A
	// while suspended must splice its predecessor snapshot into B, so removing
	// B restores the normal layout directly instead of briefly restoring A.
	release_owner(owner_a);
	release_owner(owner_b);
	run_gui_jobs();
	CHECK(fake_docking.restores == restores_before + 1);
	CHECK(get_stack().empty());
	CHECK(get_state(view_a.id).state == "unregistered");
	CHECK(get_state(view_b.id).state == "unregistered");
	CHECK(take_events({owner_a}).empty());
	CHECK(take_events({owner_b}).empty());
}

void test_shutdown_reports_dispatch_and_restore_failures()
{
	constexpr OwnerId owner = 303;
	const auto view = register_view(owner, "probe.shutdown", viewport_layout());
	run_gui_jobs();
	(void)enter_view(view.id);
	run_gui_jobs();
	acknowledge(owner);

	// A dispatcher error must produce a failed request and permit a later stop.
	fail_next_dispatch = true;
	bool dispatch_threw{};
	try { stop_host(); }
	catch (const std::runtime_error&) { dispatch_threw = true; }
	CHECK(dispatch_threw);
	CHECK(latest_shutdown().status == "failed");
	CHECK(host_active());
	CHECK(gui_jobs.empty());

	// One failed restore followed by a successful rollback is a retryable
	// shutdown failure. It must not claim that a recovery snapshot was retained.
	fake_docking.restore_failures = 1;
	stop_host();
	run_gui_jobs();
	CHECK(latest_shutdown().status == "failed");
	CHECK(host_active());
	CHECK(get_stack().size() == 1);

	// If both the requested restore and rollback fail, report retained recovery
	// and expose that state through the affected view query.
	fake_docking.restore_failures = 2;
	stop_host();
	run_gui_jobs();
	CHECK(latest_shutdown().status == "recovery-required");
	CHECK(host_active());
	CHECK(get_state(view.id).recovery_required);
	const auto recovery = recover_layout();
	run_gui_jobs();
	CHECK(get_request(recovery.id).status == "applied");
	CHECK(!get_state(view.id).recovery_required);

	stop_host();
	run_gui_jobs();
	CHECK(latest_shutdown().status == "applied");
	CHECK(!host_active());
	CHECK(get_stack().empty());
	CHECK(get_state(view.id).state == "inactive");
}
} // namespace

namespace rml::qt {
QtIntegration* QtIntegration::instance()
{
	static QtIntegration integration;
	return &integration;
}

void QtIntegration::run_on_gui_thread(std::function<void()> callback)
{
	if (fail_next_dispatch) {
		fail_next_dispatch = false;
		throw std::runtime_error("fake GUI dispatcher failure");
	}
	gui_jobs.push_back(std::move(callback));
}
} // namespace rml::qt

namespace rml::workspace {
std::vector<PanelInfo> DockingBackend::list_panels() const
{
	return {};
}

std::string DockingBackend::get_builtin_panel(const std::string_view name) const
{
	return std::string(name);
}

std::string DockingBackend::resolve_panel_id(const std::string_view id_or_suffix) const
{
	return std::string(id_or_suffix);
}

DockingResult DockingBackend::open_panel(const std::string_view id_or_suffix) const
{
	++fake_docking.opens;
	fake_docking.opened.emplace_back(id_or_suffix);
	return {"opened", true};
}

DockingResult DockingBackend::close_panel(const std::string_view id_or_suffix) const
{
	++fake_docking.closes;
	fake_docking.closed.emplace_back(id_or_suffix);
	return {"closed", true};
}

LayoutSnapshot DockingBackend::capture(const std::filesystem::path& directory) const
{
	++fake_docking.captures;
	return {directory / ("fake-layout-" + std::to_string(fake_docking.captures)), "fake_capture=verified\n"};
}

DockingResult DockingBackend::restore(const std::filesystem::path&) const
{
	++fake_docking.restores;
	if (fake_docking.restore_failures > 0) {
		--fake_docking.restore_failures;
		throw std::runtime_error("fake restore failure");
	}
	return {"fake_restore=applied\n", true};
}

DockingResult DockingBackend::apply(const SplitNode&) const
{
	++fake_docking.applies;
	if (fake_docking.apply_failures > 0) {
		--fake_docking.apply_failures;
		throw std::runtime_error("fake apply failure");
	}
	return {"fake_apply=geometry_verified\n", true};
}

std::string DockingBackend::inspect() const
{
	return "fake inventory";
}
} // namespace rml::workspace

int main()
{
	const auto reports = std::filesystem::temp_directory_path()
		/ ("rml-workspace-host-test-" + std::to_string(getpid()));
	std::filesystem::remove_all(reports);
	start_host(reports);
	test_pending_registrations_survive_other_view_publication();
	test_registration_failure_resolves_pending_state();
	test_panel_jobs_follow_owner_lifetime();
	test_layout_backend_reports_are_attached_to_requests();
	test_owner_cleanup_survives_dispatch_failure();
	test_released_view_cleanup_retries_after_recovery();
	test_released_view_cleanup_retries_after_successful_rollback();
	test_multiple_released_views_are_removed_suspended_first();
	test_shutdown_reports_dispatch_and_restore_failures();
	std::filesystem::remove_all(reports);
	std::cout << "workspace host tests passed\n";
	return 0;
}
