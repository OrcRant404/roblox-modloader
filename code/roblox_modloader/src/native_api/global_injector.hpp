#pragma once

namespace rml::native_api {
    // Core lifecycle. Unsupported engine images leave injection disabled.
    void initialize_global_injector() noexcept;
    void complete_native_api_startup() noexcept;
    bool global_injector_active() noexcept;
    void shutdown_global_injector() noexcept;
}
