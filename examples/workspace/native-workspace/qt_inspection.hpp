#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rml::qt
{
	class QObject;
	class QWidget;
}

namespace rml::workspace::support
{
	// Pointers are transient: collect and inspect synchronously on the GUI thread,
	// without processing events or retaining this collection for a later callback.
	struct QtWidgetCollection
	{
		std::vector<rml::qt::QWidget*> widgets;
		std::size_t total_widgets{};
		std::string qt_version;
		std::string skip_reason;
	};

	struct QtObjectRecord
	{
		const rml::qt::QObject* object{};
		std::string class_name;
		std::size_t root_id{};
	};

	struct QtObjectCollection
	{
		std::vector<QtObjectRecord> objects;
		std::vector<const rml::qt::QWidget*> main_windows;
		std::size_t roots_total{};
		std::size_t roots_inspected{};
		std::size_t descendant_entries_enumerated{};
		std::size_t null_entries{};
		bool truncated{};
		std::string skip_reason;
		std::string scan_error;
	};

	[[nodiscard]] QtWidgetCollection collect_qt_widgets();
	[[nodiscard]] QtObjectCollection collect_qt_objects(const QtWidgetCollection& collection);
	[[nodiscard]] std::string inspect_qt_objects(const QtWidgetCollection& collection);
}
