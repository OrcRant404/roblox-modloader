#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rml::workspace {

struct PanelInfo {
	// Exact native ID, suitable for later lookup. No native pointer escapes.
	std::string id;
	bool document{};
	bool closed{};
	bool visible{};
	int x{};
	int y{};
	int width{};
	int height{};
	int minimum_width{};
	int minimum_height{};
};

enum class Direction { Horizontal, Vertical };

struct SizeValue {
	double scale{0.5};
	int offset{};
};

struct SplitNode {
	// A leaf has only panel_id. A split has only first/second, direction/size.
	std::string panel_id;
	Direction direction{Direction::Horizontal};
	SizeValue size;
	std::shared_ptr<const SplitNode> first;
	std::shared_ptr<const SplitNode> second;
};

struct DockingResult {
	std::string report;
	// True after immediate native bounds and split-allocation checks. NativeInspect
	// after Studio's normal layout events can reconfirm the rendered result.
	bool geometry_verified{};
};

struct LayoutSnapshot {
	std::filesystem::path path;
	std::string report;
};

// Every method is synchronous and GUI-thread-only. Guards and native failures
// throw std::runtime_error. Callers serialize requests and own rollback/stack
// policy. This object retains no Qt pointers, native bindings, or panel cache.
class DockingBackend {
public:
	[[nodiscard]] std::vector<PanelInfo> list_panels() const;
	[[nodiscard]] std::string get_builtin_panel(std::string_view name) const;
	// Exact native IDs take precedence. Otherwise accepts one suffix at an '_'
	// boundary (normal plugin widget IDs); ambiguity or absence is an error.
	[[nodiscard]] std::string resolve_panel_id(std::string_view id_or_suffix) const;
	[[nodiscard]] DockingResult open_panel(std::string_view id_or_suffix) const;
	[[nodiscard]] DockingResult close_panel(std::string_view id_or_suffix) const;
	// The directory must equal the guarded workspace mod's report directory.
	// Capture creates a unique filename and never overwrites an existing file.
	[[nodiscard]] LayoutSnapshot capture(const std::filesystem::path& directory) const;
	[[nodiscard]] DockingResult restore(const std::filesystem::path& snapshot) const;
	// A complete view must contain the real Viewport document exactly once.
	// Resolve and size-check the entire tree before any panel changes.
	[[nodiscard]] DockingResult apply(const SplitNode& root) const;
	[[nodiscard]] std::string inspect() const;
};

} // namespace rml::workspace
