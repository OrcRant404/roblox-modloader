#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rml::workspace {

enum class ViewState {
	Active,
	Suspended,
	Inactive,
	Unregistered,
};

enum class EventKind {
	Exited,
	Entered,
};

enum class EventReason {
	Enter,
	Resume,
	Covered,
	Exit,
	Unregister,
};

struct ViewDefinition {
	std::string id;
	std::string name;
	std::string description;
	// Opaque to the controller. The backend owns its interpretation.
	std::string layout;
};

struct ViewInfo {
	ViewDefinition definition;
	ViewState state{ViewState::Unregistered};
};

struct StackEntry {
	std::string id;
	ViewState state{ViewState::Unregistered};
};

struct Event {
	EventKind kind{};
	std::string view_id;
	EventReason reason{};
};

enum class Status {
	Ok,
	NoOp,
	InvalidArgument,
	DuplicateId,
	DuplicateName,
	NotFound,
	NotActive,
	RecoveryPending,
	BackendError,
	LayoutApplyFailed,
	LayoutApplyAndRollbackFailed,
	RecoveryFailed,
};

struct RecoveryInfo {
	// A snapshot captured immediately before the failed transition. Applying it
	// returns the backend to the layout represented by the unchanged view stack.
	std::string layout_snapshot;
	std::string transition_error;
	std::string rollback_error;
};

struct TransitionResult {
	Status status{Status::Ok};
	std::string message;
	std::vector<Event> events;
	std::optional<RecoveryInfo> recovery;

	bool succeeded() const noexcept {
		return status == Status::Ok || status == Status::NoOp;
	}
};

struct Backend {
	// Capture the exact current layout as an opaque string snapshot.
	std::function<std::optional<std::string>(std::string& error)> capture_layout;
	// Restore a snapshot previously returned by capture_layout.
	std::function<bool(std::string_view snapshot, std::string& error)> restore_layout;
	// Apply the opaque layout descriptor stored in a ViewDefinition.
	std::function<bool(std::string_view descriptor, std::string& error)> apply_layout;
};

// Synchronous, VM-independent view stack. Callers must serialize access to one
// instance. Backend callbacks run before the stack is committed; lifecycle
// events are returned only after a successful backend change and stack commit.
class ViewStack {
public:
	explicit ViewStack(Backend backend);
	~ViewStack() = default;
	ViewStack(const ViewStack&) = delete;
	ViewStack& operator=(const ViewStack&) = delete;
	ViewStack(ViewStack&&) = delete;
	ViewStack& operator=(ViewStack&&) = delete;

	TransitionResult register_view(ViewDefinition definition);
	TransitionResult unregister_view(std::string_view id);
	TransitionResult enter(std::string_view id);
	TransitionResult exit(std::string_view id);
	TransitionResult recover_layout();

	std::optional<ViewInfo> get_view(std::string_view id) const;
	ViewState get_state(std::string_view id) const noexcept;
	std::vector<StackEntry> get_view_stack() const;
	std::optional<RecoveryInfo> last_recovery() const;

private:
	struct Frame {
		std::string id;
		// Exact layout to restore when this frame stops being active.
		std::string layout_before;
	};

	struct ApplyResult {
		Status status{Status::Ok};
		std::string message;
		std::string snapshot;
		std::optional<RecoveryInfo> recovery;
	};

	Backend backend_;
	std::vector<ViewDefinition> views_;
	std::vector<Frame> stack_;
	// Present only while a stack is active. It is the first layout snapshot
	// captured when an empty stack entered its first view.
	std::optional<std::string> normal_baseline_;
	std::optional<RecoveryInfo> recovery_;

	std::optional<std::size_t> find_view(std::string_view id) const noexcept;
	std::optional<std::size_t> find_frame(std::string_view id) const noexcept;
	ViewState state_for(std::string_view id) const noexcept;
	TransitionResult failure(Status status, std::string message) const;
	ApplyResult transact(const std::function<bool(std::string&)>& operation,
		std::string_view operation_name);
};

const char* to_string(ViewState value) noexcept;
const char* to_string(EventKind value) noexcept;
const char* to_string(EventReason value) noexcept;
const char* to_string(Status value) noexcept;

} // namespace rml::workspace
