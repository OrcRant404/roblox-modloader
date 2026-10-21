"""Offline regression tests of actual smoke parsers/gates, not a live Studio pass."""
import io
import json
from pathlib import Path
import stat
import struct
import tempfile
import unittest
from unittest import mock
import zipfile

import provision
import smoke


def pe_with_mapping(mapping):
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<H", data, 134, 1)
    struct.pack_into("<H", data, 148, 240)
    section = 128 + 24 + 240
    data[section:section + 8] = b".rdata\0\0"
    text = b"\0" + json.dumps(mapping, separators=(",", ":")).encode() + b"\0"
    struct.pack_into("<II", data, section + 16, len(text), 700)
    data[700:700 + len(text)] = text
    return bytes(data)


def runtime_pe(*, machine=0x8664, dll=False):
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<HH", data, 68, machine, 1)
    struct.pack_into("<HH", data, 84, 240, 0x2000 if dll else 0)
    struct.pack_into("<H", data, 88, 0x20B if machine == 0x8664 else 0x10B)
    return bytes(data)


class ProvisionTests(unittest.TestCase):
    def test_runtime_preflight_rejects_32bit_bootstrap_and_wrong_dll_type(self):
        provision.require_amd64_pe(runtime_pe(), "actual-x64.exe")
        provision.require_amd64_pe(runtime_pe(dll=True), "actual-x64.dll", dll=True)
        for data, dll in ((runtime_pe(machine=0x14C), False), (runtime_pe(), True),
                          (runtime_pe(dll=True), False), (runtime_pe()[:200], False), (b"MZ", False)):
            with self.subTest(dll=dll, size=len(data)), self.assertRaises(ValueError):
                provision.require_amd64_pe(data, "misleading-x64-name.exe", dll=dll)

    def test_bundled_runtime_inventory_requires_native_crt_and_wine_atomic_wait(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            studio = root / "studio"
            studio.mkdir()
            for name in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "ucrtbase.dll"):
                (studio / name).write_bytes(runtime_pe(dll=True))
            wine = root / "kombucha/bin/wine"
            modules = root / "kombucha/lib/wine/x86_64-windows"
            modules.mkdir(parents=True)
            atomic = modules / "msvcp140_atomic_wait.dll"
            with self.assertRaises(FileNotFoundError):
                provision.runtime_inventory(studio, wine)
            atomic.write_bytes(runtime_pe(dll=True))
            inventory = provision.runtime_inventory(studio, wine)
            self.assertEqual(inventory["wine_builtin"][atomic.name], provision.digest(atomic.read_bytes()))
            self.assertEqual(len(inventory["studio_redist"]), 4)
            atomic.write_bytes(runtime_pe(machine=0x14C, dll=True))
            with self.assertRaises(ValueError):
                provision.runtime_inventory(studio, wine)

    def test_microsoft_documented_https_delivery_counterparts(self):
        path = "/filestreamingservice/files/fd553f7b-c89a-4da7-bd24-85a214cc91b0?P1=123&P4=token%2Bvalue"
        for http_host, https_host in provision.WEBVIEW_HTTPS_COUNTERPARTS.items():
            with self.subTest(host=http_host):
                expected = "https://" + https_host + path
                self.assertEqual(provision.webview_delivery_url("http://" + http_host + path), expected)
                self.assertEqual(provision.webview_delivery_url(expected), expected)
        host = "msedge.sb.tlu.dl.delivery.mp.microsoft.com"
        for bad in ("http://" + host + path, "ftp://" + host + path,
                    "https://msedge.b.tlu.dl.delivery.mp.microsoft.com" + path,
                    "http://evil.dl.delivery.mp.microsoft.com" + path,
                    "https://" + host + ".example.org" + path,
                    "https://user@" + host + path, "https://" + host + ":8443" + path,
                    "https://" + host + path + "#fragment", "https://" + host + "/other-file"):
            with self.subTest(url=bad), self.assertRaises(ValueError):
                provision.webview_delivery_url(bad)

    def test_download_redirects_cannot_downgrade_or_escape_delivery_hosts(self):
        good = "https://msedge.sb.tlu.dl.delivery.mp.microsoft.com/file"
        handler = provision.ProvisionRedirectHandler(provision.WEBVIEW_HTTPS_HOSTS)
        request = provision.urllib.request.Request(good)
        self.assertEqual(handler.redirect_request(request, None, 302, "Found", {}, good).full_url, good)
        for bad in (good.replace("https:", "http:"), "https://example.org/file",
                    "https://user@msedge.sb.tlu.dl.delivery.mp.microsoft.com/file"):
            with self.subTest(url=bad):
                with self.assertRaises(ValueError):
                    handler.redirect_request(request, None, 302, "Found", {}, bad)
                with mock.patch.object(provision.urllib.request, "build_opener") as opener:
                    with self.assertRaises(ValueError):
                        provision.get(bad, 100, allowed_hosts=provision.WEBVIEW_HTTPS_HOSTS)
                    opener.assert_not_called()

    def test_https_download_keeps_sha256_and_size_verification(self):
        url = "https://msedge.sb.tlu.dl.delivery.mp.microsoft.com/file"
        payload = b"MZ pinned runtime bytes"
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.url = url
        response.read.return_value = payload
        opener = mock.Mock()
        opener.open.return_value = response
        with mock.patch.object(provision.urllib.request, "build_opener", return_value=opener):
            self.assertEqual(provision.verified(url, provision.digest(payload), 100, len(payload),
                             allowed_hosts=provision.WEBVIEW_HTTPS_HOSTS), payload)
            with self.assertRaises(ValueError):
                provision.verified(url, "0" * 64, 100, len(payload),
                                   allowed_hosts=provision.WEBVIEW_HTTPS_HOSTS)
            with self.assertRaises(ValueError):
                provision.verified(url, provision.digest(payload), 100, len(payload) + 1,
                                   allowed_hosts=provision.WEBVIEW_HTTPS_HOSTS)
            response.url = "https://example.org/file"
            with self.assertRaises(ValueError):
                provision.verified(url, provision.digest(payload), 100, len(payload),
                                   allowed_hosts=provision.WEBVIEW_HTTPS_HOSTS)

    def test_manifest_actual_grammar_and_duplicates(self):
        valid = b"v0\r\nRobloxStudio.zip\r\n" + b"a" * 32 + b"\r\n10\r\n20\r\n"
        self.assertEqual(provision.packages(valid)[0]["expanded"], 20)
        for broken in (valid[:-5], valid.replace(b"v0", b"v1"), valid + valid.split(b"\r\n", 1)[1],
                       valid.replace(b"RobloxStudio.zip", b"../Studio.zip")):
            with self.subTest(broken=broken[:20]), self.assertRaises(ValueError):
                provision.packages(broken)

    def test_actual_pe_section_mapping_and_bounds(self):
        image = pe_with_mapping({"RobloxStudio.zip": "", "content-sky.zip": "content\\sky\\"})
        self.assertEqual(provision.installer_directories(image)["content-sky.zip"], "content/sky")
        with self.assertRaises(ValueError):
            provision.installer_directories(pe_with_mapping({"RobloxStudio.zip": "..\\escape"}))
        with self.assertRaises(ValueError):
            provision.installer_directories(b"not a PE")
        with self.assertRaises(ValueError):
            provision.installer_directories(pe_with_mapping({"other.zip": "content"}))

    def test_zip_backslashes_and_path_escape(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            data = io.BytesIO()
            with zipfile.ZipFile(data, "w") as archive:
                archive.writestr("folder\\file.txt", b"actual payload")
            provision.extract_zip(data.getvalue(), root)
            self.assertEqual((root / "folder/file.txt").read_bytes(), b"actual payload")
            for name in ("../escape", "/absolute", "C:\\escape"):
                with self.subTest(name=name):
                    data = io.BytesIO()
                    with zipfile.ZipFile(data, "w") as archive:
                        archive.writestr(name, b"bad")
                    with self.assertRaises(ValueError):
                        provision.extract_zip(data.getvalue(), root)
            data = io.BytesIO()
            with zipfile.ZipFile(data, "w") as archive:
                info = zipfile.ZipInfo("link")
                info.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(info, "outside")
            with self.assertRaises(ValueError):
                provision.extract_zip(data.getvalue(), root)

    def test_actual_roblox_dos_directory_markers(self):
        # Metadata observed in all 34 pinned deployment ZIP central directories:
        # DOS creator, directory attribute 0x10, empty stored data, backslash roots.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "deployment"
            data = io.BytesIO()
            with zipfile.ZipFile(data, "w") as archive:
                for name in ("\\", "\\Optimized_Embedded_Signature\\",
                             "\\Qt5\\imageformats\\", "/empty-directory/"):
                    info = zipfile.ZipInfo(name)
                    info.create_system = 0
                    info.external_attr = 0x10
                    archive.writestr(info, b"")
                archive.writestr("Qt5\\imageformats\\plugin.dll", b"actual payload")
            provision.extract_zip(data.getvalue(), root)
            self.assertTrue((root / "Optimized_Embedded_Signature").is_dir())
            self.assertTrue((root / "empty-directory").is_dir())
            self.assertEqual((root / "Qt5/imageformats/plugin.dll").read_bytes(), b"actual payload")

    def test_rooted_zip_entries_cannot_masquerade_as_dos_directories(self):
        invalid = [
            ("/absolute", 0, 0x20, b""),
            ("\\absolute", 0, 0x20, b""),
            ("/absolute/", 0, 0x20, b""),
            ("\\payload\\", 0, 0x10, b"file data"),
            ("\\\\server\\share\\", 0, 0x10, b""),
            ("\\..\\escape\\", 0, 0x10, b""),
            ("/dir/../escape/", 0, 0x10, b""),
            ("\\C:\\escape\\", 0, 0x10, b""),
            ("/unix-directory/", 3, (stat.S_IFDIR | 0o755) << 16, b""),
            ("/", 3, (stat.S_IFLNK | 0o777) << 16, b""),
            ("\\link\\", 0, ((stat.S_IFLNK | 0o777) << 16) | 0x10, b""),
        ]
        with tempfile.TemporaryDirectory() as temp:
            for name, creator, attributes, payload in invalid:
                with self.subTest(name=name, creator=creator, attributes=attributes):
                    data = io.BytesIO()
                    with zipfile.ZipFile(data, "w") as archive:
                        info = zipfile.ZipInfo(name)
                        info.create_system = creator
                        info.external_attr = attributes
                        archive.writestr(info, payload)
                    with self.assertRaises(ValueError):
                        provision.extract_zip(data.getvalue(), Path(temp) / "deployment")

    def test_empty_manifest_fails_closed(self):
        # An empty deployment cannot be an actual baseline, even with v0.
        self.assertEqual(provision.packages(b"v0\r\n"), [])


class GateTests(unittest.TestCase):
    def test_harness_declares_its_xvfb_x11_session_even_without_desktop_environment(self):
        for original in ({}, {"DISPLAY": ":0", "XDG_SESSION_TYPE": "wayland", "KEEP": "value"}):
            with self.subTest(original=original):
                saved = dict(original)
                environment = smoke.x11_environment(original)
                self.assertEqual(smoke.display_environment(environment), {"DISPLAY": ":99", "XDG_SESSION_TYPE": "x11"})
                self.assertEqual(original, saved)
                if "KEEP" in saved:
                    self.assertEqual(environment["KEEP"], saved["KEEP"])

    def test_wine_dependency_diagnostics_separate_required_x11_and_optional_wayland(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            wine = root / "kombucha/bin/wine"
            wine.parent.mkdir(parents=True)
            wine.write_bytes(b"verified Wine image")
            unix = root / "kombucha/lib/wine/x86_64-unix"
            unix.mkdir(parents=True)
            for name in ("ntdll", "win32u", "winex11", "winewayland", "opengl32"):
                (unix / (name + ".so")).write_bytes(b"verified Wine module")
            missing = {"winewayland.so"}
            def ldd(argv, **kwargs):
                self.assertEqual(argv[0], "ldd")
                self.assertTrue(kwargs["env"]["LD_LIBRARY_PATH"].startswith(str(unix)))
                return mock.Mock(returncode=0, stdout="liboptional.so => not found\n"
                                 if Path(argv[1]).name in missing else "libc.so => /lib/libc.so\n", stderr="")
            with mock.patch.object(smoke.subprocess, "run", side_effect=ldd), \
                 mock.patch.object(smoke.ctypes, "CDLL", side_effect=OSError("optional library absent")):
                result = smoke.collect_wine_dependencies(wine, root, {})
                self.assertEqual(result["required_failures"], [])
                self.assertTrue(result["modules"]["winewayland"]["unresolved"])
                self.assertFalse(result["optional_x11_dlopen"]["libxkbregistry.so.0"]["available"])
                missing.add("winex11.so")
                with self.assertRaisesRegex(RuntimeError, "winex11"):
                    smoke.collect_wine_dependencies(wine, root, {})
                written = json.loads((root / "wine-dependencies.json").read_text())
                self.assertEqual(written["required_failures"], ["winex11"])

    def test_gui_probe_requires_notepad_and_disables_rml_in_same_prefix(self):
        self.assertEqual(smoke.probe_windows(' 0x1 "RMLProbe - Wine desktop": () 640x480+0+0')[0]["kind"], "desktop")
        trees = [
            ' 0x1 "RMLProbe - Wine Desktop": ("explorer.exe" "Wine") 640x480+0+0\n',
            ' 0x1 "RMLProbe - Wine Desktop": ("explorer.exe" "Wine") 640x480+0+0\n'
            ' 0x2 "Untitled - Notepad": ("notepad.exe" "Wine") 600x400+0+0\n',
        ]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            prefix = root / "prefix"
            prefix.mkdir()
            for index, tree in enumerate(trees):
                with self.subTest(notepad=index == 1):
                    destination = root / str(index)
                    destination.mkdir()
                    clock = [0]
                    env = {"WINEPREFIX": str(prefix), "DISPLAY": ":99", "WINEDLLOVERRIDES": "dwmapi=n,b"}
                    process = mock.Mock()
                    process.poll.return_value = None
                    def text(argv, environment):
                        return tree if "-root" in argv else "Map State: IsViewable"
                    with mock.patch.object(smoke.subprocess, "Popen", return_value=process) as launch, \
                         mock.patch.object(smoke, "text_command", side_effect=text), \
                         mock.patch.object(smoke, "wine_processes", return_value=[42]), \
                         mock.patch.object(smoke, "command") as command, \
                         mock.patch.object(smoke.time, "monotonic", side_effect=lambda: clock[0]), \
                         mock.patch.object(smoke.time, "sleep", side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)):
                        if index:
                            self.assertEqual(smoke.wine_gui_probe(root / "bin/wine", prefix, env, destination)["status"], "passed")
                        else:
                            with self.assertRaisesRegex(RuntimeError, "Wine backend GUI probe failed"):
                                smoke.wine_gui_probe(root / "bin/wine", prefix, env, destination)
                        self.assertEqual(launch.call_args.kwargs["cwd"], prefix)
                        probe_env = launch.call_args.kwargs["env"]
                        self.assertEqual(probe_env["WINEPREFIX"], str(prefix))
                        self.assertIn("dwmapi=b;", probe_env["WINEDLLOVERRIDES"])
                        self.assertNotIn("dwmapi=n", probe_env["WINEDLLOVERRIDES"])
                        cleanup = [call.args for call in command.call_args_list if "wineserver" in str(call.args[0][0])]
                        self.assertEqual({call[0][1] for call in cleanup}, {"-k", "-w"})
                    record = json.loads((destination / "wine-gui-probe.json").read_text())
                    self.assertEqual(record["status"], "passed" if index else "failed")
                    self.assertTrue((destination / record["window_history"]).is_file())

    def test_prefix_setup_runs_x64_webview_without_vc_bootstrap(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            installer = work / "webview2.exe"
            installer.write_bytes(runtime_pe())
            prefix = work / "prefix"
            metadata = {"webview": {"version": "144.0.3719.92"}}
            installed = prefix / "drive_c/Program Files (x86)/Microsoft/EdgeWebView/Application/144.0.3719.92"
            with mock.patch.object(smoke, "command") as command:
                with self.assertRaisesRegex(RuntimeError, "installation is missing"):
                    smoke.setup_prefix(work / "wine", work, prefix, metadata, {}, work / "setup.log")
                command.reset_mock()
                installed.mkdir(parents=True)
                smoke.setup_prefix(work / "wine", work, prefix, metadata, {}, work / "setup.log")
                commands = [call.args[0] for call in command.call_args_list]
                self.assertEqual(commands[0][1], "wineboot")
                self.assertEqual(commands[-1][1], installer)
                self.assertIn("--msedgewebview", commands[-1])
                self.assertFalse(any("vc_redist" in str(arg).lower() for cmd in commands for arg in cmd))
                command.reset_mock()
                installer.write_bytes(runtime_pe(machine=0x14C))
                with self.assertRaises(ValueError):
                    smoke.setup_prefix(work / "wine", work, prefix, metadata, {}, work / "setup.log")
                command.assert_not_called()

    def test_both_native_markers_required(self):
        status = smoke.loader_status("Hooking enabled.")
        self.assertFalse(all(status["markers"].values()))
        self.assertTrue(all(smoke.loader_status("Hooking enabled.\nQt action hook installed.")["markers"].values()))

    def test_markers_do_not_hide_hook_or_bootstrap_failure(self):
        for error in ("[error] Failed to initialize subsystem Pointers", "[warning] Failed to get vtable for job kind0",
                      "[RML] Fatal error during bootstrap", "Unhandled page fault", "Failed to create the bootstrap thread"):
            with self.subTest(error=error):
                self.assertTrue(smoke.loader_status("Hooking enabled.\nQt action hook installed.\n" + error)["errors"])

    def test_wine_noise_is_not_a_native_bootstrap_error(self):
        self.assertFalse(smoke.loader_status("fixme:seh ignored\nerr:ole routine returned E_NOINTERFACE")["errors"])

    def test_virtual_desktop_and_splash_alone_do_not_count(self):
        tree = (' 0x1 "RMLSmoke": ("explorer.exe" "Wine") 1280x720+0+0\n'
                ' 0x2 "Roblox Studio Splash": ("robloxstudiobeta.exe" "Wine") 640x400+0+0\n'
                ' 0x3 "Roblox Studio": ("robloxstudiobeta.exe" "Wine") 100x100+0+0\n')
        self.assertEqual(smoke.studio_windows(tree), [])

    def test_actual_studio_or_login_window_counts(self):
        tree = ' 0x40007 "Roblox Studio": ("robloxstudiobeta.exe" "Wine") 1000x600+0+0\n'
        self.assertEqual(smoke.studio_windows(tree)[0]["handle"], "0x40007")
        self.assertTrue(smoke.studio_windows(tree.replace("Roblox Studio", "Log in to Roblox Studio")))

    def test_titled_gpu_error_is_not_a_login_window(self):
        status = smoke.window_text_status("Roblox Studio Graphics failed to initialize. Please log in again.")
        self.assertTrue(status["fatal_dialog_text"])
        status = smoke.window_text_status("Roblox Studio Log In Username Password Sign up")
        self.assertFalse(status["fatal_dialog_text"])
        self.assertTrue(status["recognizable_studio_or_login_content"])
        self.assertFalse(smoke.window_text_status("Roblox Studio") ["recognizable_studio_or_login_content"])


if __name__ == "__main__":
    unittest.main()
