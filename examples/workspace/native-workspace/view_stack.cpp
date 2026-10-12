#include "view_stack.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rml::workspace {
namespace {

bool is_ascii_letter(const unsigned char ch) noexcept
{
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

bool is_ascii_digit(const unsigned char ch) noexcept
{
	return ch >= '0' && ch <= '9';
}

bool valid_id(const std::string_view value) noexcept
{
	if (value.empty() || value.size() > 128 || !is_ascii_letter(static_cast<unsigned char>(value.front())))
		return false;
	for (const unsigned char ch : value)
		if (!is_ascii_letter(ch) && !is_ascii_digit(ch) && ch != '_' && ch != '-')
			return false;
	return true;
}

bool valid_name(const std::string_view value) noexcept
{
	if (value.empty() || value.size() > 256)
		return false;
	for (const unsigned char ch : value)
		if (ch < 0x20 || ch == 0x7f)
			return false;
	return true;
}

std::string exception_message(const char* prefix, const std::exception& error)
{
	return std::string(prefix) + error.what();
}

} // namespace

ViewStack::ViewStack(Backend backend) : backend_(std::move(backend))
{
	if (!backend_.capture_layout || !backend_.restore_layout || !backend_.apply_layout)
		throw std::invalid_argument("view stack requires capture, restore, and apply callbacks");
}

std::optional<std::size_t> ViewStack::find_view(const std::string_view id) const noexcept
{
	for (std::size_t index = 0; index < views_.size(); ++index)
		if (views_[index].id == id)
			return index;
	return std::nullopt;
}

std::optional<std::size_t> ViewStack::find_frame(const std::string_view id) const noexcept
{
	for (std::size_t index = 0; index < stack_.size(); ++index)
		if (stack_[index].id == id)
			return index;
	return std::nullopt;
}

ViewState ViewStack::state_for(const std::string_view id) const noexcept
{
	const auto frame = find_frame(id);
	if (!frame)
		return find_view(id) ? ViewState::Inactive : ViewState::Unregistered;
	return *frame + 1 == stack_.size() ? ViewState::Active : ViewState::Suspended;
}

TransitionResult ViewStack::failure(const Status status, std::string message) const
{
	return TransitionResult{status, std::move(message), {}, recovery_};
}

TransitionResult ViewStack::register_view(ViewDefinition definition)
{
	if (recovery_)
		return failure(Status::RecoveryPending, "restore the retained recovery layout before changing registrations");
	if (!valid_id(definition.id) || !valid_name(definition.name))
		return failure(Status::InvalidArgument, "view ID or name is invalid");
	for (const auto& view : views_) {
		if (view.id == definition.id)
			return failure(Status::DuplicateId, "view ID is already registered");
		if (view.name == definition.name)
			return failure(Status::DuplicateName, "view name is already registered");
	}
	views_.push_back(std::move(definition));
	return {};
}

ViewStack::ApplyResult ViewStack::transact(
	const std::function<bool(std::string&)>& operation,
	const std::string_view operation_name)
{
	if (recovery_)
		return {Status::RecoveryPending, "restore the retained recovery layout before another transition", {}, recovery_};

	std::string capture_error;
	std::optional<std::string> before;
	try {
		before = backend_.capture_layout(capture_error);
	} catch (const std::exception& error) {
		return {Status::BackendError, exception_message("capture layout threw: ", error), {}, std::nullopt};
	} catch (...) {
		return {Status::BackendError, "capture layout threw an unknown exception", {}, std::nullopt};
	}
	if (!before)
		return {Status::BackendError,
			capture_error.empty() ? "capture layout failed" : "capture layout failed: " + capture_error,
			{}, std::nullopt};

	std::string transition_error;
	bool applied = false;
	try {
		applied = operation(transition_error);
	} catch (const std::exception& error) {
		transition_error = exception_message("backend operation threw: ", error);
	} catch (...) {
		transition_error = "backend operation threw an unknown exception";
	}
	if (applied) {
		recovery_.reset();
		return {Status::Ok, {}, std::move(*before), std::nullopt};
	}
	if (transition_error.empty())
		transition_error = "backend operation failed";

	std::string rollback_error;
	bool rolled_back = false;
	try {
		rolled_back = backend_.restore_layout(*before, rollback_error);
	} catch (const std::exception& error) {
		rollback_error = exception_message("rollback threw: ", error);
	} catch (...) {
		rollback_error = "rollback threw an unknown exception";
	}
	if (rolled_back) {
		return {Status::LayoutApplyFailed,
			std::string(operation_name) + " failed: " + transition_error + "; previous layout restored",
			std::move(*before), std::nullopt};
	}
	if (rollback_error.empty())
		rollback_error = "rollback failed";
	RecoveryInfo retained{*before, transition_error, rollback_error};
	recovery_ = retained;
	return {Status::LayoutApplyAndRollbackFailed,
		std::string(operation_name) + " failed: " + transition_error + "; rollback failed: " + rollback_error,
		std::move(*before), std::move(retained)};
}

TransitionResult ViewStack::enter(const std::string_view id)
{
	if (recovery_)
		return failure(Status::RecoveryPending, "restore the retained recovery layout before another transition");
	const auto view_index = find_view(id);
	if (!view_index)
		return failure(Status::NotFound, "view is not registered");

	const auto existing_frame = find_frame(id);
	if (existing_frame && *existing_frame + 1 == stack_.size())
		return TransitionResult{Status::NoOp, "view is already active", {}, std::nullopt};

	if (existing_frame) {
		// The next frame saved this view's exact layout when it was covered.
		const std::string target_snapshot = stack_[*existing_frame + 1].layout_before;
		auto applied = transact([&](std::string& error) {
			return backend_.restore_layout(target_snapshot, error);
		}, "resume view");
		if (applied.status != Status::Ok)
			return TransitionResult{applied.status, std::move(applied.message), {}, std::move(applied.recovery)};

		std::vector<std::string> outgoing;
		for (std::size_t index = stack_.size(); index-- > *existing_frame + 1;)
			outgoing.push_back(stack_[index].id);
		stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(*existing_frame + 1), stack_.end());

		TransitionResult result;
		result.events.reserve(outgoing.size() + 1);
		for (auto& outgoing_id : outgoing)
			result.events.push_back({EventKind::Exited, std::move(outgoing_id), EventReason::Exit});
		result.events.push_back({EventKind::Entered, std::string(id), EventReason::Resume});
		return result;
	}

	const std::string layout = views_[*view_index].layout;
	auto applied = transact([&](std::string& error) {
		return backend_.apply_layout(layout, error);
	}, "enter view");
	if (applied.status != Status::Ok)
		return TransitionResult{applied.status, std::move(applied.message), {}, std::move(applied.recovery)};

	if (stack_.empty())
		normal_baseline_ = applied.snapshot;
	stack_.push_back({std::string(id), std::move(applied.snapshot)});
	TransitionResult result;
	if (stack_.size() > 1)
		result.events.push_back({EventKind::Exited, stack_[stack_.size() - 2].id, EventReason::Covered});
	result.events.push_back({EventKind::Entered, std::string(id), EventReason::Enter});
	return result;
}

TransitionResult ViewStack::exit(const std::string_view id)
{
	if (recovery_)
		return failure(Status::RecoveryPending, "restore the retained recovery layout before another transition");
	if (!find_view(id))
		return failure(Status::NotFound, "view is not registered");
	if (stack_.empty() || stack_.back().id != id)
		return failure(Status::NotActive, "only the active view can exit");

	const std::string target_snapshot = stack_.size() == 1 && normal_baseline_
		? *normal_baseline_ : stack_.back().layout_before;
	auto applied = transact([&](std::string& error) {
		return backend_.restore_layout(target_snapshot, error);
	}, "exit view");
	if (applied.status != Status::Ok)
		return TransitionResult{applied.status, std::move(applied.message), {}, std::move(applied.recovery)};

	const std::string outgoing = stack_.back().id;
	stack_.pop_back();
	if (stack_.empty())
		normal_baseline_.reset();
	TransitionResult result;
	result.events.push_back({EventKind::Exited, outgoing, EventReason::Exit});
	if (!stack_.empty())
		result.events.push_back({EventKind::Entered, stack_.back().id, EventReason::Resume});
	return result;
}

TransitionResult ViewStack::unregister_view(const std::string_view id)
{
	if (recovery_)
		return failure(Status::RecoveryPending, "restore the retained recovery layout before changing registrations");
	const auto view_index = find_view(id);
	if (!view_index)
		return failure(Status::NotFound, "view is not registered");
	const auto frame_index = find_frame(id);
	if (!frame_index) {
		views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(*view_index));
		return {};
	}

	if (*frame_index + 1 != stack_.size()) {
		// The next frame used to resume this view. Bypass it to the removed
		// frame's own predecessor so later exits cannot resurrect an unregistered view.
		stack_[*frame_index + 1].layout_before = stack_[*frame_index].layout_before;
		stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(*frame_index));
		views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(*view_index));
		return {};
	}

	const std::string target_snapshot = stack_.size() == 1 && normal_baseline_
		? *normal_baseline_ : stack_.back().layout_before;
	auto applied = transact([&](std::string& error) {
		return backend_.restore_layout(target_snapshot, error);
	}, "unregister active view");
	if (applied.status != Status::Ok)
		return TransitionResult{applied.status, std::move(applied.message), {}, std::move(applied.recovery)};

	const std::string outgoing = stack_.back().id;
	stack_.pop_back();
	views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(*view_index));
	if (stack_.empty())
		normal_baseline_.reset();
	TransitionResult result;
	result.events.push_back({EventKind::Exited, outgoing, EventReason::Unregister});
	if (!stack_.empty())
		result.events.push_back({EventKind::Entered, stack_.back().id, EventReason::Resume});
	return result;
}

TransitionResult ViewStack::recover_layout()
{
	if (!recovery_)
		return TransitionResult{Status::NoOp, "no layout recovery is pending", {}, std::nullopt};
	std::string error;
	bool restored = false;
	try {
		restored = backend_.restore_layout(recovery_->layout_snapshot, error);
	} catch (const std::exception& exception) {
		error = exception_message("recovery threw: ", exception);
	} catch (...) {
		error = "recovery threw an unknown exception";
	}
	if (!restored) {
		if (error.empty()) error = "recovery restore failed";
		return failure(Status::RecoveryFailed, "layout recovery failed: " + error);
	}
	recovery_.reset();
	return {};
}

std::optional<ViewInfo> ViewStack::get_view(const std::string_view id) const
{
	const auto index = find_view(id);
	if (!index)
		return std::nullopt;
	return ViewInfo{views_[*index], state_for(id)};
}

ViewState ViewStack::get_state(const std::string_view id) const noexcept
{
	return state_for(id);
}

std::vector<StackEntry> ViewStack::get_view_stack() const
{
	std::vector<StackEntry> result;
	result.reserve(stack_.size());
	for (std::size_t index = 0; index < stack_.size(); ++index)
		result.push_back({stack_[index].id,
			index + 1 == stack_.size() ? ViewState::Active : ViewState::Suspended});
	return result;
}

std::optional<RecoveryInfo> ViewStack::last_recovery() const
{
	return recovery_;
}

const char* to_string(const ViewState value) noexcept
{
	switch (value) {
	case ViewState::Active: return "active";
	case ViewState::Suspended: return "suspended";
	case ViewState::Inactive: return "inactive";
	case ViewState::Unregistered: return "unregistered";
	}
	return "unknown";
}

const char* to_string(const EventKind value) noexcept
{
	switch (value) {
	case EventKind::Exited: return "exited";
	case EventKind::Entered: return "entered";
	}
	return "unknown";
}

const char* to_string(const EventReason value) noexcept
{
	switch (value) {
	case EventReason::Enter: return "enter";
	case EventReason::Resume: return "resume";
	case EventReason::Covered: return "covered";
	case EventReason::Exit: return "exit";
	case EventReason::Unregister: return "unregister";
	}
	return "unknown";
}

const char* to_string(const Status value) noexcept
{
	switch (value) {
	case Status::Ok: return "ok";
	case Status::NoOp: return "no-op";
	case Status::InvalidArgument: return "invalid-argument";
	case Status::DuplicateId: return "duplicate-id";
	case Status::DuplicateName: return "duplicate-name";
	case Status::NotFound: return "not-found";
	case Status::NotActive: return "not-active";
	case Status::RecoveryPending: return "recovery-pending";
	case Status::BackendError: return "backend-error";
	case Status::LayoutApplyFailed: return "layout-apply-failed";
	case Status::LayoutApplyAndRollbackFailed: return "layout-apply-and-rollback-failed";
	case Status::RecoveryFailed: return "recovery-failed";
	}
	return "unknown";
}

} // namespace rml::workspace
