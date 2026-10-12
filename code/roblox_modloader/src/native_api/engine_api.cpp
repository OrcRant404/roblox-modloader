#include "engine_api.hpp"
#include "engine_bindings.generated.hpp"

#if defined(__APPLE__) && defined(__aarch64__)

#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/vm_prot.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
bool executable_binding_range(const mach_header* image, std::uintptr_t address, std::size_t size) {
    const auto* header = reinterpret_cast<const mach_header_64*>(image);
    if (header->ncmds > 4096 || header->sizeofcmds > 1024 * 1024) return false;
    const char* cursor = reinterpret_cast<const char*>(header + 1);
    const char* end = cursor + header->sizeofcmds;
    for (std::uint32_t index = 0; index < header->ncmds; ++index) {
        if (end - cursor < static_cast<std::ptrdiff_t>(sizeof(load_command))) return false;
        const auto* command = reinterpret_cast<const load_command*>(cursor);
        if (command->cmdsize < sizeof(load_command)
            || command->cmdsize > static_cast<std::size_t>(end - cursor)) return false;
        if (command->cmd == LC_SEGMENT_64 && command->cmdsize >= sizeof(segment_command_64)) {
            const auto* segment = reinterpret_cast<const segment_command_64*>(command);
            if ((segment->initprot & (VM_PROT_READ | VM_PROT_EXECUTE)) == (VM_PROT_READ | VM_PROT_EXECUTE)
                && address >= segment->vmaddr && size <= segment->vmsize
                && address - segment->vmaddr <= segment->vmsize - size) return true;
        }
        cursor += command->cmdsize;
    }
    return false;
}

std::string engine_code_hash(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open Studio image for identity check");
    CC_SHA256_CTX context{};
    CC_SHA256_Init(&context);
    input.seekg(kEngineCodeOffset);
    std::size_t remaining = kEngineCodeSize;
    std::array<char, 65536> buffer{};
    while (remaining) {
        const auto count = std::min(remaining, buffer.size());
        if (!input.read(buffer.data(), count)) throw std::runtime_error("Cannot finish Studio code identity check");
        CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(count));
        remaining -= count;
    }
    unsigned char hash[CC_SHA256_DIGEST_LENGTH]{};
    CC_SHA256_Final(hash, &context);
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (auto byte : hash) result << std::setw(2) << static_cast<unsigned int>(byte);
    return result.str();
}
}

EngineApi rml::native_api::bind_verified_engine() {
    const auto* header = _dyld_get_image_header(0);
    const char* image = _dyld_get_image_name(0);
    if (!header || !image || header->magic != MH_MAGIC_64 || header->filetype != MH_EXECUTE
        || header->cputype != CPU_TYPE_ARM64
        || std::filesystem::path(image).filename() != "RobloxStudio")
        throw std::runtime_error("Global injector requires the verified ARM64 Studio image");
    if (engine_code_hash(image) != kVerifiedEngineCodeSha256)
        throw std::runtime_error("Studio image changed; engine bindings require verification");
    const auto slide = _dyld_get_image_vmaddr_slide(0);
    for (const auto& binding : kEngineBindings) {
        if (!executable_binding_range(header, binding.address, binding.bytes.size()))
            throw std::runtime_error(std::string("Engine binding is outside an executable image segment: ") + binding.name);
        const auto* target = reinterpret_cast<const unsigned char*>(binding.address + slide);
        if (std::memcmp(target, binding.bytes.data(), binding.bytes.size()) != 0)
            throw std::runtime_error(std::string("Engine function signature changed: ") + binding.name);
    }
    EngineApi api{};
    #define BIND(field, name) do { \
        const auto address = engine_address(name); \
        if (!address) throw std::runtime_error("Missing verified engine binding: " name); \
        api.field = reinterpret_cast<decltype(api.field)>(address + slide); \
    } while (false)
    BIND(sandboxthread, "luaL_sandboxthread");
    BIND(load, "luau_load"); BIND(checkstack, "lua_checkstack");
    BIND(gettop, "lua_gettop"); BIND(settop, "lua_settop");
    BIND(createtable, "lua_createtable"); BIND(pushcclosurek, "lua_pushcclosurek");
    BIND(pushlstring, "lua_pushlstring"); BIND(tolstring, "lua_tolstring");
    BIND(getfield, "lua_getfield"); BIND(setfield, "lua_setfield");
    BIND(setreadonly, "lua_setreadonly"); BIND(setsafeenv, "lua_setsafeenv");
    BIND(getreadonly, "lua_getreadonly");
    BIND(pushnil, "lua_pushnil"); BIND(type, "lua_type");
    BIND(pushvalue, "lua_pushvalue"); BIND(pushboolean, "lua_pushboolean");
    BIND(pushnumber, "lua_pushnumber"); BIND(tonumberx, "lua_tonumberx");
    BIND(toboolean, "lua_toboolean"); BIND(rawgeti, "lua_rawgeti");
    // These engine reference APIs depend on an uninitialized reference table
    // in ordinary Studio VMs. Providers use their own Lua registry table.
    // Preserve the vtable slots as null rather than expose unsafe callbacks.
    BIND(pcall, "lua_pcall"); BIND(mainthread, "lua_mainthread");
    BIND(rawequal, "lua_rawequal"); BIND(gettable, "lua_gettable");
    BIND(rawseti, "lua_rawseti");
    #undef BIND
    return api;
}

#else
#include <stdexcept>
EngineApi rml::native_api::bind_verified_engine() {
    throw std::runtime_error("Native engine injection is supported only for the verified macOS ARM64 Studio build");
}
#endif
