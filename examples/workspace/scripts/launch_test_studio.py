#!/usr/bin/env python3
"""Launch only the marked Studio copy behind the verified test write fence."""
from __future__ import annotations
import argparse
import json
import hashlib
import os
from pathlib import Path
import subprocess
import time

from runtime_paths import APP, PROJECT, TEST_ID

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--place", type=Path, required=True)
    profiles = parser.add_mutually_exclusive_group(required=True)
    profiles.add_argument("--native-api", action="store_true", help="Run with the rebuilt Core and no external mods")
    profiles.add_argument("--workspace-api", action="store_true", help="Run with the rebuilt Core and Workspace")
    args = parser.parse_args()
    build = PROJECT / "build"
    proof = json.loads((build / "test_profile_guard_proof.json").read_text())
    if proof.get("status") != "passed" or proof.get("failures"):
        parser.error("Run the profile guard successfully before launching Studio")
    paths = {}
    for key in ("sandbox_profile", "c_fixed_user_home"):
        path = (PROJECT / proof[key]).resolve()
        try: path.relative_to(build.resolve())
        except ValueError: parser.error("Guard paths must remain within project build")
        paths[key] = path
    profile = paths["sandbox_profile"]
    text = profile.read_text()
    expected = ['(version 1)', '(allow default)',
        f'(deny file-write* (subpath "{Path.home() / "Library/Roblox"}"))',
        f'(deny file-write* (subpath "{Path.home() / "Library/Preferences"}"))']
    for line in expected:
        if line not in text: parser.error("Guard profile is missing a required write restriction")
    for service in ("com.apple.cfprefsd.agent", "com.apple.cfprefsd.daemon"):
        if f'(global-name "{service}")' not in text: parser.error("Preferences broker guard is missing")
    if '(deny mach-lookup' not in text: parser.error("Preferences broker lookup is not denied")
    app = APP
    marker = json.loads((app.parent / "test-copy.json").read_text())
    if marker.get("bundle_id") != TEST_ID or marker.get("app") != str(app):
        parser.error("Test copy marker does not match")
    executable = app / "Contents/MacOS/RobloxStudio"
    processes = subprocess.check_output(["/bin/ps", "-axo", "command="], text=True)
    if str(executable) in processes: parser.error("Test Studio is already running")
    place = args.place.resolve()
    try: place.relative_to(build.resolve())
    except ValueError: parser.error("Use a disposable place in project build")
    if not place.is_file(): parser.error("Place is missing")
    environment = os.environ.copy()
    environment["CFFIXED_USER_HOME"] = str(paths["c_fixed_user_home"])
    environment["VFX_TEST_PROFILE_VERIFIED"] = "1"
    deployment = json.loads((build / "global-test/deployment.json").read_text())
    if not deployment.get("source_loader_unchanged"):
        parser.error("Native API deployment proof is missing")
    installed_loader = app / 'Contents/MacOS/roblox_modloader.dylib'
    if hashlib.sha256(installed_loader.read_bytes()).hexdigest() != deployment.get('built_loader_sha256'):
        parser.error('Installed loader differs from the verified Core deployment')
    mods = app / "Contents/MacOS/RobloxModLoader/mods"
    expected_mods = {"workspace"} if args.workspace_api else set()
    if set(path.name for path in mods.iterdir()) != expected_mods:
        parser.error("Launch requires exactly the Workspace mod" if args.workspace_api
                     else "Core-only launch requires zero external mods")
    if deployment.get("profile") != ("workspace" if args.workspace_api else "native-api"):
        parser.error("Native deployment profile does not match requested launch")
    environment["MODLOADER_API_PROOF_DIR"] = str(build / "global-test/reports")
    command = ["/usr/bin/sandbox-exec", "-f", str(profile), str(executable),
        "--localPlaceFile", str(place), "--task", "EditFile"]
    log = app.parent / f"process-{time.time_ns()}.log"
    with log.open("wb") as output:
        process = subprocess.Popen(command, env=environment, stdout=output, stderr=subprocess.STDOUT,
            start_new_session=True)
    state = {"pid":process.pid,"executable":str(executable),"log":str(log),
        "sandbox_profile":str(profile),"c_fixed_user_home":environment["CFFIXED_USER_HOME"],
        "place":str(place)}
    (app.parent / "current-launch.json").write_text(json.dumps(state,indent=2)+"\n")
    print(json.dumps(state,indent=2))

if __name__ == "__main__": main()
