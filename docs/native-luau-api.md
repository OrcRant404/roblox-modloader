# Built-in modloader global

RML Core owns the native export registry and injects a read-only `modloader` table into supported Roblox engine environments before their scripts load. Ordinary plugins and the real Command Bar consume that table. This does not require RML's optional internal scripting VM or a separate native API mod.

```luau
print(modloader.Version)
local Workspace = modloader["workspace"]
-- Only namespaces published by loaded compiled mods are present.
```

Native mods register APIs through `<RobloxModLoader/luau/native_api.hpp>`. Check `rml_native_api_abi_version()` against `RML_NATIVE_API_ABI_VERSION`, register a namespace, register all exports, then call `rml_native_api_publish_namespace(owner)`. Publication is atomic from consumers' perspective: snapshots and dispatch exclude unfinished namespaces. An empty namespace cannot be published; publication is idempotent, and published exports cannot be extended. Revoke the owner during unload to disable its callbacks and drain in-flight calls.

Plugins do not register mods. There is no Luau `RegisterMods` function or intermediate `modloader.mods` table. For example, an HTTP unlocker may publish `http-unlocker`, which consumers access as `modloader["http-unlocker"]`.

String callbacks receive exactly one length-delimited string argument and return loader-owned value/error strings. Use the provided assign/destroy functions for ownership. Engine callbacks receive an opaque state and versioned EngineApi table from `<RobloxModLoader/luau/engine_api.hpp>`; call only during synchronous dispatch, keep the Lua stack balanced, and do not yield or retain the state. Studio's private reference functions are intentionally unavailable.

Core installs verified engine hooks before native providers load, and permits injection only after initial mod loading finishes. A loader with zero provider namespaces still supplies `modloader.Version`. Namespace tables are frozen snapshots for each environment. Newly loaded namespaces require a restart for existing consumers; revoked callbacks report unavailability. Slots and callback code remain resident for process lifetime so old closures cannot jump into unloaded code.

The first engine binding supports the verified macOS ARM64 Studio build `0.741.19.7411056`. Core checks the executable code hash, executable segment bounds, and each function's bytes before installing hooks. On other builds/platforms this subsystem declines injection. Other RML subsystems retain their existing compatibility checks. This contract is independent of the outdated SDK VM structures and does not claim other builds are supported.

The compiled [Workspace mod](https://github.com/OrcRant404/roblox-developer-toolkit/tree/codex/workspace-mod/workspace) lives in the separate mods repository. It implements panel controls, view stacking, and lifecycle events through this API. The loader repository owns the global, export registry, engine bindings, public SDK headers, and their Core tests. Workspace uses that public SDK and the rebuilt loader; it does not embed Core source.

Run the two Core regression groups after configuring the RML build:

```sh
python3 -B tests/native_api/run_tests.py
# For another configured build, add --core-build /path/to/build.
```

These ASan/UBSan checks cover Core injection/binding guards and registry concurrency/readiness without launching Studio. Workspace's four native groups remain with its own source. [Live Core validation](evidence/native-api/runtime-summary.json) records the real Command Bar and ordinary local-plugin checks with zero external native mods. To generate that optional consumer fixture, run `python3 -B tests/native_api/export_test_plugin.py`; it writes an ignored package beneath `build/native-api-test/plugins` and is not a native mod.
