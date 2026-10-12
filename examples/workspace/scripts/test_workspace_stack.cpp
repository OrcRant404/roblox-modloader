#include "../native-workspace/view_stack.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
	using namespace rml::workspace;

	[[noreturn]] void fail(const char* expression, const int line)
	{
		std::cerr << "FAIL line " << line << ": " << expression << '\n';
		std::exit(1);
	}

#define CHECK(expression) ((expression) ? static_cast<void>(0) : fail(#expression, __LINE__))

	struct FakeBackend {
		std::string current{"normal"};
		int capture_count{};
		int restore_count{};
		int apply_count{};
		bool fail_next_capture{};
		bool fail_next_restore{};
		bool fail_next_apply{};

		Backend callbacks()
		{
			return {
				[this](std::string& error) -> std::optional<std::string> {
					++capture_count;
					if (fail_next_capture) {
						fail_next_capture = false;
						error = "capture fault";
						return std::nullopt;
					}
					return current;
				},
				[this](const std::string_view snapshot, std::string& error) {
					++restore_count;
					if (fail_next_restore) {
						fail_next_restore = false;
						error = "restore fault";
						return false;
					}
					current.assign(snapshot);
					return true;
				},
				[this](const std::string_view descriptor, std::string& error) {
					++apply_count;
					current = "partial:" + std::string(descriptor);
					if (fail_next_apply) {
						fail_next_apply = false;
						error = "apply fault";
						return false;
					}
					current = "layout:" + std::string(descriptor);
					return true;
				},
			};
		}
	};

	void register_view(ViewStack& stack, std::string id, std::string name, std::string layout)
	{
		const auto result = stack.register_view({std::move(id), std::move(name), "opaque description", std::move(layout)});
		CHECK(result.status == Status::Ok);
		CHECK(result.events.empty());
	}

	std::vector<std::string> stack_ids(const ViewStack& stack)
	{
		std::vector<std::string> ids;
		for (const auto& entry : stack.get_view_stack())
			ids.push_back(entry.id);
		return ids;
	}

	void expect_event(const TransitionResult& result, const std::size_t index,
		const EventKind kind, const std::string_view id, const EventReason reason)
	{
		CHECK(index < result.events.size());
		CHECK(result.events[index].kind == kind);
		CHECK(result.events[index].view_id == id);
		CHECK(result.events[index].reason == reason);
	}

	void test_registration_and_queries()
	{
		FakeBackend backend;
		ViewStack stack(backend.callbacks());
		CHECK(stack.register_view({"", "invalid", "description", "layout"}).status == Status::InvalidArgument);
		CHECK(stack.register_view({"not valid", "invalid", "description", "layout"}).status == Status::InvalidArgument);
		CHECK(stack.register_view({"bad", "", "description", "layout"}).status == Status::InvalidArgument);
		register_view(stack, "view-1", "probe.A", "A");
		CHECK(stack.register_view({"view-1", "probe.B", "other", "B"}).status == Status::DuplicateId);
		CHECK(stack.register_view({"view-2", "probe.A", "other", "B"}).status == Status::DuplicateName);
		CHECK(stack.get_state("missing") == ViewState::Unregistered);
		const auto view = stack.get_view("view-1");
		CHECK(view.has_value());
		CHECK(view->definition.name == "probe.A");
		CHECK(view->definition.description == "opaque description");
		CHECK(view->state == ViewState::Inactive);
		CHECK(!stack.get_view("missing"));
	}

	void test_enter_exit_and_noop()
	{
		FakeBackend backend;
		ViewStack stack(backend.callbacks());
		register_view(stack, "view-A", "probe.A", "A");
		register_view(stack, "view-B", "probe.B", "B");

		auto result = stack.enter("view-A");
		CHECK(result.status == Status::Ok);
		CHECK(result.events.size() == 1);
		expect_event(result, 0, EventKind::Entered, "view-A", EventReason::Enter);
		CHECK(backend.current == "layout:A");
		CHECK(stack.get_state("view-A") == ViewState::Active);
		CHECK(stack.get_view_stack().size() == 1);

		const auto captures = backend.capture_count;
		const auto applies = backend.apply_count;
		result = stack.enter("view-A");
		CHECK(result.status == Status::NoOp);
		CHECK(result.events.empty());
		CHECK(backend.capture_count == captures && backend.apply_count == applies);

		result = stack.enter("view-B");
		CHECK(result.status == Status::Ok && result.events.size() == 2);
		expect_event(result, 0, EventKind::Exited, "view-A", EventReason::Covered);
		expect_event(result, 1, EventKind::Entered, "view-B", EventReason::Enter);
		CHECK(backend.current == "layout:B");
		CHECK(stack.get_state("view-A") == ViewState::Suspended);
		CHECK(stack.get_state("view-B") == ViewState::Active);
		CHECK(stack.exit("view-A").status == Status::NotActive);
		CHECK(stack.exit("missing").status == Status::NotFound);
		CHECK(stack_ids(stack) == std::vector<std::string>({"view-A", "view-B"}));

		result = stack.exit("view-B");
		CHECK(result.status == Status::Ok && result.events.size() == 2);
		expect_event(result, 0, EventKind::Exited, "view-B", EventReason::Exit);
		expect_event(result, 1, EventKind::Entered, "view-A", EventReason::Resume);
		CHECK(backend.current == "layout:A");
		CHECK(stack.get_state("view-A") == ViewState::Active);
		CHECK(stack.get_state("view-B") == ViewState::Inactive);

		result = stack.exit("view-A");
		CHECK(result.status == Status::Ok && result.events.size() == 1);
		expect_event(result, 0, EventKind::Exited, "view-A", EventReason::Exit);
		CHECK(backend.current == "normal");
		CHECK(stack.get_view_stack().empty());
		CHECK(stack.get_state("view-A") == ViewState::Inactive);
	}

	void test_enter_existing_unwinds_suffix()
	{
		FakeBackend backend;
		ViewStack stack(backend.callbacks());
		register_view(stack, "view-A", "probe.A", "A");
		register_view(stack, "view-B", "probe.B", "B");
		register_view(stack, "view-C", "probe.C", "C");
		CHECK(stack.enter("view-A").status == Status::Ok);
		CHECK(stack.enter("view-B").status == Status::Ok);
		CHECK(stack.enter("view-C").status == Status::Ok);
		CHECK(backend.current == "layout:C");

		const auto result = stack.enter("view-A");
		CHECK(result.status == Status::Ok && result.events.size() == 3);
		expect_event(result, 0, EventKind::Exited, "view-C", EventReason::Exit);
		expect_event(result, 1, EventKind::Exited, "view-B", EventReason::Exit);
		expect_event(result, 2, EventKind::Entered, "view-A", EventReason::Resume);
		CHECK(stack_ids(stack) == std::vector<std::string>({"view-A"}));
		CHECK(backend.current == "layout:A");
		CHECK(stack.get_state("view-B") == ViewState::Inactive);
		CHECK(stack.get_state("view-C") == ViewState::Inactive);
		CHECK(stack.exit("view-A").status == Status::Ok);
		CHECK(backend.current == "normal");
	}

	void test_selecting_suspended_view_unwinds_current()
	{
		FakeBackend backend;
		ViewStack stack(backend.callbacks());
		register_view(stack, "view-A", "probe.A", "A");
		register_view(stack, "view-B", "probe.B", "B");
		CHECK(stack.enter("view-A").status == Status::Ok);
		CHECK(stack.enter("view-B").status == Status::Ok);

		backend.fail_next_restore = true;
		auto result = stack.enter("view-A");
		CHECK(result.status == Status::LayoutApplyFailed);
		CHECK(result.events.empty());
		CHECK(stack_ids(stack) == std::vector<std::string>({"view-A", "view-B"}));
		CHECK(backend.current == "layout:B");

		result = stack.enter("view-A");
		CHECK(result.status == Status::Ok && result.events.size() == 2);
		expect_event(result, 0, EventKind::Exited, "view-B", EventReason::Exit);
		expect_event(result, 1, EventKind::Entered, "view-A", EventReason::Resume);
		CHECK(stack_ids(stack) == std::vector<std::string>({"view-A"}));
		CHECK(backend.current == "layout:A");
		CHECK(stack.exit("view-A").status == Status::Ok);
		CHECK(backend.current == "normal");
	}

	void test_unregister_active_and_suspended()
	{
		FakeBackend backend;
		ViewStack stack(backend.callbacks());
		register_view(stack, "view-A", "probe.A", "A");
		register_view(stack, "view-B", "probe.B", "B");
		CHECK(stack.enter("view-A").status == Status::Ok);
		CHECK(stack.enter("view-B").status == Status::Ok);

		auto result = stack.unregister_view("view-B");
		CHECK(result.status == Status::Ok && result.events.size() == 2);
		expect_event(result, 0, EventKind::Exited, "view-B", EventReason::Unregister);
		expect_event(result, 1, EventKind::Entered, "view-A", EventReason::Resume);
		CHECK(backend.current == "layout:A");
		CHECK(stack.get_state("view-B") == ViewState::Unregistered);
		CHECK(stack.exit("view-A").status == Status::Ok);
		CHECK(backend.current == "normal");

		CHECK(stack.enter("view-A").status == Status::Ok);
		register_view(stack, "view-B", "probe.B", "B");
		CHECK(stack.enter("view-B").status == Status::Ok);
		result = stack.unregister_view("view-A");
		CHECK(result.status == Status::Ok && result.events.empty());
		CHECK(stack_ids(stack) == std::vector<std::string>({"view-B"}));
		CHECK(backend.current == "layout:B");
		CHECK(stack.get_state("view-A") == ViewState::Unregistered);
		result = stack.exit("view-B");
		CHECK(result.status == Status::Ok && result.events.size() == 1);
		CHECK(backend.current == "normal"); // The removed view's layout is not resurrected.
	}

	void test_failed_apply_rollback_and_recovery()
	{
		FakeBackend backend;
		ViewStack stack(backend.callbacks());
		register_view(stack, "view-A", "probe.A", "A");
		register_view(stack, "view-B", "probe.B", "B");
		CHECK(stack.enter("view-A").status == Status::Ok);
		const auto before = stack_ids(stack);

		backend.fail_next_apply = true;
		auto result = stack.enter("view-B");
		CHECK(result.status == Status::LayoutApplyFailed);
		CHECK(result.events.empty());
		CHECK(stack_ids(stack) == before);
		CHECK(stack.get_state("view-A") == ViewState::Active);
		CHECK(stack.get_state("view-B") == ViewState::Inactive);
		CHECK(backend.current == "layout:A");
		CHECK(!stack.last_recovery());

		backend.fail_next_apply = true;
		backend.fail_next_restore = true;
		result = stack.enter("view-B");
		CHECK(result.status == Status::LayoutApplyAndRollbackFailed);
		CHECK(result.events.empty());
		CHECK(result.recovery.has_value());
		CHECK(stack.last_recovery().has_value());
		CHECK(result.recovery->layout_snapshot == "layout:A");
		CHECK(stack_ids(stack) == before);
		CHECK(stack.get_state("view-A") == ViewState::Active);
		CHECK(stack.enter("view-B").status == Status::RecoveryPending);
		CHECK(stack.exit("view-A").status == Status::RecoveryPending);
		CHECK(stack.register_view({"view-C", "probe.C", "description", "C"}).status == Status::RecoveryPending);
		CHECK(stack.unregister_view("view-B").status == Status::RecoveryPending);
		CHECK(backend.current == "partial:B");

		result = stack.recover_layout();
		CHECK(result.status == Status::Ok && result.events.empty());
		CHECK(backend.current == "layout:A");
		CHECK(!stack.last_recovery());
		result = stack.exit("view-A");
		CHECK(result.status == Status::Ok);
		CHECK(backend.current == "normal");
	}

	void test_capture_failure_is_non_mutating()
	{
		FakeBackend backend;
		ViewStack stack(backend.callbacks());
		register_view(stack, "view-A", "probe.A", "A");
		backend.fail_next_capture = true;
		const auto result = stack.enter("view-A");
		CHECK(result.status == Status::BackendError);
		CHECK(result.events.empty());
		CHECK(backend.current == "normal");
		CHECK(stack.get_view_stack().empty());
		CHECK(stack.get_state("view-A") == ViewState::Inactive);
	}
}

int main()
{
	test_registration_and_queries();
	test_enter_exit_and_noop();
	test_enter_existing_unwinds_suffix();
	test_selecting_suspended_view_unwinds_current();
	test_unregister_active_and_suspended();
	test_failed_apply_rollback_and_recovery();
	test_capture_failure_is_non_mutating();
	std::cout << "workspace view stack checks passed\n";
	return 0;
}
