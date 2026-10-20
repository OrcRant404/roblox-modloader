# Third-party notices

The RML upstream MIT license, Copyright 2026 Revolutionxk, is retained as
`LICENSE` at the root of this archive. The packaging tools do not change the
licenses of the Windows loader or its components.

The `licenses/` directory retains license texts for components used by the
Windows build: spdlog (including fmt), zlib, toml++, nlohmann/json, MinHook
(including its HDE notices), PolyHook 2, its assembler/disassembler dependencies,
Luau/Lua, and the .NET native host. `licenses/SOURCES.json` records the primary
source URL used for each downloaded text. Vendored G3D/Bullet source-file
copyright/license comments are retained in `vendored-source-notices.txt`.
The general G3D license is retained from its maintained upstream distribution;
the vendored source comments preserve the older files' individual notices.
These notices are attribution material, not a complete dependency lockfile or
an assertion that every listed component is present in every loader build.
Some upstream build dependencies track moving branches; the Windows artifact's
exact source SHA and CI run are recorded separately in `manifest.json`.

This product includes software developed by the University of California,
Berkeley and its contributors (the vendored G3D fnmatch implementation).

Microsoft's .NET license and third-party notices are retained for the native
`nethost.dll` component. The .NET runtime/CoreCLR itself is not bundled. Wine,
Vinegar, Roblox Studio, and user mods are not bundled. Their installation and
use remain subject to their own licenses and terms.

Upstream projects:

- [RML](https://github.com/revolutionxk/roblox-modloader)
- [spdlog](https://github.com/gabime/spdlog), [fmt](https://github.com/fmtlib/fmt)
- [zlib](https://github.com/madler/zlib)
- [toml++](https://github.com/marzer/tomlplusplus)
- [nlohmann/json](https://github.com/nlohmann/json)
- [MinHook](https://github.com/TsudaKageyu/minhook)
- [PolyHook 2](https://github.com/stevemk14ebr/PolyHook_2_0)
- [AsmJit](https://github.com/asmjit/asmjit), [AsmTK](https://github.com/asmjit/asmtk)
- [Zydis](https://github.com/zyantific/zydis), [Zycore](https://github.com/zyantific/zycore-c)
- [Luau](https://github.com/luau-lang/luau)
- [G3D](https://casual-effects.com/g3d/), [Bullet](https://github.com/bulletphysics/bullet3)
- [.NET runtime/native hosting](https://github.com/dotnet/runtime)
