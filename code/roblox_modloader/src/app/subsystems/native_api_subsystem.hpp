#pragma once

#include "../isubsystem.hpp"
#include "native_api/global_injector.hpp"

namespace rml {
    class NativeApiSubsystem final : public ISubsystem {
    public:
        std::expected<void, SubsystemError> initialize() override {
            native_api::initialize_global_injector();
            return {}; // Unsupported Studio builds must not stop the loader.
        }
        void shutdown() override { native_api::shutdown_global_injector(); }
        std::string_view name() const noexcept override { return "NativeApi"; }
    };
}
