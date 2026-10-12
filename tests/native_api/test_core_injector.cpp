// Standalone Core regression test. It uses a fake VM and never launches Studio
// or installs detours. Compile api_registry.cpp alongside this translation unit.
#include <RobloxModLoader/luau/engine_api.hpp>
#include <RobloxModLoader/luau/native_api.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

[[noreturn]] void fail(const char* expression, int line)
{
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
    std::exit(1);
}

// Keep checks enabled if the test is compiled with a release configuration.
#define CHECK(expression) ((expression) ? static_cast<void>(0) : fail(#expression, __LINE__))

struct Table;
struct Value {
    int type{kEngineNil};
    std::string text;
    std::shared_ptr<Table> table;
    EngineCFunction function{};
};

struct Table {
    bool readonly{};
    std::unordered_map<std::string, Value> fields;
};

struct lua_State {
    std::vector<Value> stack;
    std::shared_ptr<Table> globals{std::make_shared<Table>()};
    bool reserve{true};
};

Value table_value(std::shared_ptr<Table> table)
{
    return {kEngineTable, {}, std::move(table), nullptr};
}

Value value_at(lua_State* state, int index)
{
    if (index == kGlobalsIndex) return table_value(state->globals);
    const int position = index > 0 ? index - 1 : static_cast<int>(state->stack.size()) + index;
    return position >= 0 && position < static_cast<int>(state->stack.size())
        ? state->stack[position] : Value{};
}

int top(lua_State* state) { return static_cast<int>(state->stack.size()); }

void set_top(lua_State* state, int index)
{
    const int size = index >= 0 ? index : top(state) + index + 1;
    CHECK(size >= 0);
    state->stack.resize(static_cast<std::size_t>(size));
}

int reserve(lua_State* state, int) { return state->reserve; }
int type(lua_State* state, int index) { return value_at(state, index).type; }

void push_string(lua_State* state, const char* text, std::size_t length)
{
    state->stack.push_back({kEngineString, std::string(text, length), {}, nullptr});
}

const char* to_string(lua_State* state, int index, std::size_t* length)
{
    const int position = index > 0 ? index - 1 : top(state) + index;
    *length = state->stack.at(position).text.size();
    return state->stack.at(position).text.data();
}

void push_nil(lua_State* state) { state->stack.emplace_back(); }

void create_table(lua_State* state, int, int)
{
    state->stack.push_back(table_value(std::make_shared<Table>()));
}

int get_field(lua_State* state, int index, const char* name)
{
    const auto table = value_at(state, index).table;
    state->stack.push_back(table && table->fields.contains(name) ? table->fields.at(name) : Value{});
    return state->stack.back().type;
}

void set_field(lua_State* state, int index, const char* name)
{
    const auto table = value_at(state, index).table;
    CHECK(table && !table->readonly);
    table->fields[name] = state->stack.back();
    state->stack.pop_back();
}

int readonly(lua_State* state, int index) { return value_at(state, index).table->readonly; }

void set_readonly(lua_State* state, int index, int enabled)
{
    value_at(state, index).table->readonly = enabled;
}

void push_closure(lua_State* state, EngineCFunction function, const char*, int, EngineContinuation)
{
    state->stack.push_back({kEngineFunction, {}, {}, function});
}

// Include production implementations to exercise startup gating and frozen
// snapshots directly. Paths remain valid when this repository is relocated.
// engine_api.cpp also requires Core's generated engine_bindings.generated.hpp.
#include "../../code/roblox_modloader/src/native_api/global_injector.cpp"
#include "../../code/roblox_modloader/src/native_api/engine_api.cpp"

extern "C" int DobbyHook(void*, void*, void**)
{
    // The test must not attempt to hook its own process.
    CHECK(false);
    return -1;
}

RmlNativeApiCallbackStatus echo(
    void*, const char* text, std::size_t length,
    RmlNativeApiOwnedString* result, RmlNativeApiOwnedString*)
{
    return rml_native_api_owned_string_assign(result, text, length) == RML_NATIVE_API_OK
        ? RML_NATIVE_API_CALLBACK_SUCCESS : RML_NATIVE_API_CALLBACK_FAILURE;
}

int main()
{
    CHECK(rml_native_api_abi_version() == RML_NATIVE_API_ABI_VERSION);

#if defined(__APPLE__) && defined(__aarch64__)
    struct Image {
        mach_header_64 header{};
        segment_command_64 segment{};
    } image;
    image.header.ncmds = 1;
    image.header.sizeofcmds = sizeof(image.segment);
    image.segment.cmd = LC_SEGMENT_64;
    image.segment.cmdsize = sizeof(image.segment);
    image.segment.vmaddr = 0x100000000;
    image.segment.vmsize = 4096;
    image.segment.initprot = VM_PROT_READ | VM_PROT_EXECUTE;
    const auto* header = reinterpret_cast<const mach_header*>(&image.header);
    CHECK(executable_binding_range(header, 0x100000000, 32));
    CHECK(!executable_binding_range(header, 0x100000fff, 32));
    CHECK(!executable_binding_range(header, std::numeric_limits<std::uintptr_t>::max(), 32));
    image.segment.initprot = VM_PROT_READ;
    CHECK(!executable_binding_range(header, 0x100000000, 32));
    image.segment.initprot = VM_PROT_READ | VM_PROT_EXECUTE;
    image.segment.cmdsize = sizeof(image.segment) + 4;
    CHECK(!executable_binding_range(header, 0x100000000, 32));
    image.segment.cmdsize = sizeof(image.segment);
    image.header.ncmds = 4097;
    CHECK(!executable_binding_range(header, 0x100000000, 32));
    std::cout << "PASS: binding reads stay inside bounded executable Mach-O segments\n";

    api.checkstack = reserve;
    api.gettop = top;
    api.settop = set_top;
    api.type = type;
    api.createtable = create_table;
    api.getfield = get_field;
    api.setfield = set_field;
    api.getreadonly = readonly;
    api.setreadonly = set_readonly;
    api.pushlstring = push_string;
    api.pushnil = push_nil;
    api.tolstring = to_string;
    api.pushcclosurek = push_closure;
    active.store(true);

    lua_State empty;
    install_global(&empty);
    CHECK(!empty.globals->fields.contains("modloader") && empty.stack.empty());
    rml::native_api::complete_native_api_startup();
    install_global(&empty);
    const auto empty_root = empty.globals->fields.at("modloader").table;
    CHECK(empty_root->readonly && empty_root->fields.at("Version").text == "0.1.0");
    CHECK(empty_root->fields.size() == 1 && empty.stack.empty());
    std::cout << "PASS: startup deferral followed by frozen Core-only Version root\n";

    RmlNativeApiOwner* owner{};
    RmlNativeApiExportSlot slot{};
    CHECK(rml_native_api_register_namespace("example", &owner) == RML_NATIVE_API_OK);
    CHECK(rml_native_api_register_string_export(owner, "Echo", echo, nullptr, &slot) == RML_NATIVE_API_OK);
    lua_State unpublished;
    install_global(&unpublished);
    CHECK(!unpublished.globals->fields.at("modloader").table->fields.contains("example"));
    CHECK(rml_native_api_publish_namespace(owner) == RML_NATIVE_API_OK);
    lua_State published;
    install_global(&published);
    const auto root = published.globals->fields.at("modloader").table;
    const auto provider = root->fields.at("example").table;
    CHECK(root->readonly && provider->readonly && published.stack.empty());
    const auto callback = provider->fields.at("Echo").function;
    CHECK(callback != nullptr);
    const std::string input("a\0b", 3);
    push_string(&published, input.data(), input.size());
    CHECK(callback(&published) == 1 && published.stack.back().text == input);
    set_top(&published, 0);
    CHECK(rml_native_api_revoke_namespace(owner) == RML_NATIVE_API_OK);
    push_string(&published, "x", 1);
    CHECK(callback(&published) == 2 && published.stack[published.stack.size() - 2].type == kEngineNil);
    CHECK(published.stack.back().text == "export has been removed");
    std::cout << "PASS: publication, frozen namespace, binary-string dispatch and revoked cached closure\n";

    lua_State foreign;
    foreign.globals->fields["modloader"] = {kEngineString, "consumer-owned", {}, nullptr};
    install_global(&foreign);
    CHECK(foreign.globals->fields.at("modloader").text == "consumer-owned" && foreign.stack.empty());
    lua_State protected_environment;
    protected_environment.globals->readonly = true;
    install_global(&protected_environment);
    CHECK(protected_environment.globals->fields.empty() && protected_environment.stack.empty());
    lua_State no_room;
    no_room.reserve = false;
    install_global(&no_room);
    CHECK(no_room.globals->fields.empty() && no_room.stack.empty());
    install_global(&empty);
    CHECK(empty.globals->fields.at("modloader").table == empty_root);
    CHECK(!empty_root->fields.contains("example"));

    // luau_load can supply a stack environment instead of the globals index.
    lua_State custom_environment;
    create_table(&custom_environment, 0, 0);
    const auto environment = custom_environment.stack.back().table;
    install_global(&custom_environment, -1);
    CHECK(custom_environment.stack.size() == 1 && custom_environment.stack.back().table == environment);
    CHECK(environment->fields.at("modloader").table->readonly);
    CHECK(custom_environment.globals->fields.empty());
    lua_State invalid_environment;
    push_string(&invalid_environment, "sentinel", 8);
    install_global(&invalid_environment, -1);
    CHECK(invalid_environment.stack.size() == 1 && invalid_environment.stack.back().text == "sentinel");
    CHECK(invalid_environment.globals->fields.empty());

    rml::native_api::shutdown_global_injector();
    set_top(&published, 0);
    CHECK(callback(&published) == 2 && published.stack.back().text == "modloader API is inactive");
    std::cout << "PASS: foreign globals, readonly environments, stack reserve/balance, snapshot identity and deactivated closures\n";
#else
    rml::native_api::initialize_global_injector();
    rml::native_api::complete_native_api_startup();
    CHECK(!rml::native_api::global_injector_active());
    rml::native_api::shutdown_global_injector();
    std::cout << "PASS: unsupported-platform injector remains inactive\n";
#endif

    bool rejected = false;
    try { (void)rml::native_api::bind_verified_engine(); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    std::cout << "PASS: non-Studio engine image rejects binding safely\n";
}
