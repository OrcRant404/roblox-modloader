#!/usr/bin/env python3
"""Check that a test profile cannot write shared Roblox or preference data.

This is a standalone macOS sandbox probe. It uses a fresh profile and
CFPreferences domain for each run, and persists its evidence under build/.
"""

from __future__ import annotations

import datetime as dt
import json
import os
import pathlib
import re
import subprocess
import sys
import time
import uuid


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
PROOF = BUILD / "test_profile_guard_proof.json"
DOMAIN_PREFIX = "com.orcrant.vfx-isolation-proof"
SHARED_HOME = pathlib.Path.home()
SHARED_ROBLOX = SHARED_HOME / "Library" / "Roblox"
SHARED_PREFS = SHARED_HOME / "Library" / "Preferences"


SOURCE_TEXT = r'''#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

static int attempt_file(const char *label, const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        int saved = errno;
        std::printf("%s=denied errno=%d (%s)\n", label, saved, std::strerror(saved));
        return (saved == EPERM || saved == EACCES) ? 0 : 1;
    }

    const char byte = 'x';
    ssize_t written = write(fd, &byte, 1);
    int write_error = errno;
    close(fd);
    int unlink_result = unlink(path);
    std::printf("%s=UNEXPECTEDLY_ALLOWED written=%ld write_errno=%d unlink=%s\n",
                label, static_cast<long>(written), write_error,
                unlink_result == 0 ? "cleaned" : std::strerror(errno));
    return 2;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 5) {
            std::fprintf(stderr,
                         "usage: %s write|read roblox-scratch preferences-scratch domain\n",
                         argv[0]);
            return 64;
        }

        const char *mode = argv[1];
        const char *domain_name = argv[4];
        const char *home = std::getenv("HOME");
        const char *fixed_home = std::getenv("CFFIXED_USER_HOME");
        std::printf("HOME=%s\n", home ? home : "<unset>");
        std::printf("CFFIXED_USER_HOME=%s\n", fixed_home ? fixed_home : "<unset>");
        std::printf("NSHomeDirectory=%s\n", NSHomeDirectory().UTF8String ?: "<nil>");

        CFStringRef domain = CFStringCreateWithCString(
            kCFAllocatorDefault, domain_name, kCFStringEncodingUTF8);
        CFStringRef key = CFSTR("isolation_probe_value");

        if (std::strcmp(mode, "write") == 0) {
            int result = 0;
            result |= attempt_file("Roblox_scratch_write", argv[2]);
            result |= attempt_file("shared_preferences_scratch_write", argv[3]);

            CFStringRef value = CFSTR("test-profile-only");
            CFPreferencesSetAppValue(key, value, domain);
            Boolean synced = CFPreferencesAppSynchronize(domain);
            std::printf("CFPreferencesAppSynchronize=%s\n", synced ? "true" : "false");
            CFTypeRef readback = CFPreferencesCopyAppValue(key, domain);
            std::printf("CFPreferences.local_readback=%s\n",
                        readback ? "present" : "missing");
            if (readback) CFRelease(readback);
            CFRelease(domain);
            return result;
        }

        if (std::strcmp(mode, "read") == 0) {
            CFTypeRef readback = CFPreferencesCopyAppValue(key, domain);
            std::printf("CFPreferences.persisted_readback=%s\n",
                        readback ? "present" : "missing");
            if (readback) CFRelease(readback);
            CFRelease(domain);
            return 0;
        }

        CFRelease(domain);
        std::fprintf(stderr, "unknown mode: %s\n", mode);
        return 64;
    }
}
'''


def run(cmd: list[str], *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(cmd))
    return subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, env=env)


def matching_preferences(root: pathlib.Path, domain: str) -> list[pathlib.Path]:
    if not root.exists():
        return []
    return sorted(root.glob(f"**/{domain}.plist"))


def proof_path(path: pathlib.Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def save_proof(proof: dict[str, object]) -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    temporary = PROOF.with_suffix(".tmp")
    temporary.write_text(json.dumps(proof, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(PROOF)


def output_value(output: str, key: str) -> str | None:
    match = re.search(rf"^{re.escape(key)}=(.*)$", output, flags=re.MULTILINE)
    return match.group(1) if match else None


def main() -> int:
    BUILD.mkdir(parents=True, exist_ok=True)
    nonce = uuid.uuid4().hex
    domain = f"{DOMAIN_PREFIX}.{nonce}"
    profile_home = BUILD / f"test_profile_home_{nonce}"
    profile = BUILD / f"test_profile_guard_{nonce}.sb"
    source = BUILD / f"test_profile_guard_{nonce}.mm"
    probe = BUILD / f"test_profile_guard_{nonce}"
    profile_prefs = profile_home / "Library" / "Preferences"
    roblox_scratch = SHARED_ROBLOX / f".codex-isolation-probe-{nonce}"
    prefs_scratch = SHARED_PREFS / f".codex-isolation-probe-{nonce}"

    proof: dict[str, object] = {
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "nonce": nonce,
        "domain": domain,
        "home_environment": os.environ.get("HOME"),
        "c_fixed_user_home": proof_path(profile_home),
        "status": "not_run",
        "failures": [],
    }

    def stop_before_run(message: str) -> int:
        proof["status"] = "blocked_before_probe"
        proof["failures"] = [message]
        save_proof(proof)
        print(message, file=sys.stderr)
        print(f"proof_json={PROOF}")
        return 2

    if sys.platform != "darwin":
        return stop_before_run("This probe requires macOS sandbox-exec and CoreFoundation.")
    if not SHARED_ROBLOX.is_dir() or not SHARED_PREFS.is_dir():
        return stop_before_run("Expected shared Roblox and Preferences directories are unavailable.")
    if profile_home.exists():
        return stop_before_run(f"Fresh profile path already exists; refusing reuse: {profile_home}")

    domain_candidates = matching_preferences(SHARED_PREFS, domain)
    if domain_candidates:
        return stop_before_run(
            f"Fresh domain unexpectedly exists in shared Preferences: {domain_candidates}")
    if roblox_scratch.exists() or prefs_scratch.exists():
        return stop_before_run("Unexpected shared scratch-name collision; aborting.")

    profile_home.mkdir(mode=0o700)
    profile_home.chmod(0o700)
    profile_prefs.mkdir(parents=True, mode=0o700)
    profile_prefs.chmod(0o700)

    profile.write_text(
        "(version 1)\n"
        "(allow default)\n"
        f'(deny file-write* (subpath "{SHARED_ROBLOX}"))\n'
        f'(deny file-write* (subpath "{SHARED_PREFS}"))\n'
        '(deny mach-lookup (global-name "com.apple.cfprefsd.agent"))\n'
        '(deny mach-lookup (global-name "com.apple.cfprefsd.daemon"))\n',
        encoding="utf-8",
    )
    source.write_text(SOURCE_TEXT, encoding="utf-8")

    compile_result = run([
        "/usr/bin/clang++", "-std=c++17", "-x", "objective-c++", str(source),
        "-framework", "Foundation", "-framework", "CoreFoundation", "-o", str(probe),
    ])
    print(compile_result.stdout, end="")
    proof["compile_output"] = compile_result.stdout
    proof["compile_exit"] = compile_result.returncode
    if compile_result.returncode:
        proof["status"] = "compile_failed"
        proof["failures"] = [f"Compile failed with status {compile_result.returncode}."]
        save_proof(proof)
        print(f"proof_json={PROOF}")
        return compile_result.returncode

    environment = os.environ.copy()
    environment["CFFIXED_USER_HOME"] = str(profile_home)
    # Deliberately preserve HOME and every other inherited environment setting.
    sandbox_prefix = ["/usr/bin/sandbox-exec", "-f", str(profile), str(probe)]
    write_result = run(sandbox_prefix + [
        "write", str(roblox_scratch), str(prefs_scratch), domain,
    ], env=environment)
    print(write_result.stdout, end="")
    print(f"write_probe_exit={write_result.returncode}")

    read_result = run(sandbox_prefix + [
        "read", str(roblox_scratch), str(prefs_scratch), domain,
    ], env=environment)
    print(read_result.stdout, end="")
    print(f"read_probe_exit={read_result.returncode}")

    # Allow a successful CFPreferences write time to reach its backing plist.
    for _ in range(20):
        if (matching_preferences(profile_prefs, domain)
                or matching_preferences(SHARED_PREFS, domain)):
            break
        time.sleep(0.25)
    isolated_files = matching_preferences(profile_prefs, domain)
    escaped_files = matching_preferences(SHARED_PREFS, domain)
    profile_sync = output_value(write_result.stdout, "CFPreferencesAppSynchronize")
    persisted_readback = output_value(read_result.stdout, "CFPreferences.persisted_readback")

    if escaped_files:
        preferences_outcome = "escaped_to_shared_preferences"
    elif isolated_files and persisted_readback == "present":
        preferences_outcome = "confined_to_test_profile"
    elif (not isolated_files and profile_sync == "false"
          and persisted_readback == "missing"):
        preferences_outcome = "rejected_without_persistence"
    else:
        preferences_outcome = "unverified"

    direct_roblox_denied = "Roblox_scratch_write=denied errno=" in write_result.stdout
    direct_preferences_denied = (
        "shared_preferences_scratch_write=denied errno=" in write_result.stdout)
    roblox_scratch_exists = roblox_scratch.exists()
    prefs_scratch_exists = prefs_scratch.exists()
    failures: list[str] = []
    if not direct_roblox_denied:
        failures.append("Direct write to shared Roblox data was not denied.")
    if not direct_preferences_denied:
        failures.append("Direct write to shared Preferences was not denied.")
    if escaped_files:
        failures.append("CFPreferences created a domain file in shared Preferences.")
    if preferences_outcome == "unverified":
        failures.append("CFPreferences was neither confined to the test profile nor clearly rejected.")
    if roblox_scratch_exists or prefs_scratch_exists:
        failures.append("A protected direct-write scratch file remains.")
    if write_result.returncode or read_result.returncode:
        failures.append("The standalone probe process returned a non-zero status.")

    proof.update({
        "status": "passed" if not failures else "failed",
        "failures": failures,
        "preferences_outcome": preferences_outcome,
        "cfpreferences_sync": profile_sync,
        "cfpreferences_persisted_readback": persisted_readback,
        "profile_preferences": [proof_path(path) for path in isolated_files],
        "shared_preferences_domain_files": [str(path) for path in escaped_files],
        "direct_shared_roblox_write_denied": direct_roblox_denied,
        "direct_shared_preferences_write_denied": direct_preferences_denied,
        "roblox_scratch_exists": roblox_scratch_exists,
        "shared_preferences_scratch_exists": prefs_scratch_exists,
        "profile_home_mode": oct(profile_home.stat().st_mode & 0o777),
        "write_probe_exit": write_result.returncode,
        "write_probe_output": write_result.stdout,
        "read_probe_exit": read_result.returncode,
        "read_probe_output": read_result.stdout,
        "sandbox_profile": proof_path(profile),
        "probe_source": proof_path(source),
        "probe_binary": proof_path(probe),
    })
    save_proof(proof)
    print(f"preferences_outcome={preferences_outcome}")
    print(f"profile_preferences={[proof_path(path) for path in isolated_files] or '<none>'}")
    print(f"shared_preferences_domain_files={escaped_files or '<none>'}")
    print(f"roblox_scratch_exists={roblox_scratch_exists}")
    print(f"shared_preferences_scratch_exists={prefs_scratch_exists}")
    print(f"proof_json={PROOF}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
