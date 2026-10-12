#pragma once

#include <cstddef>
#include <cstdint>

// Opaque engine state: never include the SDK's stale VM structure definitions.
struct lua_State;
using EngineCFunction = int (*)(lua_State*);
using EngineContinuation = int (*)(lua_State*, int);

struct EngineApi {
    void (*sandboxthread)(lua_State*){};
    int (*load)(lua_State*, const char*, const char*, std::size_t, int){};
    int (*checkstack)(lua_State*, int){};
    int (*gettop)(lua_State*){};
    void (*settop)(lua_State*, int){};
    void (*createtable)(lua_State*, int, int){};
    void (*pushcclosurek)(lua_State*, EngineCFunction, const char*, int, EngineContinuation){};
    void (*pushlstring)(lua_State*, const char*, std::size_t){};
    const char* (*tolstring)(lua_State*, int, std::size_t*){};
    int (*getfield)(lua_State*, int, const char*){};
    void (*setfield)(lua_State*, int, const char*){};
    void (*setreadonly)(lua_State*, int, int){};
    int (*getreadonly)(lua_State*, int){};
    void (*setsafeenv)(lua_State*, int, int){};
    void (*pushnil)(lua_State*){};
    int (*type)(lua_State*, int){};
    void (*pushvalue)(lua_State*, int){};
    void (*pushboolean)(lua_State*, int){};
    void (*pushnumber)(lua_State*, double){};
    double (*tonumberx)(lua_State*, int, int*){};
    int (*toboolean)(lua_State*, int){};
    int (*rawgeti)(lua_State*, int, int){};
    int (*ref)(lua_State*, int){};
    int (*unref)(lua_State*, int){};
    int (*getref)(lua_State*, int){};
    int (*pcall)(lua_State*, int, int, int){};
    lua_State* (*mainthread)(lua_State*){};
    int (*rawequal)(lua_State*, int, int){};
    int (*gettable)(lua_State*, int){};
    void (*rawseti)(lua_State*, int, int){};
};

// These constants were verified in this engine's instructions/type-name table.
// In particular, Roblox's added integer tag changes the SDK's string/table tags.
constexpr int kGlobalsIndex = -10002;
constexpr int kEngineNil = 0;
constexpr int kEngineString = 6;
constexpr int kEngineTable = 7;
constexpr int kEngineBoolean = 1;
constexpr int kEngineNumber = 3;
constexpr int kEngineFunction = 8;
constexpr int kEngineUserdata = 9;
constexpr int kFirstUpvalueIndex = -10003;

// ABI version 1. This layout is shared by the loader and native providers.
// Only the loader constructs it; a provider may use it during its callback.
inline constexpr std::uint32_t kEngineApiAbiVersion = 1u;
