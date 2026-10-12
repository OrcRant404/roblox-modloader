#include "RobloxModLoader/luau/native_api.hpp"
#include "global_injector.hpp"
#include "engine_api.hpp"
#include "RobloxModLoader/logger/logger.hpp"

#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" int DobbyHook(void*, void*, void**);

RML_LOG_SCOPE("NativeApi");

namespace {
EngineApi api;
void (*original_sandboxthread)(lua_State*){};
int (*original_load)(lua_State*, const char*, const char*, std::size_t, int){};
std::atomic_bool active{false};
std::atomic_bool startup_ready{false};
std::atomic_uint64_t injection_count{};
std::atomic_uint64_t load_count{};

void report(const char* event, const std::string& detail = {}) noexcept {
    try { RML_INFO("{} injections={} {}", event, injection_count.load(), detail); }
    catch (...) {}
}

bool retain_image() {
    static void* retained = [] {
        Dl_info info{};
        if (!dladdr(reinterpret_cast<const void*>(&retain_image), &info) || !info.dli_fname) return static_cast<void*>(nullptr);
        return dlopen(info.dli_fname, RTLD_NOW | RTLD_LOCAL);
    }();
    return retained != nullptr;
}

int error_result(lua_State* state, const char* error) {
    api.pushnil(state);
    api.pushlstring(state, error, std::char_traits<char>::length(error));
    return 2;
}

template<std::size_t Slot>
int dispatch(lua_State* state) {
    if (!active.load(std::memory_order_acquire)) return error_result(state, "modloader API is inactive");
    RmlNativeApiExportKind kind{};
    if (rml_native_api_export_kind(Slot, &kind) != RML_NATIVE_API_OK)
        return error_result(state, "Native export is unavailable");
    if (kind == RML_NATIVE_API_ENGINE_EXPORT) {
        const int before = api.gettop(state);
        int count{};
        rml::native_api::OwnedStringGuard owned_error;
        RmlNativeApiEngineContext context{RML_NATIVE_API_ABI_VERSION, state, &api};
        const auto status = rml_native_api_dispatch_engine(Slot, &context, &count, &owned_error.value);
        if (status == RML_NATIVE_API_OK && count <= api.gettop(state)) return count;
        api.settop(state, before);
        api.pushnil(state);
        const auto& error = owned_error.value;
        api.pushlstring(state, error.data ? error.data : "Native engine export failed", error.data ? error.size : 27);
        return 2;
    }
    if (api.gettop(state) != 1 || api.type(state, 1) != kEngineString)
        return error_result(state, "Native export expects exactly one string argument");
    std::size_t length{};
    const char* input = api.tolstring(state, 1, &length);
    rml::native_api::OwnedStringGuard owned_value, owned_error;
    auto& value = owned_value.value;
    auto& error = owned_error.value;
    const auto status = rml_native_api_dispatch_string(Slot, input, length, &value, &error);
    if (status == RML_NATIVE_API_OK) {
        api.pushlstring(state, value.data ? value.data : "", value.size);
        return 1;
    }
    api.pushnil(state);
    const char* text = error.data ? error.data : "Native export call failed";
    api.pushlstring(state, text, error.data ? error.size : std::char_traits<char>::length(text));
    return 2;
}

template<std::size_t... Slots>
constexpr auto make_trampolines(std::index_sequence<Slots...>) {
    return std::array<EngineCFunction, sizeof...(Slots)>{&dispatch<Slots + 1>...};
}
constexpr auto trampolines = make_trampolines(std::make_index_sequence<RML_NATIVE_API_MAX_EXPORT_SLOTS>{});

struct SnapshotGuard {
    RmlNativeApiSnapshot value{};
    ~SnapshotGuard() { rml_native_api_snapshot_destroy(&value); }
};

void install_global(lua_State* state, int environment = kGlobalsIndex) {
    if (!startup_ready.load(std::memory_order_acquire)) return;
    SnapshotGuard owned_snapshot;
    auto& snapshot = owned_snapshot.value;
    if (rml_native_api_snapshot(&snapshot) != RML_NATIVE_API_OK) { report("snapshot-failed"); return; }
    const int before = api.gettop(state);
    if (environment > -10000 && environment < 0) environment = before + environment + 1;
    if (!api.checkstack(state, 8)) { report("stack-reserve-failed"); return; }
    if (api.type(state, environment) != kEngineTable) { report("environment-not-table"); return; }
    api.getfield(state, environment, "modloader");
    const int existing_type = api.type(state, -1);
    api.settop(state, before);
    // Never replace a global belonging to another injector or consumer.
    if (existing_type != kEngineNil) {
        return;
    }
    if (api.getreadonly(state, environment)) { report("readonly-environment-skipped"); return; }
    api.createtable(state, 0, 2);
    api.pushlstring(state, "0.1.0", 5);
    api.setfield(state, -2, "Version");
    for (std::size_t index = 0; index < snapshot.count; ++index) {
        const auto& entry = snapshot.entries[index];
        api.getfield(state, -1, entry.namespace_name);
        const bool present = api.type(state, -1) == kEngineTable;
        api.settop(state, api.gettop(state) - 1);
        if (present) continue;
        api.createtable(state, 0, static_cast<int>(snapshot.count));
        for (std::size_t function_index = index; function_index < snapshot.count; ++function_index) {
            const auto& function = snapshot.entries[function_index];
            if (std::string_view(function.namespace_name) != entry.namespace_name) continue;
            if (function.slot < 1 || function.slot > trampolines.size()) continue;
            api.pushcclosurek(state, trampolines[function.slot - 1], function.function_name, 0, nullptr);
            api.setfield(state, -2, function.function_name);
        }
        api.setreadonly(state, -1, 1);
        api.setfield(state, -2, entry.namespace_name);
    }
    api.setreadonly(state, -1, 1);
    api.setfield(state, environment, "modloader");
    // Add the fixed read-only API before imports are resolved by luau_load.
    // Do not change safeenv or Studio's original environment argument.
    if (api.gettop(state) != before) {
        api.settop(state, before);
        report("stack-balance-failed");
        active.store(false, std::memory_order_release);
        return;
    }
    const auto count = injection_count.fetch_add(1) + 1;
    if (count <= 10 || count % 100 == 0) report("global-injected");
}

int load_hook(lua_State* state, const char* chunk, const char* data, std::size_t size, int environment) {
    if (active.load(std::memory_order_acquire)) {
        const auto count = load_count.fetch_add(1) + 1;
        if (count <= 10) report("load-seen", "environment=" + std::to_string(environment));
        install_global(state, environment == 0 ? kGlobalsIndex : environment);
    }
    return original_load(state, chunk, data, size, environment);
}

void sandboxthread_hook(lua_State* state) {
    original_sandboxthread(state);
    if (active.load(std::memory_order_acquire)) install_global(state);
}

} // namespace

namespace rml::native_api {
void initialize_global_injector() noexcept {
    static std::once_flag attempted;
    try {
        std::call_once(attempted, [] {
            api = bind_verified_engine();
            if (!retain_image()) throw std::runtime_error("Cannot retain loader callback code");
            // Install before native providers load. Injection stays disabled
            // until both trampolines are valid and the complete mod scan ends.
            // These resident hooks are never unpatched during loader teardown.
            const int sandbox_status = DobbyHook(reinterpret_cast<void*>(api.sandboxthread),
                reinterpret_cast<void*>(&sandboxthread_hook), reinterpret_cast<void**>(&original_sandboxthread));
            if (sandbox_status != 0 || !original_sandboxthread)
                throw std::runtime_error("Cannot install verified sandboxthread hook: " + std::to_string(sandbox_status));
            const int load_status = DobbyHook(reinterpret_cast<void*>(api.load),
                reinterpret_cast<void*>(&load_hook), reinterpret_cast<void**>(&original_load));
            if (load_status != 0 || !original_load)
                throw std::runtime_error("Cannot install verified luau_load hook: " + std::to_string(load_status));
            active.store(true, std::memory_order_release);
            report("hooks-installed", "sandboxthread+luau_load; verified engine C ABI; no VM structure offsets");
        });
    } catch (const std::exception& error) {
        active.store(false, std::memory_order_release);
        try { RML_WARN("Global injection disabled: {}", error.what()); } catch (...) {}
    } catch (...) {
        active.store(false, std::memory_order_release);
        try { RML_WARN("Global injection disabled: unknown initialization error"); } catch (...) {}
    }
}

void complete_native_api_startup() noexcept {
    startup_ready.store(true, std::memory_order_release);
    if (active.load(std::memory_order_acquire))
        report("global-ready", "initial provider scan complete; empty Version root is supported");
}

bool global_injector_active() noexcept {
    return active.load(std::memory_order_acquire);
}

void shutdown_global_injector() noexcept {
    active.store(false, std::memory_order_release);
    startup_ready.store(false, std::memory_order_release);
    report("injector-deactivated", "resident detours and cached closures remain valid until process exit");
}
} // namespace rml::native_api

#else
namespace rml::native_api {
void initialize_global_injector() noexcept {}
void complete_native_api_startup() noexcept {}
bool global_injector_active() noexcept { return false; }
void shutdown_global_injector() noexcept {}
}
#endif
