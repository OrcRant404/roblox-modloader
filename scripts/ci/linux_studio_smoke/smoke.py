#!/usr/bin/env python3
"""One bounded, credential-free Studio GUI/native-loader startup smoke on Linux.

This is not a mod/CLR/engine-edit/Vinegar desktop certification. A pass requires
an unmodified GUI baseline, a fresh native RML initialization log, the real Studio
process/window, a nonblank window capture, and thirty further seconds alive.
"""
import argparse
import ctypes
import datetime
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time
import zipfile

import provision

HERE = Path(__file__).resolve().parent
ANSI = re.compile(r"\x1b\[[0-9;]*m")
REQUIRED = ("Hooking enabled.", "Qt action hook installed.")
WINE_DIAGNOSTICS = "-all,err+all,warn+module,warn+winediag,warn+x11drv,warn+waylanddrv,warn+seh,trace+loaddll"
NATIVE_ERRORS = re.compile(r"\[error\]|\[critical\]|fatal error during bootstrap|"
    r"failed to (?:initialize subsystem|create the bootstrap thread|get vtable|enable .*hook|create .*hook)|"
    r"unhandled (?:exception|page fault)|stack overflow|segmentation fault", re.I)
WINDOW = re.compile(r'^\s*(0x[0-9a-f]+)\s+"([^"]*)".*?(\d+)x(\d+)', re.I)


def x11_environment(environment):
    # This harness owns an Xvfb/Openbox session. Pinned Kombucha patch17 calls
    # strcmp(getenv("XDG_SESSION_TYPE"), "wayland") without a NULL guard.
    # https://github.com/vinegarhq/kombucha/blob/593f74998013e4bba75a05ba17e0d343e96c2e5f/patches/stable/0017-winex11-Don-t-hide-cursor-under-X11-sessions.patch
    return dict(environment, DISPLAY=":99", XDG_SESSION_TYPE="x11")


def display_environment(environment):
    return {key: environment.get(key) for key in ("DISPLAY", "XDG_SESSION_TYPE")}


def loader_status(text):
    clean = ANSI.sub("", text)
    return {"markers": {value: value in clean for value in REQUIRED},
            "errors": [line[-2000:] for line in clean.splitlines() if NATIVE_ERRORS.search(line)][:40]}


def studio_windows(tree):
    result = []
    for line in tree.splitlines():
        match = WINDOW.match(line)
        if not match:
            continue
        handle, title, width, height = match.groups()
        if (int(width) >= 320 and int(height) >= 180 and
                re.search(r"Roblox Studio|Log in to Roblox|Login to Roblox", title, re.I) and
                not re.search(r"splash|error|crash", title, re.I)):
            result.append({"handle": handle, "title": title, "width": int(width), "height": int(height)})
    return result


def window_text_status(text):
    """A GPU/error dialog with Studio's title is not a baseline GUI pass."""
    clean = " ".join(text.casefold().split())
    fatal = re.search(r"(?:graphics|renderer).{0,100}(?:failed|unsupported|not compatible)|"
        r"(?:failed|unable) to (?:initialize|initialise|start)|not compatible with roblox|"
        r"unsupported graphics|fatal error|studio has crashed", clean)
    content = re.search(r"log\s*in|sign\s*in|username|welcome to|recent|templates|new experience|open from file", clean)
    return {"recognizable_studio_or_login_content": bool(content), "fatal_dialog_text": bool(fatal)}


def command(argv, env, log, *, timeout=60, codes=(0,)):
    with log.open("ab") as output:
        result = subprocess.run([str(x) for x in argv], env=env, stdout=output,
                                stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode not in codes:
        raise RuntimeError("Command failed (%s), see %s: %s" % (result.returncode, log.name, argv[0]))


def text_command(argv, env):
    return subprocess.run(argv, env=env, capture_output=True, text=True, timeout=10).stdout


def wine_processes(prefix, expected_exe):
    """Require the actual PE process in this prefix, not explorer's exe argument."""
    result = []
    for item in Path("/proc").iterdir():
        if not item.name.isdecimal():
            continue
        try:
            args = (item / "cmdline").read_bytes().split(b"\0")
            exe = args[0].decode(errors="replace").replace("\\", "/").split("/")[-1]
            if exe.casefold() != expected_exe.casefold():
                continue
            environ = (item / "environ").read_bytes().split(b"\0")
            if ("WINEPREFIX=" + str(prefix)).encode() not in environ:
                continue
            # /proc/stat has a parenthesized command that can contain spaces.
            if (item / "stat").read_text().rpartition(") ")[2].startswith("Z"):
                continue
            result.append(int(item.name))
        except (OSError, ValueError):
            pass
    return sorted(result)


def studio_processes(prefix):
    return wine_processes(prefix, "RobloxStudioBeta.exe")


def collect_wine_dependencies(wine, destination, environment):
    """Diagnose the exact verified backend modules; optional Wayland is not an X11 gate."""
    unix = wine.parent.parent / "lib/wine/x86_64-unix"
    paths = {"wine": wine, **{name: unix / (name + ".so")
             for name in ("ntdll", "win32u", "winex11", "winewayland", "opengl32")}}
    required = {"wine", "ntdll", "win32u", "winex11"}
    env = dict(environment, LD_LIBRARY_PATH=str(unix) +
               (os.pathsep + environment["LD_LIBRARY_PATH"] if environment.get("LD_LIBRARY_PATH") else ""))
    record = {"modules": {}, "optional_x11_dlopen": {}, "required_failures": [],
              "display_environment": display_environment(environment)}
    with (destination / "wine-dependencies.log").open("w") as output:
        for name, path in paths.items():
            if path.is_file():
                result = subprocess.run(["ldd", str(path)], env=env, capture_output=True, text=True, timeout=10)
                text = result.stdout + result.stderr
                failed = result.returncode != 0 or "not found" in text
                record["modules"][name] = {"path": str(path), "returncode": result.returncode,
                                           "unresolved": failed, "ldd": text}
                output.write("%s\n%s\n" % (path, text))
            else:
                failed = True
                record["modules"][name] = {"path": str(path), "missing": True}
            if failed and name in required:
                record["required_failures"].append(name)
    # These names come from the pinned winex11.so, which loads them dynamically.
    for soname in ("libGL.so.1", "libXcomposite.so.1", "libXcursor.so.1", "libXfixes.so.3",
                   "libXi.so.6", "libXinerama.so.1", "libXrandr.so.2", "libXrender.so.1",
                   "libXxf86vm.so.1", "libvulkan.so.1", "libxkbregistry.so.0"):
        try:
            ctypes.CDLL(soname)
            record["optional_x11_dlopen"][soname] = {"available": True}
        except OSError as error:
            record["optional_x11_dlopen"][soname] = {"available": False, "message": str(error)}
    (destination / "wine-dependencies.json").write_text(json.dumps(record, indent=2) + "\n")
    if record["required_failures"]:
        raise RuntimeError("Wine X11 ELF dependencies are unresolved; see wine-dependencies.json: " +
                           ", ".join(record["required_failures"]))
    return record


def probe_windows(tree):
    result = []
    for line in tree.splitlines():
        match = WINDOW.match(line)
        if not match:
            continue
        handle, title, width, height = match.groups()
        # explorer/desktop.c adds the resource title " - Wine Desktop".
        kind = "desktop" if title.casefold() in ("rmlprobe", "rmlprobe - wine desktop") else \
               "notepad" if re.search(r"\bNotepad\b", title, re.I) else None
        if kind and int(width) >= 320 and int(height) >= 180:
            result.append({"handle": handle, "title": title, "kind": kind})
    return result


def window_history(path, started, tree, pids, visible):
    with path.open("a") as output:
        output.write(json.dumps({"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "elapsed_seconds": round(time.monotonic() - started, 3), "pids": pids,
            "visible_windows": visible, "tree": tree}) + "\n")


def wine_gui_probe(wine, prefix, environment, destination):
    """A bounded builtin GUI preflight; never load RML into Notepad."""
    env = dict(environment, WINEDLLOVERRIDES="dwmapi=b;winemenubuilder.exe=;mscoree=;mshtml=")
    record = {"status": "running", "timeout_seconds": 30, "rml_disabled": True,
              "log": "wine-gui-probe.log", "window_history": "wine-gui-probe-windows.jsonl",
              "display_environment": display_environment(env)}
    process = None
    started = time.monotonic()
    ready_at = None
    with (destination / record["log"]).open("wb") as output:
        try:
            process = subprocess.Popen([str(wine), "explorer", "/desktop=RMLProbe,640x480",
                r"C:\windows\system32\notepad.exe"], cwd=prefix, env=env,
                stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
            while time.monotonic() - started < 30:
                tree = text_command(["xwininfo", "-root", "-tree"], env)
                visible = [w for w in probe_windows(tree) if "Map State: IsViewable" in
                           text_command(["xwininfo", "-id", w["handle"]], env)]
                pids = wine_processes(prefix, "notepad.exe")
                window_history(destination / record["window_history"], started, tree, pids, visible)
                if pids and {w["kind"] for w in visible} == {"desktop", "notepad"}:
                    if ready_at is None:
                        ready_at = time.monotonic()
                    if time.monotonic() - ready_at >= 2:
                        notepad = next(w for w in visible if w["kind"] == "notepad")
                        command(["import", "-window", notepad["handle"], destination / "wine-gui-probe.png"],
                                env, destination / "capture.log", timeout=10)
                        record.update(status="passed", windows=visible, notepad_pids=pids)
                        return record
                else:
                    ready_at = None
                time.sleep(1)
            raise RuntimeError("Wine backend GUI probe failed: mapped Notepad/virtual desktop missing; "
                               "see wine-gui-probe.log and wine-gui-probe-windows.jsonl")
        except Exception as error:
            record.update(status="failed", message=str(error))
            try:
                command(["import", "-window", "root", destination / "wine-gui-probe-failure.png"],
                        env, destination / "capture.log", timeout=10)
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
            raise
        finally:
            for option in ("-k", "-w"):
                try:
                    command([wine.parent / "wineserver", option], env, destination / record["log"], timeout=15)
                except (RuntimeError, subprocess.TimeoutExpired):
                    pass
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
            (destination / "wine-gui-probe.json").write_text(json.dumps(record, indent=2) + "\n")


def rml_logs(studio):
    return sorted(p for p in studio.rglob("*.log")
                  if "RobloxModLoader" in p.parts and "logs" in p.parts and p.is_file())


def read_logs(paths):
    return "\n".join(p.read_bytes()[-4 * 1024**2:].decode(errors="replace") for p in paths)


def install_rml(package, provenance, studio):
    record = json.loads(provenance.read_text())
    if provision.digest(package.read_bytes()) != record["windows_package_sha256"]:
        raise ValueError("Windows package does not match verified artifact provenance")
    # The upstream Windows CI package has empty mods. Never smoke arbitrary mods.
    with zipfile.ZipFile(package) as source:
        for item in source.infolist():
            name = str(provision.relative(item.filename))
            if item.is_dir():
                continue
            if name.startswith("RobloxModLoader/mods/"):
                raise ValueError("Native startup smoke requires an empty mods package")
            if not (name in ("dwmapi.dll", "roblox_modloader.dll", "roblox_modloader.pdb",
                              "RobloxModLoader/config.toml", "RobloxModLoader/libraries/bridge.luau",
                              "RobloxModLoader/libraries/logger.luau") or name.startswith("RobloxModLoader/runtime/")):
                raise ValueError("Unexpected Windows artifact file: " + name)
        provision.extract_zip(package.read_bytes(), studio)
    spec = importlib.util.spec_from_file_location("rml_payload", HERE.parents[2] / "scripts/linux-vinegar/payload.py")
    payload = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(payload)
    for name in ("dwmapi.dll", "roblox_modloader.dll"):
        if payload.pe_info(studio / name) != (0x8664, 0x20B, True, False):
            raise ValueError("Loader startup requires a native AMD64 PE DLL: " + name)
    mods = studio / "RobloxModLoader/mods"
    mods.mkdir(parents=True, exist_ok=True)
    if list(mods.iterdir()) or rml_logs(studio):
        raise ValueError("RML smoke must start with empty mods and no stale native logs")
    return record


def collect_logs(prefix, studio, result):
    destination = result / "logs"
    destination.mkdir(exist_ok=True)
    roots = (("studio", studio), ("prefix", prefix))
    count = 0
    for label, root in roots:
        if not root.exists():
            continue
        for path in sorted(root.rglob("*.log")):
            if not path.is_file() or count >= 80:
                continue
            # Do not upload application binaries, dumps, registry, cookies or assets.
            if not ("RobloxModLoader" in path.parts or "Roblox" in path.parts):
                continue
            name = "%s-%03d-%s" % (label, count, path.name)
            (destination / name).write_bytes(path.read_bytes()[-2 * 1024**2:])
            count += 1


def setup_prefix(wine, work, prefix, metadata, environment, log):
    # Pinned Kombucha has no i386 modules. Studio's verified native redist and
    # Wine's builtin CRT support are preflighted during provisioning; no 32-bit
    # VC_redist bootstrap is needed. Vinegar likewise installs only WebView here.
    webview = work / "webview2.exe"
    provision.require_amd64_pe(webview.read_bytes(), webview.name)
    command([wine, "wineboot", "-u"], environment, log, timeout=90)
    command([wine, "reg", "add", r"HKCU\Software\Wine", "/v", "Version", "/d", "win10", "/f"],
            environment, log)
    command([wine, "reg", "add", r"HKCU\Software\Wine\AppDefaults\msedgewebview2.exe",
             "/v", "Version", "/d", "win7", "/f"], environment, log)
    command([wine, webview, "--msedgewebview", "--do-not-launch-msedge", "--system-level"],
            environment, log, timeout=150)
    version = metadata["webview"]["version"]
    if not (prefix / "drive_c/Program Files (x86)/Microsoft/EdgeWebView/Application" / version).is_dir():
        raise RuntimeError("Pinned WebView runtime installation is missing")


def stage(name, args, metadata, env):
    stage_result = args.output / name
    stage_result.mkdir()
    prefix = args.work / (name + "-prefix")
    studio = args.work / (name + "-studio")
    shutil.copytree(args.work / "studio", studio)
    native = name == "rml"
    record = {"status": "running", "native_rml": native, "hold_seconds": 30,
              "wine_gui_probe": "wine-gui-probe.json", "wine_log": "wine-console.log"}
    if native:
        record["rml_provenance"] = install_rml(args.windows_package, args.provenance, studio)
    environment = dict(env, WINEPREFIX=str(prefix), WINEARCH="win64",
        WINEDLLOVERRIDES=("dwmapi=n,b;" if native else "") + "winemenubuilder.exe=;mscoree=;mshtml=",
        WINEDEBUG=WINE_DIAGNOSTICS, LIBGL_ALWAYS_SOFTWARE="1", WINE_D3D_CONFIG="renderer=gl",
        WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--in-process-gpu --use-angle=gl")
    record["display_environment"] = display_environment(environment)
    wine = Path(metadata["wine"])
    wineserver = wine.parent / "wineserver"
    setup_log = stage_result / "setup.log"
    process = None
    output = None
    try:
        setup_prefix(wine, args.work, prefix, metadata, environment, setup_log)
        record["wine_gui_probe_result"] = wine_gui_probe(wine, prefix, environment, stage_result)
        exe = studio / "RobloxStudioBeta.exe"
        record["studio_exe_sha256"] = provision.digest(exe.read_bytes())
        if record["studio_exe_sha256"] != metadata["studio_exe_sha256"]:
            raise ValueError("Baseline and RML must use the same unmodified Studio executable")
        output = (stage_result / "wine-console.log").open("wb")
        process = subprocess.Popen([str(wine), "explorer", "/desktop=RMLSmoke,1280x720", str(exe)],
            cwd=studio, env=environment, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        started = time.monotonic()
        ready_at = None
        held_pid = None
        chosen = None
        while time.monotonic() - started < args.startup_timeout + 35:
            tree = text_command(["xwininfo", "-root", "-tree"], environment)
            (stage_result / "windows.txt").write_text(tree)
            windows = studio_windows(tree)
            visible = []
            for window in windows:
                details = text_command(["xwininfo", "-id", window["handle"]], environment)
                if "Map State: IsViewable" in details:
                    visible.append(window)
            pids = studio_processes(prefix)
            window_history(stage_result / "windows-history.jsonl", started, tree, pids, visible)
            logs = rml_logs(studio)
            native_text = read_logs(logs)
            console = (stage_result / "wine-console.log").read_bytes()[-4 * 1024**2:].decode(errors="replace")
            status = loader_status(native_text)
            failures = status["errors"] + loader_status(console)["errors"]
            if failures:
                record["fatal_errors"] = failures
                raise RuntimeError("Studio/native loader reported a bootstrap, hook or fatal error")
            if list(studio.rglob("*.dmp")) or list(prefix.rglob("*.dmp")):
                raise RuntimeError("A crash dump appeared during Studio startup")
            markers = (bool(logs) and all(status["markers"].values())) if native else not logs
            if visible and pids and markers:
                if ready_at is None:
                    ready_at, held_pid, chosen = time.monotonic(), pids[-1], visible[0]
                    print(name, "GUI/native readiness reached; observing30seconds", flush=True)
                if held_pid not in pids or not any(w["handle"] == chosen["handle"] for w in visible):
                    raise RuntimeError("The verified Studio process/window disappeared after readiness")
                if time.monotonic() - ready_at >= 30:
                    command(["import", "-window", chosen["handle"], stage_result / "studio.png"],
                            environment, stage_result / "capture.log", timeout=15)
                    deviation = text_command(["identify", "-format", "%[fx:standard_deviation]",
                                               str(stage_result / "studio.png")], environment).strip()
                    if not deviation or float(deviation) <= 0.005:
                        raise RuntimeError("Studio window capture is blank or unavailable")
                    ocr = text_command(["tesseract", str(stage_result / "studio.png"), "stdout"], environment)
                    (stage_result / "window-text.txt").write_text(ocr)
                    content = window_text_status(ocr)
                    record["window_content"] = content
                    if content["fatal_dialog_text"] or not content["recognizable_studio_or_login_content"]:
                        raise RuntimeError("Window content is an error dialog or cannot be verified as Studio/login UI")
                    record.update(status="passed", window=chosen, studio_pid=held_pid,
                        observed_after_ready_seconds=round(time.monotonic() - ready_at, 2),
                        native_log_count=len(logs), native=status, screenshot_stddev=float(deviation))
                    return record
            elif ready_at is not None:
                raise RuntimeError("Studio readiness was lost during the observation period")
            if ready_at is None and time.monotonic() - started > args.startup_timeout:
                record.update(last_studio_pids=pids, last_windows=visible, native=status)
                raise RuntimeError("Studio GUI/native startup gate timed out")
            time.sleep(1)
        raise RuntimeError("Studio smoke exceeded its observation bound")
    except Exception as error:
        record.update(status="failed", message=str(error))
        try:
            command(["import", "-window", "root", stage_result / "failure-desktop.png"],
                    environment, stage_result / "capture.log", timeout=10)
        except Exception:
            pass
        raise
    finally:
        if output:
            output.close()
        # Only this fresh prefix; never kill an existing user's Wine session.
        try:
            command([wineserver, "-k"], environment, setup_log, timeout=15)
        except Exception:
            pass
        if process:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        collect_logs(prefix, studio, stage_result)
        (stage_result / "verdict.json").write_text(json.dumps(record, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--windows-package", type=Path, required=True)
    parser.add_argument("--provenance", type=Path, required=True)
    parser.add_argument("--startup-timeout", type=int, default=120)
    for name in ("guid", "manifest-sha256", "installer-sha256"):
        parser.add_argument("--" + name)
    args = parser.parse_args()
    args.work = args.work.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    verdict = {"status": "failed", "scope": "native-loader-startup-only-no-credentials",
               "host": platform.platform(), "architecture": platform.machine(), "stages": {}}
    servers = []
    output_handles = []
    try:
        if sys.platform != "linux" or platform.machine() not in ("x86_64", "amd64"):
            raise RuntimeError("The real smoke requires Linux x86-64")
        if not 30 <= args.startup_timeout <= 180:
            raise ValueError("Startup timeout must be30–180seconds per stage")
        print("Provisioning exact official Studio/Wine/runtime dependencies", flush=True)
        metadata = provision.provision(args)
        (args.output / "provisioning.json").write_text(json.dumps(metadata, indent=2) + "\n")
        env = x11_environment(os.environ)
        verdict["display_environment"] = display_environment(env)
        verdict["wine_dependencies"] = {"json": "wine-dependencies.json", "log": "wine-dependencies.log"}
        collect_wine_dependencies(Path(metadata["wine"]), args.output, env)
        for argv, name in ((["Xvfb", ":99", "-screen", "0", "1280x720x24", "-nolisten", "tcp"], "xvfb.log"),
                           (["openbox"], "openbox.log")):
            handle = (args.output / name).open("wb")
            output_handles.append(handle)
            servers.append(subprocess.Popen(argv, env=env, stdout=handle, stderr=subprocess.STDOUT))
            time.sleep(2)
            if servers[-1].poll() is not None:
                raise RuntimeError(name + " exited before Studio could launch")
        (args.output / "graphics.txt").write_text(text_command(["glxinfo", "-B"], env))
        for name in ("baseline", "rml"):
            verdict["stage"] = name
            verdict["stages"][name] = stage(name, args, metadata, env)
        verdict["status"] = "passed"
        verdict["message"] = "Unmodified Studio GUI and fresh native RML startup gates passed; CLR/mods/editing not tested."
        return 0
    except Exception as error:
        verdict["message"] = str(error)
        print("SMOKE FAILED:", error, file=sys.stderr, flush=True)
        return 1
    finally:
        for server in reversed(servers):
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
        for handle in output_handles:
            handle.close()
        (args.output / "verdict.json").write_text(json.dumps(verdict, indent=2) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
