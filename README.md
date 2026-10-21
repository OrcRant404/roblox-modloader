<p align="center">
  <p align="center">
	<img width="150" height="150" src="./assets/logo.png" alt="Logo">
  </p>
  <h1 align="center"><b>Roblox ModLoader</b></h1>
  <p align="center">
    A modding framework for Roblox Studio, enabling native, C#, and internal Luau script mods.
  </p>
</p>

<div align="center">

![GitHub Workflow Status](https://img.shields.io/github/actions/workflow/status/OrcRant404/roblox-modloader/ci.yml?style=for-the-badge&branch=develop&logo=github&label=fork%20build)
[![GitHub License](https://img.shields.io/github/license/OrcRant404/roblox-modloader?style=for-the-badge)](LICENSE)

</div>

This is **OrcRant404's fork** of [Revolutionxk's Roblox ModLoader](https://github.com/revolutionxk/roblox-modloader).
It retains upstream Windows support and adds macOS ARM64 compatibility and managed-mod lifecycle fixes.
[Fork downloads](https://github.com/OrcRant404/roblox-modloader/releases) ·
[Custom Templates mod](https://github.com/OrcRant404/rml-custom-templates) ·
[Linux/Vinegar installation and test limits](docs/linux-vinegar.md)

<div align="center">

[![Discord](https://img.shields.io/discord/1405678257545936916?style=for-the-badge&logo=discord&logoColor=white)](https://robloxmodloader.com/)

</div>

> [!NOTE]
> This project is still in development and may contain bugs or incomplete features.

> [!WARNING]
> Roblox changed (shuffled) the internal layout of Luau’s structs a few months ago, so the in-memory structures we
> relied on no longer line up. Because of that I’m building a
> static-analysis [dumper](https://github.com/revolutionxk/roblox-modloader/tree/develop/dumper) to reconstruct the
> correct structs and offsets so scripting support can work again. Luau/internal scripting is temporarily disabled while
> I
> finish that—native C++/C# mods keep working normally. I’ll re-enable scripting once
> the [dumper](https://github.com/revolutionxk/roblox-modloader/tree/develop/dumper) produces a stable, reliable
> mapping.

## Cross-Platform Support

| Platform | Status in this fork |
| --- | --- |
| Windows x64 | Native build and managed tests run in GitHub Actions. Native Windows startup is unverified; the same payload failed signature matching on Studio 0.743 under Wine. |
| Linux x86-64 via Vinegar/Wine | Installer preview. Unmodified Studio 0.743 opened in Ubuntu CI; RML then failed native signature matching. See [recorded result and setup](docs/linux-vinegar.md). |
| macOS ARM64 | Prebuilt loader requires macOS 26.0+. Native builds and live RML Studio workflows tested with Studio 0.741.19.7411056. |

Individual mods have their own platform requirements. **Custom Templates 0.3.1 is macOS ARM64 only**;
its native helper has not been ported to Windows/Wine. A Linux catalog-test result does not establish
that its Studio UI works there.

## Quick Start

### Installation

RML targets your local Roblox **Studio** installation (typically under
`%LOCALAPPDATA%\Roblox\Versions\<version>\`).

### Using the launcher (Not finished yet)

The launcher handles placement and keeps the loader up to date.

1. Download the latest [rml-launcher release](https://github.com/revolutionxk/rml-launcher/releases).
2. Run it and follow the prompts.
3. Start Roblox Studio through the launcher — the loader and your mods are applied automatically.

### Manual installation

#### Windows

1. Download a compatible [fork release](https://github.com/OrcRant404/roblox-modloader/releases).
2. Open your Studio directory — the folder that contains `RobloxStudioBeta.exe`, usually
   `%LOCALAPPDATA%\Roblox\Versions\<version>\`.
3. Extract the archive into that folder.
4. Start Roblox Studio.

The release archive is already laid out so a plain extract lands everything in the right place:

```
<Studio directory>/
├── RobloxStudioBeta.exe
├── dwmapi.dll                    proxy, loaded by Studio
├── roblox_modloader.dll          native core
└── RobloxModLoader/
    ├── config.toml               created with defaults if missing
    ├── runtime/                  native host and RML managed assemblies
    └── mods/                     your mods, one folder each
        └── your-mod/
            ├── native/           native C++ mod DLLs
            ├── dotnet/           .NET mod assemblies
            └── scripts/          Luau scripts (temporarily disabled)
```

The full Windows x64 .NET 10 runtime is a separate prerequisite for managed mods;
it is not bundled in these release archives.

#### Linux / Vinegar

Use the separate `linux-vinegar.tar.gz` bundle and its
[explicit-directory installer](docs/linux-vinegar.md). It uses the Windows x64
loader and requires the `dwmapi=n,b` Wine override. A Windows .NET 10 runtime
in Vinegar's Studio prefix is required for managed mods. Keep this experimental
bundle's package/installer tests separate from live Studio startup results.
The recorded Studio 0.743 test passed without RML, then failed RML's required
Windows signature checks. This preview does not provide working RML on that build.

#### macOS ARM64

The prebuilt loader's Mach-O deployment target is **macOS 26.0**. Earlier macOS
versions require a separately built compatible loader; this release does not
validate those builds.

Use a compatible Studio copy; this fork's live workflows were checked with
**Studio 0.741.19.7411056**. Close Studio before installation and keep a backup
of the app. Extract the macOS release into a separate directory, then copy its
`RobloxModLoader/runtime` into the app's `Contents/MacOS/RobloxModLoader/runtime`
without removing existing mods/config. Install a macOS ARM64 .NET 10 runtime
or make one available in `RobloxModLoader/runtime/dotnet`.
Create `Contents/MacOS/RobloxModLoader/mods` if it is absent; mod installers use
that directory. Open the same app after installation. A separate launcher or
local `rml-studio` shell command is not required for this manual setup.

From the extracted release directory, install the loader with the included
helper (replace the example app path):

```sh
bash scripts/dev/macos-inject.sh \
  --studio "/absolute/path/to/RobloxStudio.app" \
  --dylib "$PWD/roblox_modloader.dylib"
```

The helper keeps the original executable as `.rml-backup`, adds the loader's
Mach-O load command, and ad-hoc signs the app for loading native mods. It accepts
`--restore --studio "/absolute/path/to/RobloxStudio.app"` to restore that executable.
The release does not include Roblox Studio or the full .NET runtime.

## Writing a mod

The quickest way to start is to copy one of the [examples](#examples) and adapt it. Full guides:
[Writing .NET mods](docs/writing-dotnet-mods.md) and
[Writing native mods](docs/writing-native-mods.md).

Native mods can also publish functions through the [built-in `modloader` global](docs/native-luau-api.md). The global is a Core addition. [Workspace](https://github.com/OrcRant404/roblox-developer-toolkit/tree/codex/workspace-mod/workspace) is maintained in the separate mods repository and uses the public API for Studio panel and view control.

## Examples

| Example                                             | Surface    | What it shows                                                     |
|-----------------------------------------------------|------------|-------------------------------------------------------------------|
| [`basic-mod`](examples/basic-mod)                   | C++        | Minimal native mod skeleton and the hooking entry points          |
| [`internal_developer`](examples/internal_developer) | C++        | Enables Studio's internal developer tools                         |
| [`discord_rpc`](examples/discord_rpc)               | C++ / Luau | Discord Rich Presence, native + script bridge                     |
| [`example_dotnet`](examples/example_dotnet)         | C#         | Services, instances, properties, and events through the typed API |
| [`discord_rpc_dotnet`](examples/discord_rpc_dotnet) | C#         | A managed Discord Rich Presence integration                       |

## Building from source

### Prerequisites

- Windows with Visual Studio 2022 (MSVC, x64)
- CMake 3.22.1 or newer
- .NET 10 SDK (for the managed runtime and mods)
- Git

### Steps

```powershell
git clone https://github.com/OrcRant404/roblox-modloader.git
cd roblox-modloader

cmake -B build -S . -G "Visual Studio 17 2022"
cmake --build build --config Release
```

The build generates the `dwmapi.dll` proxy and the loader. Dependencies are fetched automatically
via CMake.

### Build options

| Option                                   | Description                          | Default |
|------------------------------------------|--------------------------------------|---------|
| `ROBLOX_MODLOADER_BUILD_PROXY_GENERATOR` | Build the proxy generator tool       | ON      |
| `ROBLOX_MODLOADER_BUILD_PROXY_DLL`       | Auto-generate the `dwmapi.dll` proxy | ON      |
| `ROBLOX_MODLOADER_BUILD_EXAMPLES`        | Build the example mods               | ON      |

## Roadmap

Current focus areas:

- Finish the [dumper](docs/dumper.md)
- Re-enable Luau/internal scripting support
- Broaden macOS Studio build coverage and validate Linux/Vinegar startup
- Finish .NET modding support (Better API, async support, etc.)
- Add a mod browser and installer

## Contributing

Contributions are welcome. Please open an issue to discuss substantial changes first, keep pull
requests focused, follow the existing code style, and add tests where it makes sense. The
[`docs/`](docs/) directory is the best starting point for understanding the codebase.

## License

Released under the MIT License. See [LICENSE](LICENSE).

## Disclaimer

This project is provided for educational and research purposes. You are responsible for complying
with Roblox's Terms of Service and any applicable laws. It is not affiliated with or endorsed by
Roblox Corporation.
