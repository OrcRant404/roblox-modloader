import argparse
from contextlib import redirect_stdout, redirect_stderr
import io
import json
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile


REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "scripts/linux-vinegar"
sys.path.insert(0, str(SCRIPTS))
import payload
import install
import build_bundle


def fake_pe(path, *, machine=0x8664, managed=False, dll=True):
    """A structural PE fixture, deliberately not a runnable Windows program."""
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    magic, size, directory_offset = (0x10B, 224, 96) if managed else (0x20B, 240, 112)
    struct.pack_into("<HH", data, 68, machine, 1)
    struct.pack_into("<HH", data, 84, size, 0x2000 if dll else 0x2)
    struct.pack_into("<H", data, 88, magic)
    if managed:
        struct.pack_into("<II", data, 88 + directory_offset + 14 * 8, 0x1000, 72)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def fixture_bundle(root):
    target = root / "payload"
    for name in payload.NATIVE:
        fake_pe(target / name)
    for name in payload.MANAGED:
        fake_pe(target / name, machine=0x14C, managed=True)
    config = {"runtimeOptions": {"tfm": "net10.0", "framework": {
        "name": "Microsoft.NETCore.App", "version": "10.0.12"}}}
    for name in payload.CONFIGS:
        (target / name).write_text(json.dumps(config))
    for name in payload.DEPS:
        (target / name).write_text(json.dumps({"runtimeTarget": {"name": ".NETCoreApp,Version=v10.0"},
                                              "targets": {}, "libraries": {}}))
    manifest = {"schema": 1, "platform": "windows-x64-under-vinegar-wine",
                "source_sha": "a" * 40, "files": payload.validate_payload(target)}
    (root / "manifest.json").write_text(json.dumps(manifest))
    return root


def snapshot(root):
    return {p.relative_to(root).as_posix(): p.read_bytes()
            for p in root.rglob("*") if p.is_file() and not p.is_symlink()}


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.bundle = fixture_bundle(self.root / "bundle")
        self.studio = self.root / "Studio with spaces"
        fake_pe(self.studio / "RobloxStudioBeta.exe", dll=False)
        self.log = io.StringIO()
        self.stdout = redirect_stdout(self.log)
        self.stderr = redirect_stderr(self.log)
        self.stdout.__enter__()
        self.stderr.__enter__()
        self.addCleanup(self.stdout.__exit__, None, None, None)
        self.addCleanup(self.stderr.__exit__, None, None, None)

    def old_loader(self):
        for name in payload.OWNED[:2]:
            (self.studio / name).write_bytes(b"old " + name.encode())
        runtime = self.studio / payload.OWNED[2]
        runtime.mkdir(parents=True)
        (runtime / "old.dll").write_bytes(b"prior runtime")
        mods = self.studio / "RobloxModLoader/mods/user-mod/native"
        mods.mkdir(parents=True)
        (mods / "user.dll").write_bytes(b"user mod")
        (self.studio / "RobloxModLoader/config.toml").write_bytes(b"user config")
        (self.studio / "other-file").write_bytes(b"unrelated")

    def test_dry_run_creates_nothing(self):
        self.old_loader()
        before = snapshot(self.studio)
        directories = sorted(p.relative_to(self.studio).as_posix() for p in self.studio.rglob("*"))
        install.install(install.studio_directory(self.studio), self.bundle, dry_run=True)
        self.assertEqual(snapshot(self.studio), before)
        self.assertEqual(sorted(p.relative_to(self.studio).as_posix() for p in self.studio.rglob("*")), directories)

    def test_install_backups_rollback_and_userdata(self):
        self.old_loader()
        before = snapshot(self.studio)
        receipt = install.install(install.studio_directory(self.studio), self.bundle)
        for name in payload.OWNED[:2]:
            self.assertEqual((self.studio / name).read_bytes(), (self.bundle / "payload" / name).read_bytes())
            self.assertEqual((receipt.parent / "original" / name).read_bytes(), before[name])
        self.assertFalse((self.studio / payload.OWNED[2] / "old.dll").exists())
        for name in ("RobloxModLoader/mods/user-mod/native/user.dll", "RobloxModLoader/config.toml", "other-file"):
            self.assertEqual((self.studio / name).read_bytes(), before[name])
        install.rollback(self.studio, receipt)
        after = {name: value for name, value in snapshot(self.studio).items()
                 if not name.startswith(".rml-linux-vinegar-backups/")}
        self.assertEqual(after, before)
        self.assertEqual(json.loads(receipt.read_text())["state"], "rolled-back")

    def test_clean_install_rollback_removes_only_owned_files(self):
        before = snapshot(self.studio)
        receipt = install.install(install.studio_directory(self.studio), self.bundle)
        config = self.studio / "RobloxModLoader/config.toml"
        config.write_text("created by loader/user later")
        install.rollback(self.studio, receipt)
        self.assertEqual(config.read_text(), "created by loader/user later")
        for name in payload.OWNED:
            self.assertFalse((self.studio / name).exists())
        self.assertEqual((self.studio / "RobloxStudioBeta.exe").read_bytes(), before["RobloxStudioBeta.exe"])

    def test_failed_copy_automatically_recovers_prior_loader(self):
        self.old_loader()
        before = snapshot(self.studio)
        copy = install.copy_path
        def failing_copy(source, target):
            if source == self.bundle / "payload/RobloxModLoader/runtime":
                target.mkdir()
                (target / "partial").write_bytes(b"incomplete")
                raise OSError("injected disk/copy failure")
            copy(source, target)
        with patch.object(install, "copy_path", side_effect=failing_copy):
            with self.assertRaisesRegex(OSError, "injected"):
                install.install(install.studio_directory(self.studio), self.bundle)
        after = {name: value for name, value in snapshot(self.studio).items()
                 if not name.startswith(".rml-linux-vinegar-backups/")}
        self.assertEqual(after, before)

    def test_rollback_refuses_changed_install(self):
        receipt = install.install(install.studio_directory(self.studio), self.bundle)
        (self.studio / "dwmapi.dll").write_bytes(b"user changed this after install")
        before = snapshot(self.studio)
        with self.assertRaisesRegex(payload.InvalidPayload, "Installed path changed"):
            install.rollback(self.studio, receipt)
        self.assertEqual(snapshot(self.studio), before)

    def test_rollback_refuses_changed_backup(self):
        self.old_loader()
        receipt = install.install(install.studio_directory(self.studio), self.bundle)
        (receipt.parent / "original/dwmapi.dll").write_bytes(b"tampered")
        before = snapshot(self.studio)
        with self.assertRaisesRegex(payload.InvalidPayload, "Backup is missing or changed"):
            install.rollback(self.studio, receipt)
        self.assertEqual(snapshot(self.studio), before)

    def test_rollback_dry_run_changes_nothing(self):
        receipt = install.install(install.studio_directory(self.studio), self.bundle)
        before = snapshot(self.studio)
        install.rollback(self.studio, receipt, dry_run=True)
        self.assertEqual(snapshot(self.studio), before)

    def test_wrong_studio_fails_closed(self):
        for change in ("missing", "wrong_arch", "dll"):
            with self.subTest(change=change):
                exe = self.studio / "RobloxStudioBeta.exe"
                if change == "missing":
                    exe.unlink()
                else:
                    fake_pe(exe, machine=0xAA64 if change == "wrong_arch" else 0x8664,
                            dll=change == "dll")
                with self.assertRaises(payload.InvalidPayload):
                    install.studio_directory(self.studio)
                self.assertFalse((self.studio / ".rml-linux-vinegar-backups").exists())

    def test_symlink_targets_and_studio_fail_closed(self):
        external = self.root / "external"
        external.mkdir()
        (external / "keep").write_text("safe")
        for name in payload.OWNED:
            target = self.studio / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(external, target_is_directory=True)
            with self.assertRaises(payload.InvalidPayload):
                install.studio_directory(self.studio)
            target.unlink()
        link = self.root / "studio-link"
        link.symlink_to(self.studio, target_is_directory=True)
        with self.assertRaises(payload.InvalidPayload):
            install.studio_directory(link)
        self.assertEqual((external / "keep").read_text(), "safe")

    def test_manifest_tampering_fails_before_backups(self):
        (self.bundle / "payload/dwmapi.dll").write_bytes(b"wrong")
        before = snapshot(self.studio)
        with self.assertRaises(payload.InvalidPayload):
            install.install(install.studio_directory(self.studio), self.bundle)
        self.assertEqual(snapshot(self.studio), before)

    def test_cli_requires_closed_studio_confirmation(self):
        result = subprocess.run([sys.executable, str(SCRIPTS / "install.py"),
                                 "--studio-dir", str(self.studio)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("--studio-closed", result.stderr)

    def test_running_process_guard(self):
        with patch.object(install, "running_studio", return_value=["1234"]), \
             patch.object(sys, "argv", ["install.py", "--studio-dir", str(self.studio), "--studio-closed"]):
            with self.assertRaises(SystemExit) as result:
                install.main()
            self.assertEqual(result.exception.code, 1)
        self.assertIn("still running", self.log.getvalue())


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.bundle = fixture_bundle(self.root / "fixture")

    def test_wrong_native_architecture_rejected(self):
        fake_pe(self.bundle / "payload/dwmapi.dll", machine=0xAA64)
        with self.assertRaisesRegex(payload.InvalidPayload, "AMD64"):
            payload.validate_payload(self.bundle / "payload")

    def test_renamed_non_pe_rejected(self):
        (self.bundle / "payload/roblox_modloader.dll").write_bytes(b"not a DLL")
        with self.assertRaisesRegex(payload.InvalidPayload, "DOS header"):
            payload.validate_payload(self.bundle / "payload")

    def test_missing_essential_assembly_rejected(self):
        (self.bundle / "payload/RobloxModLoader/runtime/RML.Core.dll").unlink()
        with self.assertRaisesRegex(payload.InvalidPayload, "Missing payload"):
            payload.validate_payload(self.bundle / "payload")

    def test_missing_dependency_manifest_rejected(self):
        (self.bundle / "payload" / payload.DEPS[0]).unlink()
        with self.assertRaisesRegex(payload.InvalidPayload, "Missing payload"):
            payload.validate_payload(self.bundle / "payload")

    def test_wrong_runtime_config_rejected(self):
        (self.bundle / "payload" / payload.CONFIGS[0]).write_text('{"runtimeOptions":{}}')
        with self.assertRaisesRegex(payload.InvalidPayload, "runtime config"):
            payload.validate_payload(self.bundle / "payload")

    def test_roblox_binary_and_native_linux_payload_rejected(self):
        for name in ("RobloxStudioBeta.exe", "RobloxModLoader/runtime/libnethost.so"):
            path = self.bundle / "payload" / name
            path.write_bytes(b"forbidden")
            with self.assertRaises(payload.InvalidPayload):
                payload.validate_payload(self.bundle / "payload")
            path.unlink()

    def make_zip(self, extra=None):
        archive = self.root / "windows.zip"
        with zipfile.ZipFile(archive, "w") as zipped:
            for path in (self.bundle / "payload").rglob("*"):
                if path.is_file():
                    zipped.write(path, path.relative_to(self.bundle / "payload").as_posix())
            zipped.writestr("RobloxModLoader/config.toml", "default config")
            zipped.writestr("RobloxModLoader/mods/", "")
            if extra:
                zipped.writestr(*extra)
        return archive

    def args(self, archive):
        return argparse.Namespace(windows_package=archive, source_sha="a" * 40,
            source_run_url="https://github.com/test/repo/actions/runs/123", provenance=None,
            output=self.root / "bundle.tar.gz")

    def test_bundle_contains_notices_manifest_and_only_loader_payload(self):
        args = self.args(self.make_zip())
        with redirect_stdout(io.StringIO()):
            build_bundle.build(args)
        with tarfile.open(args.output) as archive:
            names = archive.getnames()
            self.assertIn("roblox-modloader-linux-vinegar/LICENSE", names)
            self.assertIn("roblox-modloader-linux-vinegar/THIRD-PARTY-NOTICES.md", names)
            self.assertIn("roblox-modloader-linux-vinegar/manifest.json", names)
            self.assertFalse(any(name.endswith("config.toml") for name in names))
            self.assertFalse(any("RobloxStudioBeta.exe" in name for name in names))
        self.assertTrue(args.output.with_name(args.output.name + ".sha256").is_file())

    def test_zip_traversal_rejected(self):
        with self.assertRaisesRegex(payload.InvalidPayload, "Unsafe relative path"):
            build_bundle.build(self.args(self.make_zip(("../outside", "bad"))))
        self.assertFalse((self.root / "outside").exists())

    def test_zip_symlink_rejected(self):
        archive = self.make_zip()
        info = zipfile.ZipInfo("RobloxModLoader/runtime/link")
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(archive, "a") as zipped:
            zipped.writestr(info, "../../outside")
        with self.assertRaisesRegex(payload.InvalidPayload, "links/special"):
            build_bundle.build(self.args(archive))

    def test_zip_case_collision_rejected(self):
        with self.assertRaisesRegex(payload.InvalidPayload, "case-colliding"):
            build_bundle.build(self.args(self.make_zip(("DWMAPI.DLL", "bad"))))

    def test_unexpected_windows_package_file_rejected(self):
        with self.assertRaisesRegex(payload.InvalidPayload, "refusing redistribution"):
            build_bundle.build(self.args(self.make_zip(("RobloxStudioBeta.exe", "bad"))))


if __name__ == "__main__":
    unittest.main()
