#pragma once

#include "RobloxModLoader/qt/qobject.hpp"
#include "RobloxModLoader/rml_export.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rml::qt
{
	class QWidget;
	
	class RML_EXPORT QApplication : public QObject
	{
	public:
		[[nodiscard]] static QApplication* instance();
		[[nodiscard]] bool on_about_to_quit(std::function<void()> handler) const;

		[[nodiscard]] std::string style_sheet() const;
		void set_style_sheet(std::string_view css);

		[[nodiscard]] static std::vector<QWidget*> all_widgets();
	};
}
