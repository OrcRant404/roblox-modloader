# Workspace mod for the built-in RML API

The `modloader` global and native API registry are compiled into RML itself. Their implementation lives in `code/roblox_modloader/src/native_api/`, and the public provider headers live in `include/RobloxModLoader/luau/`. There is no ModloaderApi mod or separate Core library to install.

This example is the compiled `workspace` mod. It registers existing plugin panels, opens/closes panels, applies complete views around the real Studio viewport, and restores suspended or normal layouts. Ordinary plugins and the Command Bar call `modloader["workspace"]`. View handles provide `Entered` and `Exited` events. A → B → Exit B resumes A; selecting A beneath B removes B, and Exit A restores normal Studio.

See [the native API contract](../../docs/native-luau-api.md), [workspace API and validation](docs/workspace-mod-results.md), and [view specification](docs/modloader-workspace-spec.md). The integrated loader has separate live records for [Core-only plugin and Command Bar checks](docs/evidence/core-integration/runtime-summary.json) and [Workspace panel controls and A/B events](docs/evidence/core-integration/workspace-runtime-summary.json). Older workspace evidence records the standalone Core prototype.

## Source layout

- `native-workspace/`: panel controls, docking support, view stack, event delivery, and plugin lifecycle handling.
- `scripts/`: explicit build/preparation tools, isolated launcher, and native regression tests.
- `scripts/test-fixtures/`: native layout fixture and optional ordinary test consumers.
- `docs/`: workspace API notes and validation evidence.

The API probe, older VFX diagnostic providers, and generated test plugins are excluded. A/B packages are generated only with `--test-plugins`; they are ordinary regression fixtures. There is no node editor or VFX plugin. CMake exposes a help target so its recursive example discovery does not compile standalone tests into a native mod.

## Isolation and support

Workspace remains a guarded ARM64/macOS prototype. Use the marked `RMLWorkspaceTest.app` through the verified launcher. Source Studio is a read-only input; only the copied loader is replaced. The fence protects writes under shared Roblox and Preferences paths. Native guards pin the Studio engine, Qt image identities, docking manager, GUI thread, and confined snapshot paths.

Core initializes automatically in RML and safely disables injection on unsupported engine builds. It is independent of the optional RML script VM. Native changes and newly added namespace snapshots require a restart. Per-place persistence, seven-day expiry, full camera ownership, and crash recovery remain future work.
