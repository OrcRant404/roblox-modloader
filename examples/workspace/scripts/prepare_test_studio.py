#!/usr/bin/env python3
"""Create a marked, matching Studio base copy with no installed mods."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import time


from runtime_paths import APP, MANIFEST, PROJECT, TEST_ID


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def code_sha256(path: Path, manifest: dict) -> str:
    section = manifest['code_section']
    data = path.read_bytes()
    return hashlib.sha256(data[section['offset']:section['offset'] + section['size']]).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--studio", type=Path, required=True)
    args = parser.parse_args()
    source = args.studio.resolve()
    root = PROJECT / "build/test-studio"
    app = APP
    marker = root / "test-copy.json"
    source_loader = source / "Contents/MacOS/roblox_modloader.dylib"
    source_executable = source / "Contents/MacOS/RobloxStudio"
    if not source_loader.is_file() or not source_executable.is_file():
        parser.error("Source RML Studio loader or executable is missing")
    if source == app.resolve():
        parser.error("Source must be separate from the test copy")
    manifest = json.loads(MANIFEST.read_text())
    if code_sha256(source_executable, manifest) != manifest['code_sha256']:
        parser.error("Verified engine manifest does not match the source Studio")
    original_hash = sha256(source_loader)
    root.mkdir(parents=True, exist_ok=True)
    if app.exists():
        if not marker.is_file():
            parser.error("Refusing to modify an unmarked existing app")
        previous = json.loads(marker.read_text())
        if (previous.get("bundle_id") != TEST_ID or previous.get("source") != str(source)
                or previous.get("app") != str(app)):
            parser.error("Existing marker does not match this test copy")
        result = subprocess.run(["/bin/ps", "-axo", "command="], capture_output=True, text=True)
        if result.returncode != 0:
            parser.error("Cannot confirm the test copy is stopped; use process-list permission")
        if str(app / "Contents/MacOS/RobloxStudio") in result.stdout:
            parser.error("Close the test Studio before preparing the base copy")
        if code_sha256(app / "Contents/MacOS/RobloxStudio", manifest) != manifest['code_sha256']:
            parser.error("Verified engine manifest does not match the existing test copy")
    else:
        # APFS clone: writes stay in the copy and the source remains untouched.
        subprocess.run(["/bin/cp", "-cR", str(source), str(app)], check=True)
        marker.write_text(json.dumps({"source": str(source), "bundle_id": TEST_ID,
                                      "app": str(app)}, indent=2) + "\n")

    macos = app / "Contents/MacOS"
    mods = macos / "RobloxModLoader/mods"
    preserved = root / "original-mods"
    if preserved.exists():
        preserved = root / f"mods-before-base-{time.time_ns()}"
    if mods.exists():
        mods.rename(preserved)
    mods.mkdir(parents=True, exist_ok=True)
    for profile, filename in (("global-test", "NativeApiTest.rbxlx"),
                              ("workspace-test", "WorkspaceApiTest.rbxlx")):
        directory = PROJECT / "build" / profile
        (directory / "plugins").mkdir(parents=True, exist_ok=True)
        (directory / "reports").mkdir(parents=True, exist_ok=True)
        place = directory / filename
        if not place.exists():
            place.write_bytes((PROJECT / "fixtures/WorkspaceApiTest.rbxlx").read_bytes())

    info_path = app / "Contents/Info.plist"
    info = plistlib.loads(info_path.read_bytes())
    info["CFBundleIdentifier"] = TEST_ID
    info["CFBundleName"] = "RML Workspace Test"
    info["CFBundleDisplayName"] = "RML Workspace Test"
    info_path.write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
    entitlement_result = subprocess.run([
        "/usr/bin/codesign", "--display", "--entitlements", ":-", str(source)
    ], capture_output=True, check=True)
    sign = ["/usr/bin/codesign", "--force", "--deep", "--sign", "-"]
    if entitlement_result.stdout.strip():
        entitlements = root / "source-entitlements.plist"
        plistlib.loads(entitlement_result.stdout)  # refuse non-plist diagnostic output
        entitlements.write_bytes(entitlement_result.stdout)
        sign.extend(["--entitlements", str(entitlements)])
    subprocess.run([*sign, str(app)], check=True)
    subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)], check=True)
    if sha256(source_loader) != original_hash:
        raise RuntimeError("Source loader changed during preparation; investigate concurrent edits")
    if code_sha256(app / "Contents/MacOS/RobloxStudio", manifest) != manifest['code_sha256']:
        raise RuntimeError("Signing changed the engine code identity; re-verify before building")
    metadata = {
        "source": str(source), "bundle_id": TEST_ID, "app": str(app),
        "source_loader_sha256": original_hash, "test_engine_code_sha256": manifest['code_sha256'],
        "mods": [], "scope": "Marked Studio base; use the guarded launcher to isolate shared data",
    }
    marker.write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
