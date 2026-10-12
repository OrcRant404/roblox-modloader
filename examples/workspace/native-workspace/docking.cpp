#include "docking.hpp"
#include "layout_snapshot.hpp"

#include <RobloxModLoader/memory/foreign_call.hpp>
#include <RobloxModLoader/qt/qobject.hpp>
#include <RobloxModLoader/qt/qstring.hpp>
#include <RobloxModLoader/qt/qwidget.hpp>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

using Context = rml::workspace::support::GuardedDockingContext;
using Object = rml::qt::QObject;
using Widget = rml::qt::QWidget;
using rml::workspace::Direction;
using rml::workspace::PanelInfo;
using rml::workspace::SplitNode;

constexpr std::string_view kViewportId = "ideDocDocumentPanel";
constexpr std::size_t kMaxLeaves = 128;
constexpr std::size_t kMaxDepth = 64;
constexpr int kMaxSpan = 32768;

// Qt 5.15's public QSize/QPoint value representation is two ints. The pinned
// ARM64 minimumSize/pos/mapTo exports return these trivial values in x0.
// https://code.qt.io/cgit/qt/qtbase.git/tree/src/corelib/tools/qsize.h?h=v5.15.9-lts-lgpl
// https://code.qt.io/cgit/qt/qtbase.git/tree/src/corelib/tools/qpoint.h?h=v5.15.9-lts-lgpl
struct Size { int width; int height; };
struct Point { int x; int y; };
// QRect's four-int public representation stores inclusive edges. The pinned
// ARM64 frameGeometry export returns this trivial value in x0 and x1.
struct Rect { int left; int top; int right; int bottom; };
static_assert(sizeof(Size) == 8 && sizeof(Point) == 8);
static_assert(sizeof(Rect) == 16 && std::is_trivially_copyable_v<Rect> && std::is_standard_layout_v<Rect>);
static_assert(std::is_trivially_copyable_v<Size> && std::is_trivially_copyable_v<Point>);
static_assert(std::is_standard_layout_v<Size> && std::is_standard_layout_v<Point>);

class Library {
public:
	explicit Library(std::filesystem::path path) : path_(std::move(path)) {
		handle_ = dlopen(path_.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
		if (!handle_) throw std::runtime_error("Native docking library is not loaded: " + path_.string());
	}
	~Library() { if (handle_) dlclose(handle_); }
	Library(const Library&) = delete;
	Library& operator=(const Library&) = delete;
	[[nodiscard]] void* resolve(const char* name) {
		void* symbol = dlsym(handle_, name);
		Dl_info info{};
		if (!symbol || !dladdr(symbol, &info) || !info.dli_fname || !info.dli_fbase
			|| std::filesystem::weakly_canonical(info.dli_fname) != path_
			|| (base_ && base_ != info.dli_fbase))
			throw std::runtime_error(std::string("Missing or foreign native docking export: ") + name);
		base_ = info.dli_fbase;
		return symbol;
	}
private:
	std::filesystem::path path_;
	void* handle_{};
	void* base_{};
};

[[nodiscard]] std::filesystem::path executable_directory(const Context& context) {
	auto directory = context.reports_directory;
	for (int index = 0; index < 4; ++index) directory = directory.parent_path();
	return directory;
}

struct NativePanel {
	Widget* widget{};
	PanelInfo info;
};

struct NativeBranch {
	std::string id;
	std::string parent_id;
	std::string class_name;
	PanelInfo geometry;
	Size frame_size{};
	int document_tab_height{};
};

class Bindings {
public:
	using Manager = Object* (*)(const Object*);
	using Managed = const Widget* (*)(const Object*);
	using Insert = void (*)(Object*, Object*, const Size*, int, Object*);
	using Closed = bool (*)(const Object*);
	using Close = void (*)(Object*, Object*);
	using Show = void (*)(Object*, Object*, bool, bool);
	using PanelWidget = const Widget* (*)(const Object*);
	using Minimum = Size (*)(const Widget*);
	using Visible = bool (*)(const Widget*, const Widget*);
	using LayoutManager = const Object* (*)(const Object*);
	using LayoutParent = const Widget* (*)(const Object*);
	using LayoutInvalidate = void (*)(Object*);
	using LayoutActivate = bool (*)(Object*);
	using LayoutGeometry = Rect (*)(const Object*);
	using Ancestor = bool (*)(const Widget*, const Widget*);
	using ParentPanel = const Object* (*)(const Object*);
	using Metric = int (*)(const Widget*, int);
	using FrameGeometry = Rect (*)(const Widget*);
	using TabHeight = int (*)(const Object*);

	explicit Bindings(const Context& context)
		: qtitan_(executable_directory(context) / "libQtitanDocking.dylib"),
		  widgets_(executable_directory(context).parent_path()
			  / "Frameworks/QtWidgets.framework/Versions/5/QtWidgets") {
		// with_guarded_docking_manager has already verified this Qtitan image's
		// ARM64 UUID, bundled Qt 5.15.9, complete inventory and manager identity.
		id_ = qtitan_.resolve("_ZNK6Qtitan13DockPanelBase2idEv");
		manager = reinterpret_cast<Manager>(qtitan_.resolve("_ZNK6Qtitan13DockPanelBase11dockManagerEv"));
		managed = reinterpret_cast<Managed>(qtitan_.resolve("_ZNK6Qtitan16DockPanelManager13managedWidgetEv"));
		insert = reinterpret_cast<Insert>(qtitan_.resolve(
			"_ZN6Qtitan16DockPanelManager15insertDockPanelEPNS_15DockWidgetPanelERK5QSizeNS_13DockPanelAreaEPNS_13DockPanelBaseE"));
		closed = reinterpret_cast<Closed>(qtitan_.resolve("_ZNK6Qtitan15DockWidgetPanel8isClosedEv"));
		close = reinterpret_cast<Close>(qtitan_.resolve("_ZN6Qtitan16DockPanelManager14closeDockPanelEPNS_15DockWidgetPanelE"));
		show = reinterpret_cast<Show>(qtitan_.resolve("_ZN6Qtitan16DockPanelManager13showDockPanelEPNS_15DockWidgetPanelEbb"));
		panel_widget_ = reinterpret_cast<PanelWidget>(qtitan_.resolve("_ZNK6Qtitan15DockWidgetPanel6widgetEv"));
		minimum_ = reinterpret_cast<Minimum>(widgets_.resolve("_ZNK7QWidget11minimumSizeEv"));
		minimum_hint_ = reinterpret_cast<Minimum>(widgets_.resolve("_ZNK7QWidget15minimumSizeHintEv"));
		visible_ = reinterpret_cast<Visible>(widgets_.resolve("_ZNK7QWidget11isVisibleToEPKS_"));
		layout_manager_ = reinterpret_cast<LayoutManager>(qtitan_.resolve(
			"_ZNK6Qtitan20DockMainWindowLayout16dockPanelManagerEv"));
		layout_invalidate_ = reinterpret_cast<LayoutInvalidate>(qtitan_.resolve(
			"_ZN6Qtitan20DockMainWindowLayout10invalidateEv"));
		layout_activate_ = reinterpret_cast<LayoutActivate>(widgets_.resolve("_ZN7QLayout8activateEv"));
		layout_geometry_ = reinterpret_cast<LayoutGeometry>(widgets_.resolve("_ZNK7QLayout8geometryEv"));
		layout_parent_ = reinterpret_cast<LayoutParent>(widgets_.resolve("_ZNK7QLayout12parentWidgetEv"));
		ancestor_ = reinterpret_cast<Ancestor>(widgets_.resolve("_ZNK7QWidget12isAncestorOfEPKS_"));
		parent_panel_ = reinterpret_cast<ParentPanel>(qtitan_.resolve("_ZNK6Qtitan13DockPanelBase11parentPanelEv"));
		metric_ = reinterpret_cast<Metric>(widgets_.resolve("_ZNK7QWidget6metricEN12QPaintDevice17PaintDeviceMetricE"));
		frame_geometry_ = reinterpret_cast<FrameGeometry>(widgets_.resolve("_ZNK7QWidget13frameGeometryEv"));
		tab_height_ = reinterpret_cast<TabHeight>(qtitan_.resolve("_ZN6Qtitan15DockWidgetPanel16getTabAreaHeightEv"));
	}

	[[nodiscard]] Object* main_window_layout(const Context& context, std::string* diagnostic = nullptr) const {
		const auto manager_record = std::find_if(context.objects.objects.begin(), context.objects.objects.end(),
			[&](const auto& record) { return record.object == context.manager; });
		if (manager_record == context.objects.objects.end())
			throw std::runtime_error("Docking manager is absent from the guarded object inventory");
		const auto* managed_widget = managed(context.manager);
		if (!managed_widget || std::find(context.widgets.widgets.begin(), context.widgets.widgets.end(), managed_widget)
			== context.widgets.widgets.end())
			throw std::runtime_error("Docking manager's managed widget is absent from the guarded inventory");
		const auto class_name = [&](const Object* object) -> std::string {
			const auto record = std::find_if(context.objects.objects.begin(), context.objects.objects.end(),
				[&](const auto& candidate) { return candidate.object == object; });
			return record == context.objects.objects.end() ? "unrecorded" : record->class_name;
		};
		std::ostringstream candidates;
		std::size_t exact_count = 0, type_count = 0, same_root_count = 0, linked_count = 0, matches = 0;
		Object* found = nullptr;
		for (const auto& record : context.objects.objects) {
			const bool exact_class = record.class_name == "Qtitan::DockMainWindowLayout";
			const bool layout_type = exact_class || record.object->inherits("Qtitan::DockMainWindowLayout");
			if (!layout_type) continue;
			++type_count;
			exact_count += exact_class;
			const bool same_root = record.root_id == manager_record->root_id;
			same_root_count += same_root;
			const auto* parent = layout_parent_(record.object);
			const auto parent_record = std::find_if(context.objects.objects.begin(), context.objects.objects.end(),
				[&](const auto& candidate) { return candidate.object == parent; });
			const bool parent_widget = parent && std::find(context.widgets.widgets.begin(),
				context.widgets.widgets.end(), parent) != context.widgets.widgets.end();
			const bool parent_same_root = parent_record != context.objects.objects.end()
				&& parent_record->root_id == manager_record->root_id;
			const bool parent_main = parent_widget && parent == context.main_window;
			const bool parent_managed = parent_widget && parent == managed_widget;
			const bool parent_managed_ancestor = parent_widget && ancestor_(parent, managed_widget);
			const bool within_main = parent_widget && (parent_main || ancestor_(context.main_window, parent));
			const bool manager_linked = layout_manager_(record.object) == context.manager;
			linked_count += manager_linked;
			const bool matches_host = same_root && parent_same_root && manager_linked && within_main
				&& (parent_managed || parent_managed_ancestor);
			if (matches_host) {
				++matches;
				found = const_cast<Object*>(record.object);
			}
			// Bound output independently of the guarded inventory limit. All
			// identity checks still run; no native addresses enter the report.
			if (type_count <= 16) candidates << "workspace_layout_candidate class=" << std::quoted(record.class_name)
				<< " root=" << record.root_id << " same_root=" << same_root
				<< " parent_class=" << std::quoted(class_name(parent))
				<< " parent_widget=" << parent_widget << " parent_same_root=" << parent_same_root
				<< " parent_main=" << parent_main << " parent_managed=" << parent_managed
				<< " parent_managed_ancestor=" << parent_managed_ancestor << " within_main=" << within_main
				<< " manager_linked=" << manager_linked << " matches_host=" << matches_host << '\n';
		}
		std::ostringstream summary;
		summary << "workspace_layout_lookup manager_root=" << manager_record->root_id
			<< " main_class=" << std::quoted(class_name(context.main_window))
			<< " managed_class=" << std::quoted(class_name(managed_widget))
			<< " exact_classes=" << exact_count << " layout_types=" << type_count
			<< " same_root=" << same_root_count << " manager_linked=" << linked_count
			<< " host_matches=" << matches << " candidates_omitted=" << (type_count > 16 ? type_count - 16 : 0)
			<< '\n' << candidates.str();
		if (diagnostic) *diagnostic = summary.str();
		if (matches != 1) throw std::runtime_error(matches == 0
			? "Docking layout is not linked to a guarded host of the managed widget\n" + summary.str()
			: "Docking layout host identity is ambiguous\n" + summary.str());
		return found;
	}

	void invalidate_main_window_layout(Object* layout) const { layout_invalidate_(layout); }
	[[nodiscard]] bool activate_main_window_layout(Object* layout) const { return layout_activate_(layout); }
	[[nodiscard]] Rect main_window_layout_geometry(const Object* layout) const { return layout_geometry_(layout); }

	[[nodiscard]] PanelInfo managed_bounds(const Context& context) const {
		const auto* widget = managed(context.manager);
		if (!widget || std::find(context.widgets.widgets.begin(), context.widgets.widgets.end(), widget)
			== context.widgets.widgets.end())
			throw std::runtime_error("Docking manager's managed widget is absent from the guarded inventory");
		const Point origin{0, 0};
		const auto global = map_to_global(widget, origin);
		const auto window_global = map_to_global(context.main_window, origin);
		PanelInfo bounds;
		bounds.x = global.x - window_global.x;
		bounds.y = global.y - window_global.y;
		const auto size = dimensions(widget);
		bounds.width = size.width;
		bounds.height = size.height;
		return bounds;
	}

	[[nodiscard]] std::vector<NativePanel> panels(const Context& context) const {
		std::vector<NativePanel> result;
		std::set<std::string> ids;
		std::set<const Widget*> seen;
		for (auto* widget : context.widgets.widgets) {
			if (!widget->inherits("Qtitan::DockWidgetPanel") || manager(widget) != context.manager) continue;
			if (!seen.insert(widget).second) throw std::runtime_error("Native docking inventory repeats a panel pointer");
			PanelInfo info;
			info.id = panel_id(widget);
			if (info.id.empty() || info.id.find('\0') != std::string::npos || !ids.insert(info.id).second)
				throw std::runtime_error("Native docking inventory has an empty or duplicate panel ID");
			info.document = widget->inherits("Qtitan::DockDocumentPanel");
			info.closed = closed(widget);
			info.visible = visible_(widget, nullptr);
			// Floating widgets and docked widgets use the same MainWindow origin.
			const Point origin{0, 0};
			// QWidget::mapTo requires an ancestor. Floating panels are not always
			// MainWindow descendants, so use the public global-coordinate mapping.
			const auto global = map_to_global(widget, origin);
			const auto window_global = map_to_global(context.main_window, origin);
			info.x = global.x - window_global.x;
			info.y = global.y - window_global.y;
			const auto size = dimensions(widget);
			info.width = size.width;
			info.height = size.height;
			const auto minimum = minimum_(widget);
			const auto hint = minimum_hint_(widget);
			info.minimum_width = std::max({0, minimum.width, hint.width});
			info.minimum_height = std::max({0, minimum.height, hint.height});
			if (const auto* content = panel_widget_(widget)) {
				if (std::find(context.widgets.widgets.begin(), context.widgets.widgets.end(), content)
					== context.widgets.widgets.end())
					throw std::runtime_error("Native docking panel content is absent from the guarded widget inventory");
				const auto content_minimum = minimum_(content);
				const auto content_hint = minimum_hint_(content);
				info.minimum_width = std::max({info.minimum_width, content_minimum.width, content_hint.width});
				info.minimum_height = std::max({info.minimum_height, content_minimum.height, content_hint.height});
			}
			result.push_back({widget, std::move(info)});
		}
		std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.info.id < b.info.id; });
		return result;
	}

	[[nodiscard]] std::map<std::string, NativeBranch> branches(const Context& context,
		const std::set<std::string>& included_ids) const {
		const auto leaves = panels(context);
		const auto manager_record = std::find_if(context.objects.objects.begin(), context.objects.objects.end(),
			[&](const auto& record) { return record.object == context.manager; });
		if (manager_record == context.objects.objects.end()) throw std::runtime_error("Docking manager has no guarded object record");
		std::map<std::string, NativeBranch> result;
		std::map<std::string, const Object*> seen_ids;
		for (const auto& id : included_ids) {
			const auto leaf = std::find_if(leaves.begin(), leaves.end(), [&](const auto& panel) { return panel.info.id == id; });
			if (leaf == leaves.end()) throw std::runtime_error("Workspace branch leaf is no longer available: " + id);
			const Object* current = leaf->widget;
			std::set<const Object*> ancestry;
			while (current) {
				if (ancestry.size() > kMaxDepth || !ancestry.insert(current).second)
					throw std::runtime_error("Native workspace parent chain is too deep or cyclic");
				const auto record = std::find_if(context.objects.objects.begin(), context.objects.objects.end(),
					[&](const auto& candidate) { return candidate.object == current; });
				const auto widget = std::find_if(context.widgets.widgets.begin(), context.widgets.widgets.end(),
					[&](const auto* candidate) { return static_cast<const Object*>(candidate) == current; });
				if (record == context.objects.objects.end() || record->root_id != manager_record->root_id
					|| widget == context.widgets.widgets.end() || !current->inherits("Qtitan::DockPanelBase")
					|| manager(current) != context.manager)
					throw std::runtime_error("Native workspace parent is outside the guarded docking tree");
				NativeBranch branch;
				branch.id = panel_id(current);
				if (branch.id.empty()) throw std::runtime_error("A native workspace branch has no serialized ID");
				const auto* parent = parent_panel_(current);
				// Validate the returned parent before calling any export on it.
				if (parent && (std::find_if(context.widgets.widgets.begin(), context.widgets.widgets.end(),
					[&](const auto* candidate) { return static_cast<const Object*>(candidate) == parent; })
					== context.widgets.widgets.end() || !parent->inherits("Qtitan::DockPanelBase")
					|| manager(parent) != context.manager))
					throw std::runtime_error("Native workspace parent is not a guarded panel widget");
				branch.parent_id = parent ? panel_id(parent) : std::string{};
				branch.class_name = record->class_name;
				branch.geometry.id = branch.id;
				branch.geometry.document = current->inherits("Qtitan::DockDocumentPanel");
				if (branch.geometry.document) {
					// Pinned public getter 0x3d990 reads the height of the visible
					// painted document tab bar, not its logical layout's QWidget.
					branch.document_tab_height = tab_height_(current);
					if (branch.document_tab_height < 0 || branch.document_tab_height > 128)
						throw std::runtime_error("Workspace document tab height is outside the supported measured range");
				}
				const Point origin{0, 0};
				const auto global = map_to_global(*widget, origin);
				const auto window_global = map_to_global(context.main_window, origin);
				branch.geometry.x = global.x - window_global.x;
				branch.geometry.y = global.y - window_global.y;
				const auto size = dimensions(*widget);
				branch.geometry.width = size.width;
				branch.geometry.height = size.height;
				const auto frame = frame_geometry_(*widget);
				branch.frame_size = {frame.right - frame.left + 1, frame.bottom - frame.top + 1};
				const auto previous = seen_ids.find(branch.id);
				if (previous != seen_ids.end() && previous->second != current)
					throw std::runtime_error("Native workspace branch IDs are ambiguous: " + branch.id);
				seen_ids.emplace(branch.id, current);
				result.insert_or_assign(branch.id, std::move(branch));
				current = parent;
			}
		}
		return result;
	}

	Manager manager{};
	Managed managed{};
	Insert insert{};
	Closed closed{};
	Close close{};
	Show show{};
private:
	[[nodiscard]] Size dimensions(const Widget* widget) const {
		// Qt 5.15 QPaintDevice::PdmWidth=1, PdmHeight=2. Resolve the public
		// QWidget implementation in the verified image directly so installed
		// loader versions cannot change the width/height fallback behavior.
		return {metric_(widget, 1), metric_(widget, 2)};
	}
	[[nodiscard]] std::string panel_id(const Object* panel) const {
		rml::qt::QString value;
		auto& storage = *static_cast<void**>(value.storage());
		rml::memory::call_returning_member(id_, storage, panel);
		return value.to_utf8();
	}
	[[nodiscard]] Point map_to_global(const Widget* widget, const Point& origin) const {
		return map_global_(widget, &origin);
	}
	Library qtitan_;
	Library widgets_;
	void* id_{};
	PanelWidget panel_widget_{};
	Minimum minimum_{};
	Minimum minimum_hint_{};
	Visible visible_{};
	LayoutManager layout_manager_{};
	LayoutParent layout_parent_{};
	LayoutInvalidate layout_invalidate_{};
	LayoutActivate layout_activate_{};
	LayoutGeometry layout_geometry_{};
	Ancestor ancestor_{};
	ParentPanel parent_panel_{};
	Metric metric_{};
	FrameGeometry frame_geometry_{};
	TabHeight tab_height_{};
	using MapGlobal = Point (*)(const Widget*, const Point*);
	MapGlobal map_global_{reinterpret_cast<MapGlobal>(widgets_.resolve("_ZNK7QWidget11mapToGlobalERK6QPoint"))};
};

// The legacy guard reports failures as text and catches callback exceptions.
// Capture exceptions inside the callback so this public backend can propagate
// the actual failure, while still reusing all of the established guard checks.
template<class Function>
[[nodiscard]] auto guarded(Function&& function) {
	using Result = std::invoke_result_t<Function, const Context&>;
	std::optional<Result> result;
	std::exception_ptr exception;
	bool called = false;
	const auto report = rml::workspace::support::with_guarded_docking_manager([&](const Context& context) {
		called = true;
		try { result.emplace(function(context)); }
		catch (...) { exception = std::current_exception(); }
		return std::string{};
	});
	if (exception) std::rethrow_exception(exception);
	if (!called || !result) throw std::runtime_error("Native docking guard rejected the operation:\n" + report);
	return std::move(*result);
}

void validate_key(const std::string_view key) {
	if (key.empty() || key.size() > 4096 || key.find('\0') != std::string_view::npos)
		throw std::runtime_error("Panel ID must be a nonempty string without NUL bytes");
}

[[nodiscard]] std::string_view builtin_id(const std::string_view name) {
	if (name == "Viewport") return kViewportId;
	if (name == "Output") return "outputWidgetPanel";
	if (name == "Properties") return "propertiesWidgetPanel";
	if (name == "Explorer") return "rplg_sabuiltin_ExplorerPlugin.rbxm_ExplorerPlugin";
	return {};
}

[[nodiscard]] const NativePanel& exact_panel(const std::vector<NativePanel>& panels, const std::string_view id) {
	const auto found = std::find_if(panels.begin(), panels.end(), [&](const auto& panel) { return panel.info.id == id; });
	if (found == panels.end()) throw std::runtime_error("Native panel is no longer available: " + std::string(id));
	return *found;
}

[[nodiscard]] std::string resolve(const std::vector<NativePanel>& panels, const std::string_view key) {
	validate_key(key);
	for (const auto& panel : panels) if (panel.info.id == key) return panel.info.id;
	if (const auto id = builtin_id(key); !id.empty()) return exact_panel(panels, id).info.id;
	const NativePanel* match = nullptr;
	for (const auto& panel : panels) {
		const auto& id = panel.info.id;
		if (id.size() <= key.size() || !id.ends_with(key) || id[id.size() - key.size() - 1] != '_') continue;
		if (match) throw std::runtime_error("Panel ID suffix is ambiguous: " + std::string(key));
		match = &panel;
	}
	if (!match) throw std::runtime_error("Panel ID does not resolve to a native panel: " + std::string(key));
	return match->info.id;
}

[[nodiscard]] std::string panel_report(const PanelInfo& panel) {
	std::ostringstream report;
	report << "workspace_panel id=" << std::quoted(panel.id)
		<< " document=" << (panel.document ? "true" : "false")
		<< " closed=" << (panel.closed ? "true" : "false")
		<< " visible=" << (panel.visible ? "true" : "false")
		<< " x=" << panel.x << " y=" << panel.y
		<< " width=" << panel.width << " height=" << panel.height
		<< " minimum_width=" << panel.minimum_width << " minimum_height=" << panel.minimum_height << '\n';
	return report.str();
}

[[nodiscard]] PanelInfo inspect_panel(const std::string_view id) {
	return guarded([&](const Context& context) {
		Bindings bindings(context);
		return exact_panel(bindings.panels(context), id).info;
	});
}

[[nodiscard]] std::string reflow_main_window() {
	const auto invalidated = guarded([](const Context& context) {
		Bindings bindings(context);
		// Qtitan restore can invalidate QLayout's stored rectangle. Calling
		// DockMainWindowLayout::updateGeometry alone reads that stored rectangle;
		// it does not assign the host's current extent. The pinned public
		// QLayout::activate export (0x2ac64) runs doResize and Qtitan's virtual
		// setGeometry (0x14474) synchronously, using the actual host contents.
		// Qt 5.15 source: https://github.com/qt/qtbase/blob/5.15/src/widgets/kernel/qlayout.cpp
		// First clear its activated flag, then reacquire before activation. No
		// event-loop pumping or native pointer survives either mutation.
		std::string diagnostic;
		auto* layout = bindings.main_window_layout(context, &diagnostic);
		const auto before = bindings.main_window_layout_geometry(layout);
		std::ostringstream report;
		report << diagnostic << "workspace_layout_cached_rectangle left=" << before.left << " top=" << before.top
			<< " right=" << before.right << " bottom=" << before.bottom << '\n';
		bindings.invalidate_main_window_layout(layout);
		return report.str();
	});
	const auto activated = guarded([](const Context& context) {
		Bindings bindings(context);
		std::string diagnostic;
		auto* layout = bindings.main_window_layout(context, &diagnostic);
		if (!bindings.activate_main_window_layout(layout))
			throw std::runtime_error("Workspace docking layout could not be activated after invalidation\n" + diagnostic);
		return std::string{"workspace_layout_activation=completed\n"};
	});
	return invalidated + activated;
}

[[nodiscard]] rml::workspace::DockingResult set_panel_open(const std::string& id, const bool open) {
	const auto before = inspect_panel(id);
	if (!open && before.document)
		throw std::runtime_error("Workspace cannot close a Studio document panel: " + id);
	const auto requested = guarded([&](const Context& context) {
		Bindings bindings(context);
		const auto panels = bindings.panels(context);
		const auto& panel = exact_panel(panels, id);
		if (open) {
			// Exact pinned export at 0x20934; DockWidgetPanel::showPanel at
			// 0x3c708 supplies true,false. Its return type is void. The extra
			// bool is a Studio fork detail absent from current public docs:
			// https://www.devmachines.com/documentation/QtitanComponents/html/dockpanelmanager.html
			if (panel.info.closed || !panel.info.visible) bindings.show(context.manager, panel.widget, true, false);
		} else if (!panel.info.closed) bindings.close(context.manager, panel.widget);
		// Do not touch panel/manager or entry-state inventory after mutation.
		return true;
	});
	const auto after = inspect_panel(id);
	const bool verified = requested && (open ? !after.closed && after.visible : after.closed && !after.visible);
	if (!verified) throw std::runtime_error(std::string("Native panel ") + (open ? "open" : "close")
		+ " did not pass the visibility check: " + id + "\n" + panel_report(after));
	std::ostringstream report;
	report << "workspace_panel_operation=" << (open ? "open" : "close") << '\n'
		<< "workspace_panel_id=" << std::quoted(id) << '\n'
		<< "workspace_panel_verified=true\n" << panel_report(after);
	return {report.str(), false};
}

struct PreparedNode {
	std::string id;
	Direction direction{};
	double scale{};
	int offset{};
	int width{};
	int height{};
	int minimum_width{};
	int minimum_height{};
	std::unique_ptr<PreparedNode> first;
	std::unique_ptr<PreparedNode> second;
};

[[nodiscard]] std::unique_ptr<PreparedNode> prepare_node(const SplitNode& node,
	const std::vector<NativePanel>& panels, std::set<std::string>& ids,
	std::set<const SplitNode*>& ancestry, const std::size_t depth) {
	if (depth > kMaxDepth || !ancestry.insert(&node).second)
		throw std::runtime_error("Workspace split tree is too deep or contains a cycle");
	struct RemoveAncestor {
		std::set<const SplitNode*>& set;
		const SplitNode* node;
		~RemoveAncestor() { set.erase(node); }
	} remove{ancestry, &node};
	auto prepared = std::make_unique<PreparedNode>();
	if (!node.panel_id.empty()) {
		if (node.first || node.second) throw std::runtime_error("A panel leaf cannot also contain split children");
		prepared->id = resolve(panels, node.panel_id);
		if (!ids.insert(prepared->id).second || ids.size() > kMaxLeaves)
			throw std::runtime_error("Workspace split tree repeats a panel or exceeds its panel limit");
		const auto& panel = exact_panel(panels, prepared->id).info;
		if (prepared->id.starts_with("PlaceholderPanel") || (panel.document && prepared->id != kViewportId))
			throw std::runtime_error("Workspace split tree contains an unsupported document or placeholder panel: " + prepared->id);
		prepared->minimum_width = std::max(1, panel.minimum_width);
		prepared->minimum_height = std::max(1, panel.minimum_height);
		return prepared;
	}
	if (!node.first || !node.second) throw std::runtime_error("Workspace split requires both First and Second children");
	if (node.direction != Direction::Horizontal && node.direction != Direction::Vertical)
		throw std::runtime_error("Workspace split direction is invalid");
	if (!std::isfinite(node.size.scale) || node.size.scale < 0 || node.size.scale > 1)
		throw std::runtime_error("Workspace split scale must be finite and between zero and one");
	prepared->direction = node.direction;
	prepared->scale = node.size.scale;
	prepared->offset = node.size.offset;
	prepared->first = prepare_node(*node.first, panels, ids, ancestry, depth + 1);
	prepared->second = prepare_node(*node.second, panels, ids, ancestry, depth + 1);
	const auto& first = *prepared->first;
	const auto& second = *prepared->second;
	const auto minimum_width = node.direction == Direction::Horizontal
		? static_cast<long long>(first.minimum_width) + second.minimum_width
		: std::max(first.minimum_width, second.minimum_width);
	const auto minimum_height = node.direction == Direction::Vertical
		? static_cast<long long>(first.minimum_height) + second.minimum_height
		: std::max(first.minimum_height, second.minimum_height);
	if (minimum_width > kMaxSpan || minimum_height > kMaxSpan)
		throw std::runtime_error("Workspace split minimum sizes exceed the supported managed-widget span");
	prepared->minimum_width = static_cast<int>(minimum_width);
	prepared->minimum_height = static_cast<int>(minimum_height);
	return prepared;
}

void allocate(PreparedNode& node, const int width, const int height) {
	if (width <= 0 || height <= 0 || width > kMaxSpan || height > kMaxSpan
		|| width < node.minimum_width || height < node.minimum_height)
		throw std::runtime_error("Workspace allocation is smaller than a panel's native minimum size");
	node.width = width;
	node.height = height;
	if (!node.first) return;
	const int span = node.direction == Direction::Horizontal ? width : height;
	const double requested = span * node.scale + node.offset;
	if (!std::isfinite(requested) || requested < 1 || requested > span - 1)
		throw std::runtime_error("Workspace split Size leaves no space for one of its branches");
	const int first_span = static_cast<int>(std::lround(requested));
	if (node.direction == Direction::Horizontal) {
		allocate(*node.first, first_span, height);
		allocate(*node.second, width - first_span, height);
	} else {
		allocate(*node.first, width, first_span);
		allocate(*node.second, width, height - first_span);
	}
}

[[nodiscard]] bool contains(const PreparedNode& node, const std::string_view id) {
	return node.first ? contains(*node.first, id) || contains(*node.second, id) : node.id == id;
}

[[nodiscard]] const PreparedNode& seed(const PreparedNode& node) {
	return node.first ? seed(*node.first) : node;
}

void arrange(const PreparedNode& node, const std::string& anchor, std::string& report) {
	if (!node.first) {
		if (node.id != anchor) throw std::runtime_error("Workspace split planner lost its anchor panel");
		return;
	}
	const bool anchor_first = contains(*node.first, anchor);
	const auto& anchored = anchor_first ? *node.first : *node.second;
	const auto& added = anchor_first ? *node.second : *node.first;
	const auto& selected = seed(added);
	const int area = node.direction == Direction::Horizontal
		? (anchor_first ? 2 : 1) : (anchor_first ? 8 : 4);
	const Size size{added.width, added.height};
	(void)guarded([&](const Context& context) {
		Bindings bindings(context);
		const auto panels = bindings.panels(context);
		const auto& panel = exact_panel(panels, selected.id);
		const auto& target = exact_panel(panels, anchor);
		if (panel.info.document) throw std::runtime_error("Workspace refuses to insert a document as a dock-widget sibling");
		bindings.insert(context.manager, panel.widget, &size, area, target.widget);
		// Never read any pointer again after the native mutation.
		return true;
	});
	std::ostringstream inserted;
	inserted << "workspace_insert id=" << std::quoted(selected.id) << " target=" << std::quoted(anchor)
		<< " area=" << area << " width=" << size.width << " height=" << size.height
		<< " first_width=" << node.first->width << " first_height=" << node.first->height << '\n';
	report += inserted.str();
	report += set_panel_open(selected.id, true).report;
	arrange(anchored, anchor, report);
	arrange(added, selected.id, report);
}

[[nodiscard]] bool report_has(const std::string& report, const std::string_view line) {
	return report.find(std::string(line) + '\n') != std::string::npos;
}

struct File {
	int descriptor{-1};
	~File() { if (descriptor >= 0) close(descriptor); }
};

[[nodiscard]] std::string read_snapshot(const std::filesystem::path& path) {
	constexpr std::size_t maximum = 4 * 1024 * 1024;
	File file{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
	struct stat status{};
	if (file.descriptor < 0 || fstat(file.descriptor, &status) != 0 || !S_ISREG(status.st_mode)
		|| status.st_uid != getuid() || status.st_nlink != 1 || status.st_size <= 0
		|| static_cast<std::uintmax_t>(status.st_size) > maximum)
		throw std::runtime_error("Workspace sizing snapshot is not a bounded, owned regular file");
	std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto received = read(file.descriptor, bytes.data() + offset, bytes.size() - offset);
		if (received < 0 && errno == EINTR) continue;
		if (received <= 0) throw std::runtime_error("Could not read the complete workspace sizing snapshot");
		offset += static_cast<std::size_t>(received);
	}
	char extra{};
	if (read(file.descriptor, &extra, 1) != 0)
		throw std::runtime_error("Workspace sizing snapshot changed while reading");
	return bytes;
}

void write_snapshot(const std::filesystem::path& path, const std::string& bytes) {
	if (bytes.empty() || bytes.size() > 4 * 1024 * 1024)
		throw std::runtime_error("Workspace sized snapshot exceeds its byte limit");
	File file{open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
	if (file.descriptor < 0) throw std::runtime_error("Could not exclusively create the workspace sized snapshot");
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto written = write(file.descriptor, bytes.data() + offset, bytes.size() - offset);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0) throw std::runtime_error("Could not write the complete workspace sized snapshot");
		offset += static_cast<std::size_t>(written);
	}
}

[[nodiscard]] std::string xml_attribute(const xmlNode* node, const char* name) {
	auto* value = xmlGetProp(node, reinterpret_cast<const xmlChar*>(name));
	if (!value) return {};
	const std::string result(reinterpret_cast<const char*>(value));
	xmlFree(value);
	return result;
}

[[nodiscard]] bool xml_named(const xmlNode* node, const char* name) {
	return node && node->type == XML_ELEMENT_NODE && !node->ns
		&& xmlStrEqual(node->name, reinterpret_cast<const xmlChar*>(name));
}

[[nodiscard]] std::size_t xml_count(const xmlNode* node) {
	const auto value = xml_attribute(node, "Count");
	std::size_t count{};
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
	if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || count > 8192)
		throw std::runtime_error("Workspace sizing snapshot has an invalid declared count");
	return count;
}

using LeafSet = std::set<std::string>;

[[nodiscard]] LeafSet planned_sets(const PreparedNode& node, std::map<LeafSet, const PreparedNode*>& nodes) {
	LeafSet leaves;
	if (!node.first) leaves.insert(node.id);
	else {
		leaves = planned_sets(*node.first, nodes);
		const auto second = planned_sets(*node.second, nodes);
		leaves.insert(second.begin(), second.end());
	}
	if (!nodes.emplace(leaves, &node).second)
		throw std::runtime_error("Workspace sizing plan has duplicate descendant sets");
	return leaves;
}

struct NativeSplit {
	std::string id;
	std::string first_id;
	std::string second_id;
	Direction direction{};
	double scale{};
	int offset{};
	LeafSet first_leaves;
	LeafSet second_leaves;
};

struct SizedLayout {
	std::filesystem::path path;
	std::vector<NativeSplit> splits;
	std::string report;
};

[[nodiscard]] SizedLayout size_snapshot(const std::filesystem::path& snapshot, const PreparedNode& root,
	const LeafSet& included, const std::map<std::string, NativeBranch>& native) {
	const auto bytes = read_snapshot(snapshot);
	using Document = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
	Document document(xmlReadMemory(bytes.data(), static_cast<int>(bytes.size()), "workspace-sizing.xml", nullptr,
		XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc);
	if (!document || document->intSubset || document->extSubset)
		throw std::runtime_error("Workspace sizing snapshot is not safe XML");
	const auto* envelope = xmlDocGetRootElement(document.get());
	if (!xml_named(envelope, "DockPanelLayouts") || xml_attribute(envelope, "type") != "layoutPanel"
		|| xml_attribute(envelope, "version") != "1.3")
		throw std::runtime_error("Workspace sizing snapshot has an unsupported root");
	xmlNode* panel_section = nullptr;
	for (auto* section = envelope->children; section; section = section->next)
		if (xml_named(section, "Panels")) {
			if (panel_section) throw std::runtime_error("Workspace sizing snapshot repeats its panel section");
			panel_section = section;
		}
	if (!panel_section) throw std::runtime_error("Workspace sizing snapshot has no panel declarations");
	std::map<std::string, xmlNode*> declarations;
	std::map<std::string, std::vector<std::string>> children;
	std::map<std::string, std::vector<std::string>> parents;
	for (auto* panel = panel_section->children; panel; panel = panel->next) {
		if (panel->type != XML_ELEMENT_NODE) continue;
		const auto id = xml_attribute(panel, "ID");
		if (!xml_named(panel, "Panel") || id.empty() || !declarations.emplace(id, panel).second)
			throw std::runtime_error("Workspace sizing snapshot has an invalid panel declaration");
		for (auto* section = panel->children; section; section = section->next) {
			if (!xml_named(section, "Panels")) continue;
			if (children.contains(id)) throw std::runtime_error("Workspace sizing snapshot repeats a child panel section");
			children.emplace(id, std::vector<std::string>{});
			for (auto* child = section->children; child; child = child->next) {
				if (child->type != XML_ELEMENT_NODE) continue;
				const auto child_id = xml_attribute(child, "ID");
				if (!xml_named(child, "Panel") || child_id.empty())
					throw std::runtime_error("Workspace sizing snapshot has an invalid child reference");
				children[id].push_back(child_id);
				parents[child_id].push_back(id);
			}
			if (xml_count(section) != children.at(id).size())
				throw std::runtime_error("Workspace sizing snapshot child count differs from its references");
		}
	}
	if (xml_count(panel_section) != declarations.size())
		throw std::runtime_error("Workspace sizing snapshot panel count differs from its declarations");
	for (const auto& [id, references] : children) {
		(void)id;
		for (const auto& child : references) if (!declarations.contains(child))
			throw std::runtime_error("Workspace sizing snapshot references a missing panel: " + child);
	}
	std::map<std::string, LeafSet> descendants;
	for (const auto& leaf : included) {
		std::string current = leaf;
		std::set<std::string> ancestry;
		while (!current.empty()) {
			if (ancestry.size() > kMaxDepth || !ancestry.insert(current).second)
				throw std::runtime_error("Workspace serialized parent chain is too deep or cyclic");
			const auto branch = native.find(current);
			if (branch == native.end() || !declarations.contains(current))
				throw std::runtime_error("Workspace native branch is absent from its fresh snapshot: " + current);
			const auto references = parents.find(current);
			if (!branch->second.parent_id.empty() && (references == parents.end() || references->second.size() != 1
				|| references->second.front() != branch->second.parent_id))
				throw std::runtime_error("Workspace snapshot parent differs from the guarded native parent: " + current);
			if (branch->second.parent_id.empty() && references != parents.end() && !references->second.empty())
				throw std::runtime_error("Workspace snapshot root differs from the guarded native root: " + current);
			descendants[current].insert(leaf);
			current = branch->second.parent_id;
		}
	}
	std::map<LeafSet, const PreparedNode*> planned;
	(void)planned_sets(root, planned);
	SizedLayout result;
	result.path = snapshot.parent_path() / (snapshot.stem().string() + "-sized.xml");
	for (const auto& [leaves, node] : planned) {
		if (!node->first) continue;
		std::map<LeafSet, const PreparedNode*> unused;
		const auto first_leaves = planned_sets(*node->first, unused);
		unused.clear();
		const auto second_leaves = planned_sets(*node->second, unused);
		std::optional<NativeSplit> match;
		for (const auto& [id, contained] : descendants) {
			if (contained != leaves) continue;
			std::vector<std::string> active;
			for (const auto& child : children[id]) if (descendants.contains(child)) active.push_back(child);
			if (active.size() != 2 || descendants.at(active[0]) != first_leaves || descendants.at(active[1]) != second_leaves)
				continue;
			const auto horizontal = xml_attribute(declarations.at(id), "Horizontal");
			if (horizontal != (node->direction == Direction::Horizontal ? "True" : "False")) continue;
			if (match) throw std::runtime_error("Workspace native split mapping is ambiguous");
			match = NativeSplit{id, active[0], active[1], node->direction, node->scale, node->offset, first_leaves, second_leaves};
		}
		if (!match) throw std::runtime_error("Workspace native split structure does not match its requested First/Second tree");
		result.splits.push_back(std::move(*match));
	}
	std::ostringstream report;
	for (const auto& [id, leaves] : descendants) {
		const auto allocation = planned.find(leaves);
		if (allocation == planned.end()) throw std::runtime_error("Workspace native branch groups an unplanned set of leaves: " + id);
		int width = allocation->second->width;
		int height = allocation->second->height;
		const auto& branch = native.at(id);
		if (branch.geometry.document) {
			// Logical DockLayoutPanel QWidget rectangles can be 0x0/default
			// placeholders. Only the real painted tab-bar height belongs in the
			// document's content hint; its container gets the full allocation.
			if (branch.document_tab_height <= 0 || branch.document_tab_height > 128)
				throw std::runtime_error("Workspace document tab height is not ready or outside the supported measured range: "
					+ id + " document_tab_height=" + std::to_string(branch.document_tab_height));
			height -= branch.document_tab_height;
		}
		if (width <= 0 || height <= 0 || width > kMaxSpan || height > kMaxSpan)
			throw std::runtime_error("Workspace serialized branch allocation has no usable content extent");
		const auto x = std::to_string(width), y = std::to_string(height);
		if (!xmlSetProp(declarations.at(id), BAD_CAST "DockCX", BAD_CAST x.c_str())
			|| !xmlSetProp(declarations.at(id), BAD_CAST "DockCY", BAD_CAST y.c_str()))
			throw std::runtime_error("Could not set workspace serialized branch sizes");
		report << "workspace_size_hint id=" << std::quoted(id) << " width=" << width << " height=" << height << '\n';
	}
	xmlChar* serialized = nullptr;
	int length = 0;
	xmlDocDumpFormatMemoryEnc(document.get(), &serialized, &length, "UTF-8", 1);
	if (!serialized || length <= 0) {
		if (serialized) xmlFree(serialized);
		throw std::runtime_error("Could not serialize workspace branch size hints");
	}
	const std::string output(reinterpret_cast<const char*>(serialized), static_cast<std::size_t>(length));
	xmlFree(serialized);
	write_snapshot(result.path, output);
	result.report = "workspace_native_parent_mapping=verified\n" + report.str();
	return result;
}

[[nodiscard]] bool is_contained(const PanelInfo& panel, const PanelInfo& bounds, const int tolerance = 1) {
	return panel.width > 0 && panel.height > 0 && bounds.width > 0 && bounds.height > 0
		&& static_cast<long long>(panel.x) >= static_cast<long long>(bounds.x) - tolerance
		&& static_cast<long long>(panel.y) >= static_cast<long long>(bounds.y) - tolerance
		&& static_cast<long long>(panel.x) + panel.width <= static_cast<long long>(bounds.x) + bounds.width + tolerance
		&& static_cast<long long>(panel.y) + panel.height <= static_cast<long long>(bounds.y) + bounds.height + tolerance;
}

[[nodiscard]] std::string leaf_rectangles(const LeafSet& leaves, const std::map<std::string, NativeBranch>& branches) {
	std::ostringstream report;
	std::size_t count = 0;
	for (const auto& id : leaves) {
		if (++count > 16) break;
		const auto& leaf = branches.at(id);
		const auto& rect = leaf.geometry;
		report << "workspace_rendered_leaf id=" << std::quoted(id) << " class=" << std::quoted(leaf.class_name)
			<< " x=" << rect.x << " y=" << rect.y << " width=" << rect.width << " height=" << rect.height
			<< " document_tab_height=" << leaf.document_tab_height << " measurement=QtWidgets.QWidget.metric\n";
	}
	if (leaves.size() > 16) report << "workspace_rendered_leaves_omitted=" << leaves.size() - 16 << '\n';
	return report.str();
}

[[nodiscard]] PanelInfo rendered_bounds(const LeafSet& leaves, const std::map<std::string, NativeBranch>& branches) {
	if (leaves.empty()) throw std::runtime_error("Workspace rendered branch contains no leaves");
	int left = std::numeric_limits<int>::max(), top = left;
	int right = std::numeric_limits<int>::min(), bottom = right;
	for (const auto& id : leaves) {
		const auto& leaf = branches.at(id);
		const auto& rect = leaf.geometry;
		if (rect.width <= 0 || rect.height <= 0 || leaf.document_tab_height < 0 || leaf.document_tab_height > 128
			|| (rect.document && leaf.document_tab_height == 0))
			throw std::runtime_error("Workspace rendered leaf or document tab has no usable extent: " + id
				+ "\n" + leaf_rectangles(leaves, branches));
		left = std::min(left, rect.x);
		top = std::min(top, rect.y - leaf.document_tab_height);
		right = std::max(right, rect.x + rect.width);
		bottom = std::max(bottom, rect.y + rect.height);
	}
	PanelInfo result;
	result.x = left; result.y = top; result.width = right - left; result.height = bottom - top;
	return result;
}

[[nodiscard]] std::string verify_splits(const std::vector<NativeSplit>& splits,
	const std::map<std::string, NativeBranch>& branches, const LeafSet& included, const PanelInfo& managed) {
	std::ostringstream report;
	const auto rectangle = [&](const char* role, const std::string& id, const PanelInfo& rect) {
		std::ostringstream text;
		text << "workspace_split_rect role=" << role << " id=" << std::quoted(id)
			<< " x=" << rect.x << " y=" << rect.y << " width=" << rect.width << " height=" << rect.height
			<< " measurement=visible_leaf_union\n";
		return text.str();
	};
	const auto complete = rendered_bounds(included, branches);
	const bool complete_fills_managed = is_contained(complete, managed)
		&& std::abs(complete.x - managed.x) <= 1 && std::abs(complete.y - managed.y) <= 1
		&& std::abs(complete.x + complete.width - managed.x - managed.width) <= 1
		&& std::abs(complete.y + complete.height - managed.y - managed.height) <= 1;
	for (const auto& split : splits) {
		if (!branches.contains(split.id) || !branches.contains(split.first_id) || !branches.contains(split.second_id))
			throw std::runtime_error("A workspace split branch disappeared after loading its size hints");
		const auto first = rendered_bounds(split.first_leaves, branches);
		const auto second = rendered_bounds(split.second_leaves, branches);
		LeafSet both = split.first_leaves;
		both.insert(split.second_leaves.begin(), split.second_leaves.end());
		const auto parent = rendered_bounds(both, branches);
		const auto diagnostic = rectangle("parent", split.id, parent) + rectangle("first", split.first_id, first)
			+ rectangle("second", split.second_id, second) + rectangle("managed", "managed", managed)
			+ leaf_rectangles(both, branches);
		if (!complete_fills_managed)
			throw std::runtime_error("Workspace visible panel extent does not fill the managed widget\n" + diagnostic);
		const bool horizontal = split.direction == Direction::Horizontal;
		const int span = horizontal ? parent.width : parent.height;
		const int first_span = horizontal ? first.width : first.height;
		const int second_span = horizontal ? second.width : second.height;
		const int gap = horizontal ? second.x - first.x - first.width : second.y - first.y - first.height;
		const auto desired = std::lround(span * split.scale + split.offset);
		// A native splitter consumes part of the span. Qtitan divides its
		// remaining pixels in proportion to DockCX/DockCY, with integer rounding.
		const int tolerance = std::max(1, gap) + 1;
		const bool aligned = horizontal
			? std::abs(first.y - second.y) <= 1
				&& std::abs(first.y + first.height - second.y - second.height) <= 1
			: std::abs(first.x - second.x) <= 1
				&& std::abs(first.x + first.width - second.x - second.width) <= 1;
		if (gap < -1 || gap > 16 || span <= 0 || std::abs(first_span + second_span + gap - span) > 2
			|| !aligned || std::abs(first_span - desired) > tolerance)
			throw std::runtime_error("Workspace native split does not match its requested allocation: " + split.id
				+ " span=" + std::to_string(span) + " requested_first=" + std::to_string(desired)
				+ " measured_first=" + std::to_string(first_span) + " measured_second=" + std::to_string(second_span)
				+ " gap=" + std::to_string(gap) + " tolerance=" + std::to_string(tolerance) + "\n" + diagnostic);
		report << "workspace_split_allocation id=" << std::quoted(split.id) << " span=" << span
			<< " requested_first=" << desired << " measured_first=" << first_span
			<< " measured_second=" << second_span << " gap=" << gap << " tolerance=" << tolerance << '\n';
	}
	if (!complete_fills_managed)
		throw std::runtime_error("Workspace visible panel extent does not fill the managed widget\n"
			+ rectangle("complete", "complete", complete) + rectangle("managed", "managed", managed)
			+ leaf_rectangles(included, branches));
	return report.str();
}

} // namespace

namespace rml::workspace {

std::vector<PanelInfo> DockingBackend::list_panels() const {
	return guarded([](const Context& context) {
		Bindings bindings(context);
		std::vector<PanelInfo> result;
		for (const auto& panel : bindings.panels(context)) result.push_back(panel.info);
		return result;
	});
}

std::string DockingBackend::get_builtin_panel(const std::string_view name) const {
	const auto id = builtin_id(name);
	if (id.empty()) throw std::runtime_error("Unsupported built-in panel name: " + std::string(name));
	return guarded([&](const Context& context) {
		Bindings bindings(context);
		const auto panels = bindings.panels(context);
		const auto& panel = exact_panel(panels, id);
		if ((name == "Viewport") != panel.info.document)
			throw std::runtime_error("Built-in panel type does not match its supported identity: " + std::string(name));
		return panel.info.id;
	});
}

std::string DockingBackend::resolve_panel_id(const std::string_view id_or_suffix) const {
	return guarded([&](const Context& context) {
		Bindings bindings(context);
		return resolve(bindings.panels(context), id_or_suffix);
	});
}

DockingResult DockingBackend::open_panel(const std::string_view id_or_suffix) const {
	return set_panel_open(resolve_panel_id(id_or_suffix), true);
}

DockingResult DockingBackend::close_panel(const std::string_view id_or_suffix) const {
	return set_panel_open(resolve_panel_id(id_or_suffix), false);
}

LayoutSnapshot DockingBackend::capture(const std::filesystem::path& directory) const {
	const auto destination = guarded([&](const Context& context) {
		if (!directory.is_absolute() || directory.native().find('\0') != std::string::npos
			|| directory.lexically_normal() != context.reports_directory
			|| std::filesystem::weakly_canonical(directory) != context.reports_directory)
			throw std::runtime_error("Layout snapshot directory must be the workspace mod's own guarded report directory");
		static std::atomic<std::uint64_t> sequence{};
		const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
		return directory / ("layout-" + std::to_string(getpid()) + "-" + std::to_string(tick)
			+ "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".xml");
	});
	const auto report = rml::workspace::support::export_layout_snapshot(destination);
	if (!report_has(report, "layout_snapshot=exported"))
		throw std::runtime_error("Could not capture native layout snapshot:\n" + report);
	return {destination, report};
}

DockingResult DockingBackend::restore(const std::filesystem::path& snapshot) const {
	// The existing validator checks report-directory confinement, symlinks,
	// owner/link count/size, XML envelope, IDs/counts/references and native result.
	const auto report = rml::workspace::support::restore_layout_snapshot(snapshot);
	if (!report_has(report, "layout_snapshot=restored"))
		throw std::runtime_error("Could not restore native layout snapshot:\n" + report);
	return {report + "workspace_restore=applied_pending_geometry_check\n", false};
}

DockingResult DockingBackend::apply(const SplitNode& root) const {
	struct Plan {
		std::unique_ptr<PreparedNode> tree;
		std::vector<std::string> close_ids;
		std::set<std::string> included_ids;
	};
	auto plan = guarded([&](const Context& context) {
		Bindings bindings(context);
		const auto panels = bindings.panels(context);
		const auto& viewport = exact_panel(panels, kViewportId);
		if (!viewport.info.document || viewport.info.closed || !viewport.info.visible)
			throw std::runtime_error("A visible real Viewport document is required before applying a workspace view");
		Plan prepared;
		std::set<const SplitNode*> ancestry;
		prepared.tree = prepare_node(root, panels, prepared.included_ids, ancestry, 0);
		if (!prepared.included_ids.contains(std::string(kViewportId)))
			throw std::runtime_error("A complete workspace view must include the real Viewport document");
		// Verify the identity needed for recalculation before closing any panel.
		(void)bindings.main_window_layout(context);
		const auto bounds = bindings.managed_bounds(context);
		allocate(*prepared.tree, bounds.width, bounds.height);
		for (const auto& panel : panels)
			if (!panel.info.document && !panel.info.id.starts_with("PlaceholderPanel") && !panel.info.closed)
				prepared.close_ids.push_back(panel.info.id);
		return prepared;
	});
	std::string report = "workspace_prepare=verified\n";
	// Keep only immutable node data and ID strings between each operation.
	// Closing/placing/showing can run Studio or plugin callbacks synchronously.
	for (const auto& id : plan.close_ids) report += set_panel_open(id, false).report;
	report += reflow_main_window();
	// Closing old groups can change the manager's client rectangle. Allocate
	// from its refreshed extent rather than the entry-state geometry.
	(void)guarded([&](const Context& context) {
		Bindings bindings(context);
		const auto bounds = bindings.managed_bounds(context);
		allocate(*plan.tree, bounds.width, bounds.height);
		return true;
	});
	arrange(*plan.tree, std::string(kViewportId), report);
	report += reflow_main_window();
	const auto directory = guarded([](const Context& context) { return context.reports_directory; });
	const auto inserted_snapshot = capture(directory);
	const auto native_branches = guarded([&](const Context& context) {
		Bindings bindings(context);
		return bindings.branches(context, plan.included_ids);
	});
	const auto sized_layout = size_snapshot(inserted_snapshot.path, *plan.tree, plan.included_ids, native_branches);
	report += inserted_snapshot.report + sized_layout.report;
	// The existing loader validates the complete unchanged XML graph, owner,
	// directory, IDs, counts and references before Qtitan sees the new hints.
	report += restore(sized_layout.path).report;
	report += reflow_main_window();
	report += "workspace_main_window_layout=recalculated\n";
	report += guarded([&](const Context& context) {
		Bindings bindings(context);
		const auto panels = bindings.panels(context);
		const auto branches = bindings.branches(context, plan.included_ids);
		const auto bounds = bindings.managed_bounds(context);
		std::ostringstream measured;
		measured << "workspace_managed_bounds x=" << bounds.x << " y=" << bounds.y
			<< " width=" << bounds.width << " height=" << bounds.height << '\n';
		std::string verification = measured.str();
		for (const auto& id : plan.included_ids) {
			const auto& panel = exact_panel(panels, id).info;
			if (panel.closed || !panel.visible)
				throw std::runtime_error("An included workspace panel is not visible after applying the split tree: " + id);
			if (!is_contained(panel, bounds))
				throw std::runtime_error("Workspace panel extends outside the managed widget after layout recalculation: "
					+ id + "\n" + measured.str() + panel_report(panel) + leaf_rectangles(plan.included_ids, branches));
			verification += panel_report(panel);
		}
		verification += verify_splits(sized_layout.splits, branches, plan.included_ids, bounds);
		for (const auto& panel : panels)
			if (!panel.info.document && !panel.info.id.starts_with("PlaceholderPanel")
				&& !plan.included_ids.contains(panel.info.id) && (!panel.info.closed || panel.info.visible))
				throw std::runtime_error("An omitted workspace panel reopened during the split-tree transition: " + panel.info.id);
		return verification;
	});
	report += "workspace_bounds_verified=true\nworkspace_allocation_verified=true\n"
		"workspace_arrangement=applied_pending_render_check\nworkspace_geometry_verified=true\n";
	return {std::move(report), true};
}

std::string DockingBackend::inspect() const {
	std::string report = "workspace_inspection_format=workspace-native-panels-v1\n";
	for (const auto& panel : list_panels()) report += panel_report(panel);
	return report;
}

} // namespace rml::workspace
