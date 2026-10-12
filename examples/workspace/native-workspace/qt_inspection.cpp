#include "qt_inspection.hpp"

#include <RobloxModLoader/memory/foreign_call.hpp>
#include <RobloxModLoader/qt/qmetaobject.hpp>
#include <RobloxModLoader/qt/qwidget.hpp>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <map>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace
{
	constexpr std::size_t kMaxWidgets = 8192;
	constexpr std::size_t kMaxObjects = 8192;
	constexpr std::size_t kMaxCandidates = 64;
	constexpr int kMaxMethods = 512;
	constexpr std::string_view kSupportedQt = "5.15.9";

	// These declarations mirror the actual Qt 5.15.9 fields, not QObject private
	// offsets. Sources (tag v5.15.9-lts-lgpl):
	// https://raw.githubusercontent.com/qt/qtbase/v5.15.9-lts-lgpl/src/corelib/kernel/qmetaobject.h
	// https://raw.githubusercontent.com/qt/qtbase/v5.15.9-lts-lgpl/src/corelib/tools/qlist.h
	// https://raw.githubusercontent.com/qt/qtbase/v5.15.9-lts-lgpl/src/corelib/tools/qarraydata.h
	// RefCount contains QBasicAtomicInt; the C++11 atomic backend stores
	// std::atomic<int> (tools/qrefcount.h and thread/qatomic_cxx11.h).
	struct QtListData
	{
		std::atomic<int> ref;
		int alloc;
		int begin;
		int end;
		void* array[1];
	};

	struct QtArrayData
	{
		std::atomic<int> ref;
		int size;
		unsigned int alloc : 31;
		unsigned int capacity_reserved : 1;
		std::ptrdiff_t offset;
	};

	struct QtMetaMethodValue
	{
		const rml::qt::QMetaObject* mobj;
		unsigned int handle;
	};

	static_assert(sizeof(void*) == 8 && sizeof(int) == 4 && sizeof(unsigned int) == 4);
	static_assert(sizeof(std::atomic<int>) == 4 && alignof(std::atomic<int>) == 4);
	static_assert(std::atomic<int>::is_always_lock_free);
	static_assert(std::is_standard_layout_v<QtListData> && sizeof(QtListData) == 24);
	static_assert(offsetof(QtListData, ref) == 0 && offsetof(QtListData, alloc) == 4);
	static_assert(offsetof(QtListData, begin) == 8 && offsetof(QtListData, end) == 12);
	static_assert(offsetof(QtListData, array) == 16 && alignof(QtListData) == 8);
	static_assert(std::is_standard_layout_v<QtArrayData> && sizeof(QtArrayData) == 24);
	static_assert(offsetof(QtArrayData, ref) == 0 && offsetof(QtArrayData, size) == 4);
	static_assert(offsetof(QtArrayData, offset) == 16 && alignof(QtArrayData) == 8);
	static_assert(std::is_trivially_copyable_v<QtMetaMethodValue>);
	static_assert(sizeof(QtMetaMethodValue) == 16 && alignof(QtMetaMethodValue) == 8);
	static_assert(offsetof(QtMetaMethodValue, mobj) == 0 && offsetof(QtMetaMethodValue, handle) == 8);

	// Returns whether to dispose, the inverse of QtPrivate::RefCount::deref().
	// Refcount -1 is static; 0 is unsharable.
	[[nodiscard]] bool release_ref(std::atomic<int>& ref) noexcept
	{
		const int count = ref.load(std::memory_order_relaxed);
		return count != -1 && (count == 0 || ref.fetch_sub(1, std::memory_order_acq_rel) == 1);
	}

	using DisposeList = void (*)(QtListData*);
	using DeallocateArray = void (*)(QtArrayData*, std::size_t, std::size_t);

	class ListOwner
	{
	public:
		ListOwner(QtListData* shared_null, DisposeList dispose) : data(shared_null), dispose_(dispose) {}
		~ListOwner() { if (data && release_ref(data->ref)) dispose_(data); }
		ListOwner(const ListOwner&) = delete;
		ListOwner& operator=(const ListOwner&) = delete;
		[[nodiscard]] bool valid() const noexcept
		{
			return data && data->alloc >= 0 && data->begin >= 0
			    && data->end >= data->begin && data->end <= data->alloc;
		}
		[[nodiscard]] int size() const noexcept { return data->end - data->begin; }
		[[nodiscard]] void* at(int index) const noexcept { return data->array[data->begin + index]; }
		QtListData* data;
	private:
		DisposeList dispose_;
	};

	class ArrayOwner
	{
	public:
		ArrayOwner(QtArrayData* shared_null, DeallocateArray deallocate, std::size_t element_size)
		    : data(shared_null), deallocate_(deallocate), element_size_(element_size) {}
		~ArrayOwner()
		{
			// QTypedArrayData<T>::AlignmentDummy has QArrayData alignment for char
			// and ushort. Pointer elements in ListOwner require no destruction.
			if (data && release_ref(data->ref)) deallocate_(data, element_size_, alignof(QtArrayData));
		}
		ArrayOwner(const ArrayOwner&) = delete;
		ArrayOwner& operator=(const ArrayOwner&) = delete;
		QtArrayData* data;
	private:
		DeallocateArray deallocate_;
		std::size_t element_size_;
	};

	[[nodiscard]] std::string bounded_text(const char* text, const std::size_t limit)
	{
		if (!text) return {};
		std::string result;
		for (std::size_t index = 0; index < limit && text[index] != '\0'; ++index)
		{
			const unsigned char value = static_cast<unsigned char>(text[index]);
			result += value < 32 || value == 127 ? '?' : static_cast<char>(value);
		}
		return result;
	}

	struct QtBindings
	{
		void* core{};
		void* widgets{};
		std::string version;
		std::string skip_reason;
		QtListData* list_null{};
		QtArrayData* array_null{};
		DisposeList dispose_list{};
		DeallocateArray deallocate_array{};
		void* all_widgets{};
		void* method_signature{};
		rml::qt::QObject* const* application{};
		const rml::qt::QMetaObject* object_meta{};
		const char* (*class_name)(const rml::qt::QMetaObject*){};
		void* (*current_thread)(){};
		void* (*object_thread)(const rml::qt::QObject*){};
		void (*find_children)(const rml::qt::QObject*, const void*, const rml::qt::QMetaObject*, void*, int){};
		int (*method_count)(const rml::qt::QMetaObject*){};
		int (*method_offset)(const rml::qt::QMetaObject*){};
		QtMetaMethodValue (*method)(const rml::qt::QMetaObject*, int){};
		int (*method_type)(const QtMetaMethodValue*){};
		int (*parameter_count)(const QtMetaMethodValue*){};
		const char* (*type_name)(const QtMetaMethodValue*){};

		~QtBindings()
		{
			if (widgets) dlclose(widgets);
			if (core) dlclose(core);
		}
		QtBindings(const QtBindings&) = delete;
		QtBindings& operator=(const QtBindings&) = delete;

		template<typename T>
		void resolve(T& destination, void* image, const char* symbol)
		{
			destination = reinterpret_cast<T>(dlsym(image, symbol));
			if (!destination && skip_reason.empty()) skip_reason = std::string("missing_symbol:") + symbol;
		}

		QtBindings()
		{
			if (!pthread_main_np()) { skip_reason = "not_process_main_thread"; return; }
			std::uint32_t capacity = 1024;
			std::vector<char> path(capacity);
			if (_NSGetExecutablePath(path.data(), &capacity) != 0)
			{
				path.resize(capacity);
				if (_NSGetExecutablePath(path.data(), &capacity) != 0)
				{ skip_reason = "executable_path_unavailable"; return; }
			}
			const auto frameworks = std::filesystem::path(path.data()).parent_path().parent_path() / "Frameworks";
			const auto core_path = frameworks / "QtCore.framework/Versions/5/QtCore";
			core = dlopen(core_path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
			if (!core) { skip_reason = "bundled_qtcore_not_loaded"; return; }
			const char* (*qversion)() = nullptr;
			resolve(qversion, core, "qVersion");
			if (!skip_reason.empty()) return;
			version = bounded_text(qversion(), 64);
			if (version != kSupportedQt) { skip_reason = "unsupported_qt_version"; return; }

			resolve(list_null, core, "_ZN9QListData11shared_nullE");
			resolve(array_null, core, "_ZN10QArrayData11shared_nullE");
			resolve(dispose_list, core, "_ZN9QListData7disposeEPNS_4DataE");
			resolve(deallocate_array, core, "_ZN10QArrayData10deallocateEPS_mm");
			resolve(application, core, "_ZN16QCoreApplication4selfE");
			resolve(current_thread, core, "_ZN7QThread13currentThreadEv");
			resolve(object_thread, core, "_ZNK7QObject6threadEv");
			resolve(object_meta, core, "_ZN7QObject16staticMetaObjectE");
			resolve(class_name, core, "_ZNK11QMetaObject9classNameEv");
			resolve(find_children, core,
			    "_Z23qt_qFindChildren_helperPK7QObjectRK7QStringRK11QMetaObjectP5QListIPvE6QFlagsIN2Qt15FindChildOptionEE");
			resolve(method_count, core, "_ZNK11QMetaObject11methodCountEv");
			resolve(method_offset, core, "_ZNK11QMetaObject12methodOffsetEv");
			resolve(method, core, "_ZNK11QMetaObject6methodEi");
			resolve(method_signature, core, "_ZNK11QMetaMethod15methodSignatureEv");
			resolve(method_type, core, "_ZNK11QMetaMethod10methodTypeEv");
			resolve(parameter_count, core, "_ZNK11QMetaMethod14parameterCountEv");
			resolve(type_name, core, "_ZNK11QMetaMethod8typeNameEv");
			if (!skip_reason.empty()) return;
			if (!*application) { skip_reason = "no_qapplication"; return; }
			if (current_thread() != object_thread(*application))
			{ skip_reason = "not_qapplication_thread"; return; }
			// Validate static container headers before constructing either value type.
			if (list_null->ref.load(std::memory_order_relaxed) != -1 || list_null->alloc != 0
			    || list_null->begin != 0 || list_null->end != 0
			    || array_null->ref.load(std::memory_order_relaxed) != -1 || array_null->size != 0)
			{ skip_reason = "qt_shared_null_layout_mismatch"; return; }
			const auto widgets_path = frameworks / "QtWidgets.framework/Versions/5/QtWidgets";
			widgets = dlopen(widgets_path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
			if (!widgets) { skip_reason = "bundled_qtwidgets_not_loaded"; return; }
			resolve(all_widgets, widgets, "_ZN12QApplication10allWidgetsEv");
		}
	};

	using ObjectRecord = rml::workspace::support::QtObjectRecord;

	[[nodiscard]] const char* method_kind(const int kind)
	{
		switch (kind)
		{
		case 0: return "method";
		case 1: return "signal";
		case 2: return "slot";
		case 3: return "constructor";
		default: return "unknown";
		}
	}

	void append_methods(std::ostream& report, const QtBindings& qt, const ObjectRecord& record, std::size_t id)
	{
		const auto* meta = record.object->metaObject();
		if (!meta) { report << "qt_method_candidate " << id << " scan=skipped reason=no_metaobject\n"; return; }
		const int total = qt.method_count(meta);
		const int offset = qt.method_offset(meta);
		if (total < 0 || offset < 0 || offset > total)
		{ report << "qt_method_candidate " << id << " scan=skipped reason=invalid_method_range\n"; return; }
		const int count = std::min(total, kMaxMethods);
		std::size_t save_matches = 0;
		std::size_t load_matches = 0;
		report << "qt_method_candidate " << id << " class=" << std::quoted(record.class_name)
		       << " total=" << total << " local_offset=" << offset << " inspected=" << count
		       << " truncated=" << (total > count ? "true" : "false") << '\n';
		for (int index = 0; index < count; ++index)
		{
			// Qt 5.15.9 QMetaMethod uses a trivial two-register return on arm64,
			// unlike the nontrivial QByteArray and QList indirect returns.
			const QtMetaMethodValue method = qt.method(meta, index);
			if (!method.mobj) { report << "qt_method " << id << ':' << index << " valid=false\n"; continue; }
			ArrayOwner signature(qt.array_null, qt.deallocate_array, sizeof(char));
			rml::memory::call_returning_member(qt.method_signature, signature.data, &method);
			if (!signature.data || signature.data->size < 0)
			{ report << "qt_method " << id << ':' << index << " valid=false reason=invalid_signature\n"; continue; }
			const auto signature_length = std::min<std::size_t>(signature.data->size, 512);
			const char* bytes = reinterpret_cast<const char*>(signature.data) + signature.data->offset;
			std::string text(bytes, signature_length);
			for (char& value : text) if (static_cast<unsigned char>(value) < 32 || value == 127) value = '?';
			const std::string name = text.substr(0, text.find('('));
			std::string lowered = name;
			for (char& value : lowered) if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
			const bool save = lowered.find("save") != std::string::npos;
			const bool load = lowered.find("load") != std::string::npos;
			save_matches += save;
			load_matches += load;
			report << "qt_method " << id << ':' << index << " name=" << std::quoted(name)
			       << " signature=" << std::quoted(text) << " kind=" << method_kind(qt.method_type(&method))
			       << " parameters=" << qt.parameter_count(&method)
			       << " return=" << std::quoted(bounded_text(qt.type_name(&method), 256))
			       << " inherited=" << (index < offset ? "true" : "false")
			       << " signature_truncated=" << (signature.data->size > 512 ? "true" : "false")
			       << " save_name_match=" << (save ? "true" : "false")
			       << " load_name_match=" << (load ? "true" : "false") << '\n';
		}
		report << "qt_method_matches " << id << " save=" << save_matches << " load=" << load_matches
		       << " basis=case_insensitive_name_substring scope=inspected_registered_methods\n";
	}
}

namespace rml::workspace::support
{
	QtWidgetCollection collect_qt_widgets()
	{
		QtWidgetCollection collection;
		try
		{
			QtBindings qt;
			collection.qt_version = qt.version;
			collection.skip_reason = qt.skip_reason;
			if (!collection.skip_reason.empty()) return collection;
			ListOwner list(qt.list_null, qt.dispose_list);
			// QList<T> has a nontrivial destructor and a single Data* member.
			rml::memory::call_returning(qt.all_widgets, list.data);
			if (!list.valid()) { collection.skip_reason = "invalid_widget_list"; return collection; }
			collection.total_widgets = static_cast<std::size_t>(list.size());
			const auto count = std::min(collection.total_widgets, kMaxWidgets);
			collection.widgets.reserve(count);
			for (std::size_t index = 0; index < count; ++index)
				collection.widgets.push_back(static_cast<rml::qt::QWidget*>(list.at(static_cast<int>(index))));
		}
		catch (...)
		{
			collection.widgets.clear();
			collection.skip_reason = "cxx_exception_collecting_widgets";
		}
		return collection;
	}

	QtObjectCollection collect_qt_objects(const QtWidgetCollection& collection)
	{
		QtObjectCollection objects;
		try
		{
			QtBindings qt;
			objects.skip_reason = !qt.skip_reason.empty() ? qt.skip_reason : collection.skip_reason;
			if (!objects.skip_reason.empty()) return objects;
			std::vector<const rml::qt::QObject*> roots;
			std::unordered_set<const rml::qt::QObject*> seen;
			std::unordered_set<const rml::qt::QObject*> seen_roots;
			for (const auto* widget : collection.widgets)
			{
				if (!widget) continue;
				const auto* meta = widget->metaObject();
				if (meta && bounded_text(qt.class_name(meta), 256) == "RBX::Studio::MainWindow"
				    && seen_roots.insert(widget).second)
				{
					++objects.roots_total;
					if (roots.size() < kMaxCandidates)
					{
						roots.push_back(widget);
						objects.main_windows.push_back(widget);
					}
				}
			}
			objects.roots_inspected = roots.size();
			if (roots.empty())
			{ objects.skip_reason = "studio_mainwindow_not_found_in_inspected_widgets"; return objects; }
			auto add_object = [&](const rml::qt::QObject* object, const std::size_t root_id) {
				if (!object) { ++objects.null_entries; return; }
				if (seen.contains(object)) return;
				if (objects.objects.size() == kMaxObjects) { objects.truncated = true; return; }
				seen.insert(object);
				const auto* meta = object->metaObject();
				objects.objects.push_back({object, meta ? bounded_text(qt.class_name(meta), 256) : "", root_id});
			};
			// Record all roots first, so every root ID exists even if descendants
			// reach the record cap or a MainWindow appears under another root.
			for (std::size_t index = 0; index < roots.size(); ++index) add_object(roots[index], index + 1);
			for (std::size_t root_index = 0; root_index < roots.size(); ++root_index)
			{
				ListOwner descendants(qt.list_null, qt.dispose_list);
				ArrayOwner null_name(qt.array_null, qt.deallocate_array, sizeof(char16_t));
				// QString's default constructor uses shared_null (text/qstring.h).
				// qobject.cpp's null-name short circuit never reads objectName().
				// qnamespace.h: FindChildrenRecursively = 0x1; qflags.h stores int.
				qt.find_children(roots[root_index], &null_name.data, qt.object_meta, &descendants.data, 1);
				if (!descendants.valid())
				{ objects.scan_error = "invalid_descendant_list"; return objects; }
				objects.descendant_entries_enumerated += static_cast<std::size_t>(descendants.size());
				for (int index = 0; index < descendants.size(); ++index)
					add_object(static_cast<const rml::qt::QObject*>(descendants.at(index)), root_index + 1);
			}
		}
		catch (...) { objects.scan_error = "cxx_exception_collecting_objects"; }
		return objects;
	}

	std::string inspect_qt_objects(const QtWidgetCollection& collection)
	{
		std::ostringstream report;
		report << "qt_inspection_format=vfx-qt-metadata-v1\n"
		       << "qt_runtime_version=" << std::quoted(collection.qt_version) << '\n'
		       << "qt_supported_version=" << std::quoted(std::string(kSupportedQt)) << '\n'
		       << "qt_widgets_total=" << collection.total_widgets << '\n'
		       << "qt_widgets_inspected=" << collection.widgets.size() << '\n'
		       << "qt_widgets_limit=" << kMaxWidgets << '\n'
		       << "qt_widgets_truncated=" << (collection.total_widgets > collection.widgets.size() ? "true" : "false") << '\n';
		try
		{
			QtBindings qt;
			const auto& reason = !qt.skip_reason.empty() ? qt.skip_reason : collection.skip_reason;
			if (!reason.empty())
			{ report << "qt_object_scan=skipped\nqt_skip_reason=" << std::quoted(reason) << '\n'; return report.str(); }
			const QtObjectCollection objects = collect_qt_objects(collection);
			const auto& records = objects.objects;
			report << "qt_object_scan=" << (!objects.scan_error.empty() ? "error" : !objects.skip_reason.empty() ? "skipped" : "complete") << '\n';
			if (!objects.skip_reason.empty()) report << "qt_skip_reason=" << std::quoted(objects.skip_reason) << '\n';
			if (!objects.scan_error.empty()) report << "qt_scan_error=" << std::quoted(objects.scan_error) << '\n';
			report << "qt_mainwindow_roots_total=" << objects.roots_total << '\n'
			       << "qt_mainwindow_roots_inspected=" << objects.roots_inspected << '\n'
			       << "qt_roots_truncated=" << (objects.roots_total > objects.roots_inspected ? "true" : "false") << '\n'
			       << "qt_descendant_entries_enumerated=" << objects.descendant_entries_enumerated << '\n'
			       << "qt_objects_inspected=" << records.size() << '\n'
			       << "qt_objects_limit=" << kMaxObjects << '\n'
			       << "qt_objects_truncated=" << (objects.truncated ? "true" : "false") << '\n'
			       << "qt_null_object_entries=" << objects.null_entries << '\n'
			       << "qt_enumeration_limit_note=helper_enumerates_each_root_before_record_cap\n";
			if (!objects.scan_error.empty()) return report.str();
			std::map<std::string, std::size_t> counts;
			std::vector<std::size_t> candidates;
			std::size_t candidate_total = 0;
			std::size_t exact_managers = 0;
			for (std::size_t index = 0; index < records.size(); ++index)
			{
				const auto& record = records[index];
				++counts[record.class_name];
				const bool manager = record.class_name.find("DockPanelManager") != std::string::npos;
				const bool mainwindow = record.class_name == "RBX::Studio::MainWindow";
				exact_managers += record.class_name == "Qtitan::DockPanelManager";
				report << "qt_object " << index + 1 << " class=" << std::quoted(record.class_name)
				       << " mainwindow_root=" << record.root_id << " dock_manager_candidate=" << (manager ? "true" : "false") << '\n';
				if (manager || mainwindow)
				{
					++candidate_total;
					if (candidates.size() < kMaxCandidates) candidates.push_back(index);
				}
			}
			report << "qt_class_summary_count=" << counts.size() << '\n';
			for (const auto& [name, count] : counts) report << "qt_class_count " << std::quoted(name) << '=' << count << '\n';
			report << "qt_exact_dockpanelmanagers=" << exact_managers << '\n'
			       << "qt_method_candidates_total=" << candidate_total << '\n'
			       << "qt_method_candidates_inspected=" << candidates.size() << '\n'
			       << "qt_method_candidates_limit=" << kMaxCandidates << '\n'
			       << "qt_method_candidates_truncated=" << (candidate_total > candidates.size() ? "true" : "false") << '\n'
			       << "qt_methods_limit_per_candidate=" << kMaxMethods << '\n'
			       << "qt_method_scope=registered_metadata_only\n";
			for (const auto index : candidates) append_methods(report, qt, records[index], index + 1);
		}
		catch (...)
		{
			report << "qt_object_scan=error\nqt_scan_error=cxx_exception_inspecting_metadata\n";
		}
		return report.str();
	}
}
