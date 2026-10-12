#pragma once

#include "qt_inspection.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace rml::workspace::support
{
	// GUI-thread-only layout support; supported only in the marked test Studio build.
	[[nodiscard]] std::string export_layout_snapshot(const std::filesystem::path& output);
	[[nodiscard]] std::string restore_layout_snapshot(const std::filesystem::path& snapshot);
	[[nodiscard]] std::string roundtrip_layout_snapshot(const std::filesystem::path& before,
	    const std::filesystem::path& after);

	struct GuardedDockingContext
	{
		rml::qt::QObject* manager{};
		const rml::qt::QWidget* main_window{};
		QtWidgetCollection widgets;
		QtObjectCollection objects;
		std::filesystem::path reports_directory;
	};

	// The callback runs synchronously on the GUI thread after all guards pass.
	// Never retain its pointers or process events. Collections describe entry
	// state; discover objects again after a call that can change the object tree.
	[[nodiscard]] std::string with_guarded_docking_manager(
	    const std::function<std::string(const GuardedDockingContext&)>& callback);
}
