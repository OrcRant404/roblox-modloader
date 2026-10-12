#include "layout_snapshot.hpp"

#include <RobloxModLoader/qt/qobject.hpp>
#include <RobloxModLoader/qt/qstring.hpp>
#include <RobloxModLoader/qt/qwidget.hpp>

#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libxml/c14n.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

#ifndef RML_WORKSPACE_MOD_DIRECTORY
#define RML_WORKSPACE_MOD_DIRECTORY "workspace"
#endif

namespace
{
	// Exact libQtitanDocking identity investigated in this Studio 0.741 build.
	constexpr std::array<unsigned char, 16> kQtitanUuid{
	    0x4C, 0x35, 0x09, 0x0D, 0xC1, 0x49, 0x38, 0x0F,
	    0x9A, 0xA5, 0xF8, 0x7D, 0x9E, 0x68, 0x7E, 0xBB};
	constexpr std::size_t kMaxSnapshotBytes = 4 * 1024 * 1024;
	constexpr std::size_t kMaxPanels = 8192;

	class LibraryHandle
	{
	public:
		LibraryHandle() = default;
		~LibraryHandle() { if (value) dlclose(value); }
		LibraryHandle(const LibraryHandle&) = delete;
		LibraryHandle& operator=(const LibraryHandle&) = delete;
		void open(const std::filesystem::path& path)
		{ value = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD); }
		void* value{};
	};

	class FileHandle
	{
	public:
		explicit FileHandle(int fd) : value(fd) {}
		~FileHandle() { if (value >= 0) close(value); }
		FileHandle(const FileHandle&) = delete;
		FileHandle& operator=(const FileHandle&) = delete;
		int value;
	};

	struct XmlFree
	{
		void operator()(xmlChar* value) const { xmlFree(value); }
	};
	using XmlText = std::unique_ptr<xmlChar, XmlFree>;
	using XmlDocument = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

	[[nodiscard]] bool has_expected_uuid(const void* base)
	{
		const auto* header = static_cast<const mach_header_64*>(base);
		if (!header || header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64
		    || header->ncmds > 4096 || header->sizeofcmds > 1024 * 1024) return false;
		const char* cursor = reinterpret_cast<const char*>(header + 1);
		const char* end = cursor + header->sizeofcmds;
		for (std::uint32_t index = 0; index < header->ncmds; ++index)
		{
			if (end - cursor < static_cast<std::ptrdiff_t>(sizeof(load_command))) return false;
			const auto* command = reinterpret_cast<const load_command*>(cursor);
			if (command->cmdsize < sizeof(load_command)
			    || command->cmdsize > static_cast<std::size_t>(end - cursor)) return false;
			if (command->cmd == LC_UUID && command->cmdsize >= sizeof(uuid_command))
				return std::equal(kQtitanUuid.begin(), kQtitanUuid.end(),
				    reinterpret_cast<const uuid_command*>(command)->uuid);
			cursor += command->cmdsize;
		}
		return false;
	}

	// Exact scalar bool returns and QString object-address arguments verified in
	// the pinned image's save/load file helpers; no autoFormatting argument.
	using StateFile = bool (*)(rml::qt::QObject*, const void* qstring);
	using ManagedWidget = const rml::qt::QWidget* (*)(const rml::qt::QObject*);
	struct Binding
	{
		LibraryHandle qtitan;
		LibraryHandle core;
		StateFile save{};
		StateFile load{};
		ManagedWidget managed_widget{};
		rml::workspace::support::GuardedDockingContext context;
		std::string reason;
	};

	[[nodiscard]] bool prepare_binding(Binding& binding, std::ostream& report)
	{
		auto fail = [&](const char* reason) { binding.reason = reason; return false; };
		if (!pthread_main_np()) return fail("not_process_main_thread");
		std::uint32_t capacity = 1024;
		std::vector<char> path(capacity);
		if (_NSGetExecutablePath(path.data(), &capacity) != 0)
		{
			path.resize(capacity);
			if (_NSGetExecutablePath(path.data(), &capacity) != 0) return fail("no_executable_path");
		}
		const auto executable = std::filesystem::weakly_canonical(path.data());
		const auto app = executable.parent_path().parent_path().parent_path();
		if (executable.filename() != "RobloxStudio" || app.filename() != "RMLWorkspaceTest.app")
			return fail("not_test_studio_copy");
		binding.context.reports_directory = executable.parent_path()
		    / "RobloxModLoader/mods" / RML_WORKSPACE_MOD_DIRECTORY / "reports";
		if (std::filesystem::weakly_canonical(binding.context.reports_directory)
		    != binding.context.reports_directory) return fail("reports_directory_redirected");
		const auto core_path = app / "Contents/Frameworks/QtCore.framework/Versions/5/QtCore";
		binding.core.open(core_path);
		if (!binding.core.value) return fail("bundled_qtcore_not_loaded");
		const auto qversion = reinterpret_cast<const char* (*)()>(dlsym(binding.core.value, "qVersion"));
		Dl_info core_info{};
		if (!qversion || !dladdr(reinterpret_cast<const void*>(qversion), &core_info)
		    || !core_info.dli_fname || std::filesystem::weakly_canonical(core_info.dli_fname) != core_path)
			return fail("unsupported_qtcore_image");
		const char* version = qversion();
		if (!version || std::string_view(version) != "5.15.9") return fail("unsupported_qt_version");
		const auto library = executable.parent_path() / "libQtitanDocking.dylib";
		binding.qtitan.open(library);
		if (!binding.qtitan.value) return fail("qtitan_not_loaded");
		binding.save = reinterpret_cast<StateFile>(dlsym(binding.qtitan.value,
		    "_ZN6Qtitan16DockPanelManager15saveStateToFileERK7QString"));
		binding.load = reinterpret_cast<StateFile>(dlsym(binding.qtitan.value,
		    "_ZN6Qtitan16DockPanelManager17loadStateFromFileERK7QString"));
		binding.managed_widget = reinterpret_cast<ManagedWidget>(dlsym(binding.qtitan.value,
		    "_ZNK6Qtitan16DockPanelManager13managedWidgetEv"));
		if (!binding.save || !binding.load || !binding.managed_widget) return fail("missing_qtitan_export");
		void* image = nullptr;
		for (const auto function : {reinterpret_cast<const void*>(binding.save),
		    reinterpret_cast<const void*>(binding.load), reinterpret_cast<const void*>(binding.managed_widget)})
		{
			Dl_info info{};
			if (!dladdr(function, &info) || !info.dli_fname
			    || std::filesystem::weakly_canonical(info.dli_fname) != library
			    || !has_expected_uuid(info.dli_fbase) || (image && image != info.dli_fbase))
				return fail("unsupported_qtitan_image");
			image = info.dli_fbase;
		}
		auto& context = binding.context;
		context.widgets = rml::workspace::support::collect_qt_widgets();
		if (!context.widgets.skip_reason.empty() || context.widgets.qt_version != "5.15.9"
		    || context.widgets.total_widgets != context.widgets.widgets.size()
		    || std::find(context.widgets.widgets.begin(), context.widgets.widgets.end(), nullptr)
		        != context.widgets.widgets.end()) return fail("qt_widget_scan_incomplete");
		context.objects = rml::workspace::support::collect_qt_objects(context.widgets);
		if (!context.objects.skip_reason.empty() || !context.objects.scan_error.empty()
		    || context.objects.truncated || context.objects.null_entries != 0
		    || context.objects.roots_total == 0
		    || context.objects.roots_total != context.objects.roots_inspected
		    || context.objects.roots_inspected != context.objects.main_windows.size())
			return fail("qt_object_scan_incomplete");
		std::size_t matching_managers = 0;
		for (const auto& record : context.objects.objects)
		{
			if (!record.object || record.class_name.empty() || record.root_id == 0
			    || record.root_id > context.objects.main_windows.size()) return fail("qt_object_scan_incomplete");
			if (record.class_name != "Qtitan::DockPanelManager") continue;
			const auto* managed = binding.managed_widget(record.object);
			const auto managed_record = std::find_if(context.objects.objects.begin(), context.objects.objects.end(),
			    [managed](const auto& candidate) { return candidate.object == managed; });
			const bool is_widget = managed && std::find(context.widgets.widgets.begin(),
			    context.widgets.widgets.end(), managed) != context.widgets.widgets.end();
			const bool same_root = managed_record != context.objects.objects.end()
			    && managed_record->root_id == record.root_id;
			report << "docking_manager_root=" << record.root_id << '\n'
			       << "docking_managed_is_widget=" << (is_widget ? "true" : "false") << '\n'
			       << "docking_managed_same_root=" << (same_root ? "true" : "false") << '\n';
			if (is_widget && same_root)
			{
				++matching_managers;
				context.manager = const_cast<rml::qt::QObject*>(record.object);
				context.main_window = context.objects.main_windows[record.root_id - 1];
			}
		}
		report << "docking_matching_managers=" << matching_managers << '\n';
		if (matching_managers != 1) return fail("manager_identity_ambiguous");
		report << "docking_guard=passed\ndocking_qt_version=5.15.9\n";
		return true;
	}

	[[nodiscard]] bool is_report_path(const Binding& binding, const std::filesystem::path& path)
	{
		return path.is_absolute() && path.extension() == ".xml" && !path.filename().empty()
		    && path.native().find('\0') == std::string::npos
		    && std::filesystem::weakly_canonical(path.parent_path()) == binding.context.reports_directory;
	}

	struct Snapshot
	{
		std::string bytes;
		std::string canonical_xml;
		std::string reason;
		std::size_t panels{};
		std::size_t windows{};
		[[nodiscard]] bool valid() const { return reason.empty() && !canonical_xml.empty(); }
	};

	[[nodiscard]] std::string attribute(const xmlNode* node, const char* name)
	{
		XmlText value(xmlGetProp(node, reinterpret_cast<const xmlChar*>(name)));
		return value ? reinterpret_cast<const char*>(value.get()) : "";
	}

	[[nodiscard]] bool named(const xmlNode* node, const char* name)
	{
		return node && node->type == XML_ELEMENT_NODE && !node->ns
		    && xmlStrEqual(node->name, reinterpret_cast<const xmlChar*>(name));
	}

	[[nodiscard]] bool count_attribute(const xmlNode* node, std::size_t& count)
	{
		const auto text = attribute(node, "Count");
		if (text.empty()) return false;
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
		return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && count <= kMaxPanels;
	}

	// Native XML parser, not a hand-written parser or guessed Qt object layout.
	// libxml2 C APIs/layouts come from the macOS SDK's parser.h/tree.h/c14n.h:
	// https://gnome.pages.gitlab.gnome.org/libxml2/html/parser_8h.html
	// This validates the observed generated 1.3 envelope, panel counts, unique
	// IDs and references. It does not claim to implement Qtitan's complete schema.
	[[nodiscard]] Snapshot validate_bytes(std::string bytes)
	{
		Snapshot result;
		result.bytes = std::move(bytes);
		auto fail = [&](const char* reason) { result.reason = reason; return std::move(result); };
		if (result.bytes.empty() || result.bytes.size() > kMaxSnapshotBytes) return fail("empty_or_oversize_snapshot");
		if (!result.bytes.starts_with("<?xml") || result.bytes.find("<!DOCTYPE") != std::string::npos
		    || result.bytes.find("<!ENTITY") != std::string::npos) return fail("unsupported_xml_envelope");
		XmlDocument document(xmlReadMemory(result.bytes.data(), static_cast<int>(result.bytes.size()),
		    nullptr, "UTF-8", XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NOBLANKS), xmlFreeDoc);
		if (!document || document->intSubset || document->extSubset) return fail("invalid_xml");
		const xmlNode* root = xmlDocGetRootElement(document.get());
		if (!named(root, "DockPanelLayouts") || attribute(root, "type") != "layoutPanel"
		    || attribute(root, "version") != "1.3") return fail("unsupported_layout_root");
		if (!root->nsDef) return fail("missing_qtitan_namespace");
		for (const xmlNs* ns = root->nsDef; ns; ns = ns->next)
			if (!xmlStrEqual(ns->prefix, reinterpret_cast<const xmlChar*>("Qtitan"))
			    || !xmlStrEqual(ns->href, reinterpret_cast<const xmlChar*>("https://www.devmachines.com/qt/qtitan")))
				return fail("unsupported_layout_namespace");
		std::map<std::string, const xmlNode*> sections;
		for (const xmlNode* node = root->children; node; node = node->next)
		{
			if (node->type != XML_ELEMENT_NODE || node->ns) return fail("unexpected_root_content");
			const std::string name(reinterpret_cast<const char*>(node->name));
			if (name != "Panels" && name != "Windows" && name != "TopLayout" && name != "Client")
				return fail("unexpected_root_section");
			if (!sections.emplace(name, node).second) return fail("duplicate_root_section");
		}
		if (sections.size() != 4 || !count_attribute(sections.at("Panels"), result.panels)
		    || !count_attribute(sections.at("Windows"), result.windows) || result.panels == 0)
			return fail("missing_or_invalid_root_sections");
		std::set<std::string> ids;
		std::vector<const xmlNode*> declarations;
		for (const xmlNode* panel = sections.at("Panels")->children; panel; panel = panel->next)
		{
			if (!named(panel, "Panel")) return fail("unexpected_panel_declaration");
			const std::string id = attribute(panel, "ID");
			if (id.empty() || attribute(panel, "Type").empty() || !ids.insert(id).second)
				return fail("invalid_or_duplicate_panel_id");
			declarations.push_back(panel);
		}
		if (declarations.size() != result.panels) return fail("panel_count_mismatch");
		std::size_t window_count = 0;
		for (const xmlNode* node = sections.at("Windows")->children; node; node = node->next)
		{
			if (node->type != XML_ELEMENT_NODE || node->ns) return fail("unexpected_window_content");
			++window_count;
		}
		if (window_count != result.windows) return fail("window_count_mismatch");
		for (const char* section : {"TopLayout", "Client"})
			if (!ids.contains(attribute(sections.at(section), "ID"))) return fail("invalid_root_panel_reference");
		for (const auto* panel : declarations)
		{
			if (!panel->children || panel->children->next) return fail("missing_or_multiple_panel_sections");
			for (const xmlNode* child = panel->children; child; child = child->next)
			{
				if (named(child, "Panels"))
				{
					std::size_t expected = 0, actual = 0;
					if (!count_attribute(child, expected)) return fail("invalid_nested_panel_count");
					for (const xmlNode* ref = child->children; ref; ref = ref->next)
					{
						if (!named(ref, "Panel") || !ids.contains(attribute(ref, "ID"))) return fail("invalid_panel_reference");
						++actual;
					}
					if (actual != expected) return fail("nested_panel_count_mismatch");
				}
				else if (named(child, "Layouts"))
				{
					std::set<std::string> reference_kinds;
					for (const xmlNode* ref = child->children; ref; ref = ref->next)
					{
						if (!named(ref, "DockingLayout") && !named(ref, "FloatingLayout") && !named(ref, "LastLayout"))
							return fail("unexpected_layout_reference");
						if (!reference_kinds.emplace(reinterpret_cast<const char*>(ref->name)).second
						    || !xmlHasProp(ref, reinterpret_cast<const xmlChar*>("ID")))
							return fail("missing_or_duplicate_layout_reference");
						const std::string id = attribute(ref, "ID");
						if (!id.empty() && !ids.contains(id)) return fail("invalid_layout_reference");
					}
					if (reference_kinds.size() != 3) return fail("missing_layout_reference");
				}
				else return fail("unexpected_panel_content");
			}
		}
		xmlChar* canonical = nullptr;
		const int size = xmlC14NDocDumpMemory(document.get(), nullptr, XML_C14N_1_0, nullptr, 0, &canonical);
		XmlText canonical_owner(canonical);
		if (size <= 0 || static_cast<std::size_t>(size) > kMaxSnapshotBytes || !canonical)
			return fail("xml_canonicalization_failed");
		result.canonical_xml.assign(reinterpret_cast<const char*>(canonical), static_cast<std::size_t>(size));
		return result;
	}

	[[nodiscard]] Snapshot read_snapshot(const std::filesystem::path& path)
	{
		Snapshot result;
		FileHandle file(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
		struct stat state{};
		if (file.value < 0 || fstat(file.value, &state) != 0 || !S_ISREG(state.st_mode)
		    || state.st_nlink != 1 || state.st_uid != getuid())
		{ result.reason = "missing_or_unsafe_snapshot_file"; return result; }
		if (state.st_size <= 0 || static_cast<std::uintmax_t>(state.st_size) > kMaxSnapshotBytes)
		{ result.reason = "empty_or_oversize_snapshot"; return result; }
		std::string bytes(static_cast<std::size_t>(state.st_size), '\0');
		std::size_t offset = 0;
		while (offset < bytes.size())
		{
			const auto received = read(file.value, bytes.data() + offset, bytes.size() - offset);
			if (received < 0 && errno == EINTR) continue;
			if (received <= 0) { result.reason = "snapshot_read_failed"; return result; }
			offset += static_cast<std::size_t>(received);
		}
		char extra;
		if (read(file.value, &extra, 1) != 0) { result.reason = "snapshot_changed_while_reading"; return result; }
		return validate_bytes(std::move(bytes));
	}

	void append_validation(std::ostream& report, const Snapshot& snapshot)
	{
		report << "layout_snapshot_bytes=" << snapshot.bytes.size() << '\n'
		       << "layout_snapshot_xml_valid=" << (snapshot.valid() ? "true" : "false") << '\n'
		       << "layout_snapshot_panel_count=" << snapshot.panels << '\n'
		       << "layout_snapshot_window_count=" << snapshot.windows << '\n';
	}

	struct Operation
	{
		bool succeeded{};
		bool native_called{};
		bool native_result{};
		Snapshot snapshot;
		std::string report;
	};

	[[nodiscard]] Operation operate_snapshot(const std::filesystem::path& path, const bool restore,
	    const Snapshot* expected = nullptr)
	{
		Operation operation;
		std::ostringstream report;
		report << "layout_snapshot_format=vfx-qtitan-snapshot-v2\n"
		       << "layout_snapshot_operation=" << (restore ? "restore" : "export") << '\n';
		auto finish = [&](const char* status, const std::string& reason) {
			report << "layout_snapshot_native_called=" << (operation.native_called ? "true" : "false") << '\n'
			       << "layout_snapshot_native_result=" << (operation.native_result ? "true" : "false") << '\n'
			       << "layout_snapshot=" << status << '\n';
			if (!reason.empty()) report << "layout_snapshot_reason=" << std::quoted(reason) << '\n';
			operation.report = report.str();
			return std::move(operation);
		};
		try
		{
			Binding binding;
			if (!prepare_binding(binding, report)) return finish("skipped", binding.reason);
			if (!is_report_path(binding, path)) return finish("skipped", "path_outside_own_report_directory");
			report << "layout_snapshot_file=" << std::quoted(path.string()) << '\n';
			if (restore)
			{
				operation.snapshot = read_snapshot(path);
				append_validation(report, operation.snapshot);
				if (!operation.snapshot.valid()) return finish("rejected", operation.snapshot.reason);
				if (expected && operation.snapshot.bytes != expected->bytes) return finish("rejected", "fresh_snapshot_changed_before_load");
			}
			else
			{
				std::filesystem::create_directories(binding.context.reports_directory);
				FileHandle reserved(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
				if (reserved.value < 0) return finish("rejected", "invalid_or_existing_output_path");
			}
			const rml::qt::QString filename(path.string());
			if (filename.to_utf8() != path.string()) return finish("rejected", "qstring_filename_conversion_failed");
			operation.native_called = true;
			operation.native_result = (restore ? binding.load : binding.save)(binding.context.manager, filename.data());
			if (!restore)
			{
				operation.snapshot = read_snapshot(path);
				append_validation(report, operation.snapshot);
			}
			operation.succeeded = operation.native_result && operation.snapshot.valid();
			if (!operation.succeeded) return finish("failed", !operation.snapshot.valid()
			    ? operation.snapshot.reason : "native_state_file_returned_false");
			return finish(restore ? "restored" : "exported", "");
		}
		catch (...) { return finish("failed", "cxx_exception"); }
	}
}

namespace rml::workspace::support
{
	std::string with_guarded_docking_manager(const std::function<std::string(const GuardedDockingContext&)>& callback)
	{
		std::ostringstream report;
		try
		{
			Binding binding;
			if (!prepare_binding(binding, report))
			{ report << "docking_guard=skipped\ndocking_guard_reason=" << std::quoted(binding.reason) << '\n'; return report.str(); }
			if (!callback) { report << "docking_callback=skipped\ndocking_callback_reason=empty_callback\n"; return report.str(); }
			report << callback(binding.context);
		}
		catch (...) { report << "docking_callback=failed\ndocking_callback_reason=cxx_exception\n"; }
		return report.str();
	}

	std::string export_layout_snapshot(const std::filesystem::path& output)
	{ return operate_snapshot(output, false).report; }

	std::string restore_layout_snapshot(const std::filesystem::path& snapshot)
	{ return operate_snapshot(snapshot, true).report; }

	std::string roundtrip_layout_snapshot(const std::filesystem::path& before, const std::filesystem::path& after)
	{
		std::ostringstream report;
		report << "layout_roundtrip_format=vfx-qtitan-roundtrip-v1\n";
		try
		{
			// Validate both paths and ensure the second export cannot overwrite an
			// existing file before any native save/load operation is requested.
			{
				Binding binding;
				if (!prepare_binding(binding, report) || !is_report_path(binding, before) || !is_report_path(binding, after)
				    || before.lexically_normal() == after.lexically_normal()
				    || std::filesystem::exists(before) || std::filesystem::exists(after)
				    || std::filesystem::is_symlink(std::filesystem::symlink_status(before))
				    || std::filesystem::is_symlink(std::filesystem::symlink_status(after)))
				{
					report << "layout_roundtrip=skipped\nlayout_roundtrip_reason="
					       << std::quoted(binding.reason.empty() ? "invalid_or_existing_roundtrip_paths" : binding.reason) << '\n';
					return report.str();
				}
			}
			const Operation exported = operate_snapshot(before, false);
			report << "layout_roundtrip_stage=before_export\n" << exported.report;
			if (!exported.succeeded)
			{ report << "layout_roundtrip=failed\nlayout_roundtrip_reason=before_export_failed\n"; return report.str(); }
			const Operation restored = operate_snapshot(before, true, &exported.snapshot);
			report << "layout_roundtrip_stage=unchanged_load\n" << restored.report;
			if (!restored.native_called)
			{ report << "layout_roundtrip=failed\nlayout_roundtrip_reason=load_rejected_before_native_call\n"; return report.str(); }
			// A false native load can still have changed internal state; preserve a
			// diagnostic after-export and report failure instead of assuming rollback.
			const Operation after_export = operate_snapshot(after, false);
			report << "layout_roundtrip_stage=after_export\n" << after_export.report;
			const bool byte_equal = after_export.succeeded && exported.snapshot.bytes == after_export.snapshot.bytes;
			const bool xml_equal = after_export.succeeded
			    && exported.snapshot.canonical_xml == after_export.snapshot.canonical_xml;
			report << "layout_roundtrip_byte_equal=" << (byte_equal ? "true" : "false") << '\n'
			       << "layout_roundtrip_xml_equal=" << (xml_equal ? "true" : "false") << '\n'
			       << "layout_roundtrip_comparison=xml_c14n_1_0_without_formatting_whitespace\n"
			       << "layout_roundtrip=" << (restored.succeeded && after_export.succeeded && xml_equal ? "passed" : "failed") << '\n';
			if (!restored.succeeded) report << "layout_roundtrip_reason=native_load_failed\n";
			else if (!after_export.succeeded) report << "layout_roundtrip_reason=after_export_failed\n";
			else if (!xml_equal) report << "layout_roundtrip_reason=serialized_layout_changed\n";
		}
		catch (...) { report << "layout_roundtrip=failed\nlayout_roundtrip_reason=cxx_exception\n"; }
		return report.str();
	}
}
