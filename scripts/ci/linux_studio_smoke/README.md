# Linux Wine Studio native startup smoke

Manual workflow: `.github/workflows/linux-studio-smoke.yml`. This is an actual
Studio GUI launch attempt on Ubuntu x64, separate from the filesystem/installer
checks in `linux-vinegar.yml`. It accepts no Roblox credentials and installs no
mods. It does not test CLR hosting, managed mods, engine edits, saves, thumbnails,
or the Custom Templates port.

The workflow first fetches one successful same-repository Windows CI artifact
with exact source SHA and GitHub digest verification. It provisions the exact
public Studio deployment and runtimes pinned in `lock.json`, then launches:

1. An untouched Studio deployment in a new Wine prefix, requiring an actual
   visible Studio/login window and process to remain alive for30seconds.
2. The same Studio executable with the verified RML package, empty mods and
   another new prefix, requiring fresh native log messages `Hooking enabled.`
   and `Qt action hook installed.`, no bootstrap/subsystem/missing-vtable/hook
   or fatal-crash errors, the actual Studio process/window alive30further
   seconds, and a nonblank capture of that window. Passive OCR must recognize
   Studio/login content and reject a graphics/initialization error dialog, even
   when that dialog's title contains Roblox Studio.

Both stages use pinned VinegarHQ Kombucha, Xvfb, Openbox and a Wine virtual
desktop with software OpenGL. A baseline renderer/provisioning failure is a
failed smoke, never a loader pass. A native failure after a passing baseline
can distinguish loader/build incompatibility from baseline setup failure.
The current Windows Studio pin may differ from the Mac-supported version; it
is an explicit Windows compatibility attempt, not an assumed ABI match.

`verdict.json`, stage verdicts, deployment/package hashes, graphics inventory,
window inventory, curated logs and window screenshots are uploaded. Downloaded
Studio/Wine/runtime binaries, Wine prefixes, registry/cookies and crash dumps
are **not** uploaded. Every job failure has a verdict; timeout/cancellation can
only be `not_completed`, not a pass. There are no logins or interactions beyond
passive process/window/capture observation. No external UI input is sent. No
static parser test is a live pass.

For local Linux x64 reproduction after installing the workflow dependencies:

```sh
python3 scripts/linux-vinegar/fetch_artifact.py \
  --repository OrcRant404/roblox-modloader --run-id <successful-ci-run> \
  --source-sha <exact-sha> --output /tmp/verified-rml-smoke
python3 scripts/ci/linux_studio_smoke/smoke.py \
  --work /tmp/fresh-rml-smoke-work --output /tmp/fresh-rml-smoke-results \
  --windows-package /tmp/verified-rml-smoke/package-windows.zip \
  --provenance /tmp/verified-rml-smoke/provenance.json
```

Python3.12 is required for safe tar extraction. Work/output directories must
be new; the harness never targets an existing user's Studio or Wine prefix.
The only `wineserver -k` commands carry that stage's private `WINEPREFIX`.

Offline tests (no Studio/Wine launch):

```sh
python3 -m unittest discover -s scripts/ci/linux_studio_smoke -p 'test_*.py' -v
```

## Provisioning sources and pins

- [Vinegar bootstrap deployment setup](https://github.com/vinegarhq/vinegar/blob/b76ae5af1e5e8bde2052ea8bce17961c622dd4fb/cmd/vinegar/bootstrapper_setup_rblx.go)
  uses GetPackages, BinaryDirectories and WriteAppSettings from
  [rbxbin14ca138004f5](https://github.com/sewnie/rbxbin/tree/14ca138004f5).
  The provisioner independently implements those public deployment formats,
  extracting the actual directory map from the pinned installer's PE `.rdata`.
  All manifest packages are installed, including fonts, plugins, redist and
  WebView assets; this is not an executable-only deployment.
- [Official LIVE WindowsStudio64 endpoint](https://clientsettingscdn.roblox.com/v2/client-version/WindowsStudio64/channel/LIVE)
  resolved on2026-10-20 to `version-4f087c32742644ab`, version0.743.0.7431133.
  Manifest and installer SHA256 pins are explicit manual-dispatch inputs.
  Package size/MD5 entries are bound by that pinned official manifest; each
  downloaded package's SHA256 is also recorded. Studio content remains local.
- [Kombucha stable+20261018103513](https://github.com/vinegarhq/kombucha/releases/tag/stable%2B20261018103513)
  uses GitHub's SHA256 digest pinned in `lock.json`.
- VC++ x64 redistributable is downloaded from the fixed official Microsoft
  delivery URL and SHA256 in the lock, not a floating aka.ms URL at runtime.
- WebView144.0.3719.92 follows the pinned Vinegar default. The official Microsoft
  CDP metadata must match its pinned runtime SHA256. HTTP `msedge.b/f` delivery
  hosts map only to their documented HTTPS `msedge.sb/sf` counterparts, preserving
  the returned file path and signed query. These are separate hosts in Microsoft's
  [official endpoint list](https://learn.microsoft.com/en-us/deployedge/microsoft-edge-security-endpoints);
  changing only the scheme on an HTTP host causes a certificate-name mismatch.
  The `msedge.sb.tlu` counterpart was verified with normal TLS to deliver all
  185,153,080 bytes with SHA256
  `74d0bbcd2396ac37e89e47e8ce37a598fbb3206abd8bc6fb1f7de24b3fe924cd`.
  Downloads and redirects must stay HTTPS and within the exact documented
  delivery-host allowlist. No HTTP fallback or TLS-verification bypass is used.
  `microsoft-root-2011.pem` is the public Microsoft Root Certificate Authority2011
  trust anchor used by [sewnie/wine's WebView helper](https://github.com/sewnie/wine/blob/436144de12bd/webview2/webview.go).
  Its SHA256 is pinned too; certificate verification is never disabled. The
  app-specific Win7 WebView override and install arguments follow that helper.
- Renderer environment follows [Vinegar config](https://github.com/vinegarhq/vinegar/blob/b76ae5af1e5e8bde2052ea8bce17961c622dd4fb/internal/config/config.go).
  Software rendering is a CI experiment; success is not GPU or full Vinegar
  desktop validation.

The new helper code is part of this repository under its MIT license. Source
URLs are recorded for format/procedure attribution; no Vinegar application or
Roblox package is vendored here. The certificate is public certificate data.
