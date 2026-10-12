#pragma once
#include <RobloxModLoader/luau/native_api.hpp>
namespace rml::workspace {
void register_lua_exports(RmlNativeApiOwner* owner);
void deactivate_lua_exports();
}
