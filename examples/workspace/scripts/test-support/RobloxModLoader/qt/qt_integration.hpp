#pragma once

#include <functional>

namespace rml::qt {

// Small compile-time stand-in for the GUI dispatcher used by the host test.
// The test defines these methods and stores callbacks in its fake GUI queue.
class QtIntegration {
public:
	static QtIntegration* instance();
	void run_on_gui_thread(std::function<void()> callback);
};

} // namespace rml::qt
