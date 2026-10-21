# Experimental Linux / Vinegar bundle

This bundle repackages the **Windows x64 RML loader** for Roblox Studio running
under Vinegar/Wine on Linux. It contains no Roblox executable and no Linux ELF
loader. It does not install Vinegar, change its configuration, download Studio,
or install the Windows .NET runtime.

The checks cover package structure, native PE AMD64 headers, managed assemblies,
and filesystem installation/recovery. They do **not** establish that this RML
build, a mod, Wine, or a particular Studio version starts or works together.
Treat this as an experimental manual installation. Native mods must be built
for Windows x64; macOS `.dylib`/`.so` builds do not work in Windows Studio.

## Recorded live result: Studio 0.743 is unsupported

The [real Ubuntu Wine run on 2026-10-20](https://github.com/OrcRant404/roblox-modloader/actions/runs/39519306475)
launched official **Studio 0.743.0.7431133** with pinned Kombucha
`stable+20261018103513`, Xvfb/Openbox and software OpenGL. The unmodified Studio
login window passed process, visible-window, screenshot/OCR and 32-second
survival checks. No account sign-in was needed.

The exact Windows RML payload from source
`5d3090ad93ca8b212ca859fea5b1dca3e2e0fe1b` then failed native initialization:
`Failed to initialize subsystem Pointers: Failed to find some patterns for roblox`.
Five signatures were missing: `LUAH_NEW`, `GET_STRING_ATOM`,
`DESCRIPTOR_LOOKUP`, `SIGNAL_DISCONNECT` and `LUAE_NEWTHREAD`.

**This Linux download is an installer preview. RML does not initialize with the
tested Studio 0.743 build.** Updating the Windows Studio bindings and testing
them again is required before claiming working Linux support. Managed mods,
editing, saves and Custom Templates were not exercised. The Windows archive
contains the same loader payload; native Windows startup remains unverified.

## Before installing

- Install Vinegar and launch Studio successfully once without RML. Close both
  Studio and Vinegar before changing loader files. Do not run the installer as root.
- Install Python 3.10 or newer on Linux. The installer uses only its standard library.
- Install the **Windows x64 .NET 10 runtime** into the same Wine prefix that
  Vinegar uses for Studio. The package contains `nethost.dll` and RML managed
  assemblies, but no `hostfxr.dll` or CoreCLR. A Linux `dotnet` installation is
  insufficient. Use the Windows runtime from [Microsoft's .NET 10 download page](https://dotnet.microsoft.com/en-us/download/dotnet/10.0)
  with Vinegar's Wine/prefix. This setup has not been tested here. The two
  `runtimeconfig.json` files in the bundle specify the minimum `10.0.x` patch;
  install that patch or a later compatible 10.0 release.
- The Windows loader uses the system .NET lookup. Copying a runtime into
  `RobloxModLoader/runtime/dotnet` does not configure this lookup.

## Find the active Studio directory

The installer requires `--studio-dir` pointing to the **existing directory
containing `RobloxStudioBeta.exe`**. It never picks a version or creates a prefix.
Use the version Vinegar actually launches. Verify it in Vinegar's current logs
or inspect its data directory; multiple old versions can exist.

[Vinegar's directory definitions](https://github.com/vinegarhq/vinegar/blob/master/internal/dirs/dirs.go)
place versions under its XDG data directory's `vinegar/versions`, and the
[prefix configuration](https://github.com/vinegarhq/vinegar/blob/master/internal/config/config.go)
uses `vinegar/prefixes/studio`. Typical data roots are
`~/.local/share/vinegar` for a native install and
`~/.var/app/org.vinegarhq.Vinegar/data/vinegar` for Flatpak. These are hints for
inspection, **not automatic install targets**. Follow your installation's XDG
settings and actual deployed version.

Do not target an overlay directory. Vinegar also has an
[overlay mechanism](https://vinegarhq.org/Vinegar/Configuration/TipsAndTricks.html),
but this installer manages an explicit deployed version so its receipt can
restore that version's prior loader files. A Studio update can deploy a new
directory; recheck the target and compatibility before installing again.

## Review, then install

Before extracting the downloaded archive, verify its entry in the
release's `SHA256SUMS-roblox-modloader.txt` file:

```sh
sha256sum --ignore-missing -c SHA256SUMS-roblox-modloader.txt
```

A locally built bundle instead has an adjacent `.sha256` file, checked with
`sha256sum -c <archive>.sha256`.

Extract the archive into a separate folder and enter `roblox-modloader-linux-vinegar`.

```sh
python3 install.py --studio-dir "/absolute/path/to/active-Studio-version" --dry-run

# Close Studio and Vinegar, then install into the same reviewed directory:
python3 install.py --studio-dir "/absolute/path/to/active-Studio-version" --studio-closed
```

The installer requires a structurally valid Windows x64 Studio executable,
validates the bundled payload against its hash manifest, and refuses links or
special files in paths it changes. Its Linux process check is a best-effort
extra guard; `--studio-closed` is your confirmation that Studio/Vinegar are closed.
Do not edit the target or start Studio while it runs.

Only these paths are replaced:

```text
<Studio>/dwmapi.dll
<Studio>/roblox_modloader.dll
<Studio>/RobloxModLoader/runtime/
```

Existing files at those paths are copied and verified under
`<Studio>/.rml-linux-vinegar-backups/<timestamp-id>/original/` before any
replacement. The installer prints the matching `receipt.json` recovery path.
Keep it. Existing `RobloxModLoader/mods`, `config.toml`, libraries, logs, other
Studio files, and Wine/Vinegar configuration are not changed. A clean install
leaves config creation to the loader's built-in defaults at first launch.
No example mods or Luau libraries are installed by this bundle.

## Enable the Wine DLL override

Configure **`dwmapi` = native, builtin** in the Wine prefix used by Vinegar.
The environment spelling is `WINEDLLOVERRIDES="dwmapi=n,b"`. For example, edit
Vinegar's existing `config.toml`, merging this value into its existing table:

```toml
[studio.env]
WINEDLLOVERRIDES = "dwmapi=n,b"
```

Do not add a duplicate TOML table or discard existing overrides; append
`;dwmapi=n,b` to an existing override value instead. Vinegar's
[configuration source](https://github.com/vinegarhq/vinegar/blob/master/internal/config/config.go)
defines `studio.env`, copies these environment values into its Studio prefix,
and adds its own overrides. The config normally lives under
`~/.config/vinegar/config.toml` or
`~/.var/app/org.vinegarhq.Vinegar/config/vinegar/config.toml` for Flatpak.
The installer does not edit this file or Wine's registry.

Start Studio through Vinegar and check its logs for actual RML initialization.
Successful file installation is not proof of initialization or mod compatibility.
If the loader fails, keep the Studio version, Wine/Vinegar versions, receipt,
and logs when reporting the issue.

## Recover the previous loader

Close Studio and Vinegar. Use the exact receipt printed by the installation:

```sh
python3 install.py --studio-dir "/absolute/path/to/active-Studio-version" \
  --rollback "/absolute/path/to/active-Studio-version/.rml-linux-vinegar-backups/<id>/receipt.json" \
  --dry-run

python3 install.py --studio-dir "/absolute/path/to/active-Studio-version" \
  --rollback "/absolute/path/to/active-Studio-version/.rml-linux-vinegar-backups/<id>/receipt.json" \
  --studio-closed
```

Rollback restores the three prior loader paths (or removes them if absent
before installation) and retains backups/receipt. It refuses changed installed
payloads and damaged backups instead of discarding them. Preserve changed files
manually before trying recovery. A failed install attempts automatic recovery;
if recovery also fails, use the retained receipt and original copies. Recovery
from a failed install replaces the paths marked changed in that receipt.
Rollback does not change the DLL override or user mods/config. If this was the
first RML install, remove only the `dwmapi` override you added in Vinegar config
after rollback.

## Build and check a bundle

From the RML source checkout:

```sh
python3 -m unittest discover -s tests/linux-vinegar -v

python3 scripts/linux-vinegar/build_bundle.py \
  --windows-package /path/to/package-windows.zip \
  --source-sha <exact-40-character-Windows-build-commit> \
  --source-run-url https://github.com/<owner>/<repo>/actions/runs/<run-id> \
  --output /path/to/roblox-modloader-linux-vinegar.tar.gz

python3 tests/linux-vinegar/check_real_bundle.py /path/to/roblox-modloader-linux-vinegar.tar.gz
```

Local build inputs are the operator's responsibility. The SHA/run URL are recorded
provenance, not independently authenticated by the local builder. The release
manifest also records the original Windows ZIP SHA256 and hashes of every payload
file. Hashes detect accidental changes; they do not authenticate a release signer.

The dedicated **Linux Vinegar package checks** GitHub workflow runs installer
tests on Ubuntu for scoped pushes/PRs. For a real artifact, dispatch it with
`windows_run_id` and `windows_source_sha` identifying a successful `ci.yml` run
in the same repository. It verifies that run's source SHA, workflow, result,
repository, artifact ID and GitHub artifact SHA256 before downloading its single
`package-windows.zip`. The Windows build may predate the packaging tools: the
manifest identifies the Windows payload source, while the bundle workflow run
identifies the packaging source. The resulting bundle and checksum are uploaded
as `package-linux-vinegar`; the workflow does not publish a release.

The Ubuntu job validates the actual native DLL PE32+ AMD64 headers and required
RML assemblies/runtimeconfigs, then installs and rolls back those real files
in a temporary directory with a synthetic Studio PE fixture. Unit tests also
cover dry runs, backups, failed-copy recovery, changed-file protection, symlinks,
wrong targets/payloads, missing files, ZIP traversal and preserving user data.
Neither the fixture nor this package test launches Wine, Studio, or any DLL.
Existing managed/native CI results remain separate evidence. The separate live
Studio attempt and its RML initialization failure are recorded above.

The archive retains the upstream RML MIT license and third-party notices.
Wine, Vinegar, Windows .NET/CoreCLR, Roblox Studio and user mods are not bundled.
