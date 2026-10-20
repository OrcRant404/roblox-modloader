#!/usr/bin/env python3
"""Install or roll back only the three RML loader paths in an explicit Studio directory."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import sys
import uuid

from payload import InvalidPayload, OWNED, check_tree, pe_info, tree_digest, validate_payload


def no_link_ancestors(path):
    for part in (path, *path.parents):
        if part.is_symlink():
            raise InvalidPayload("Symlink path is not allowed: " + str(part))


def studio_directory(value):
    raw = Path(os.path.abspath(value))
    no_link_ancestors(raw)
    if not raw.is_dir() or raw == Path(raw.anchor):
        raise InvalidPayload("--studio-dir must be an existing Studio version directory")
    exe = raw / "RobloxStudioBeta.exe"
    if not exe.is_file() or exe.is_symlink():
        raise InvalidPayload("Missing regular RobloxStudioBeta.exe in --studio-dir")
    machine, magic, dll, _ = pe_info(exe)
    if (machine, magic, dll) != (0x8664, 0x20B, False):
        raise InvalidPayload("Studio must be a Windows PE32+ AMD64 executable")
    rml = raw / "RobloxModLoader"
    if rml.is_symlink() or (rml.exists() and not rml.is_dir()):
        raise InvalidPayload("RobloxModLoader must be a regular directory")
    for name in OWNED:
        target = raw / name
        if target.exists() or target.is_symlink():
            check_tree(target)
            if target.is_dir() != name.endswith("/runtime"):
                raise InvalidPayload("Wrong existing loader path type: " + str(target))
    return raw


def running_studio():
    """Best-effort Linux guard; explicit --studio-closed is still required."""
    proc = Path("/proc")
    if not proc.is_dir():
        return []
    found = []
    for entry in proc.iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            command = (entry / "cmdline").read_bytes().split(b"\0")
            if any(arg.replace(b"\\", b"/").rsplit(b"/", 1)[-1].lower()
                   == b"robloxstudiobeta.exe" for arg in command):
                found.append(entry.name)
        except (OSError, ProcessLookupError):
            continue
    return found


def write_receipt(path, receipt):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(receipt, indent=2) + "\n")
    temporary.replace(path)


def remove_owned(path):
    # Called only for an exact, preflight-checked OWNED path; never a prefix or Studio root.
    check_tree(path)
    if path.is_dir():
        shutil.rmtree(path)
    else:
        path.unlink()


def copy_path(source, target):
    if source.is_dir():
        shutil.copytree(source, target)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def rollback(studio, receipt_path, dry_run=False):
    no_link_ancestors(receipt_path)
    expected_parent = studio / ".rml-linux-vinegar-backups"
    if (receipt_path.name != "receipt.json" or receipt_path.parent.parent != expected_parent
            or not receipt_path.is_file()):
        raise InvalidPayload("Receipt must be inside this Studio's .rml-linux-vinegar-backups/<id>/")
    receipt = json.loads(receipt_path.read_text())
    if (receipt.get("schema") != 1 or receipt.get("studio_dir") != str(studio)
            or receipt.get("state") not in ("installed", "install-failed")):
        raise InvalidPayload("Receipt does not describe an installed/recoverable change in this Studio")
    entries = receipt.get("entries", [])
    if [entry.get("path") for entry in entries] != list(OWNED):
        raise InvalidPayload("Receipt does not contain the exact loader paths")
    for entry in entries:
        target = studio / entry["path"]
        backup = receipt_path.parent / "original" / entry["path"]
        if entry["before"] is not None:
            no_link_ancestors(backup)
            if not backup.exists() or tree_digest(backup) != entry["before"]:
                raise InvalidPayload("Backup is missing or changed: " + str(backup))
        if receipt["state"] == "installed":
            if not target.exists() or tree_digest(target) != entry["after"]:
                raise InvalidPayload("Installed path changed; preserve it manually before rollback: " + str(target))
    print("Rollback exact loader paths using " + str(receipt_path))
    if dry_run:
        return
    for entry in reversed(entries):
        if not entry["changed"]:
            continue
        target = studio / entry["path"]
        if target.exists():
            remove_owned(target)
        if entry["before"] is not None:
            copy_path(receipt_path.parent / "original" / entry["path"], target)
        entry["changed"] = False
        write_receipt(receipt_path, receipt)
    receipt["state"] = "rolled-back"
    write_receipt(receipt_path, receipt)
    print("Rolled back. Backups and receipt retained. Mods/config were not touched.")


def install(studio, bundle, dry_run=False):
    no_link_ancestors(bundle)
    actual = validate_payload(bundle / "payload")
    manifest = json.loads((bundle / "manifest.json").read_text())
    if (manifest.get("schema") != 1 or manifest.get("platform") != "windows-x64-under-vinegar-wine"
            or manifest.get("files") != actual):
        raise InvalidPayload("Payload differs from the release manifest")
    backup_root = studio / ".rml-linux-vinegar-backups"
    if backup_root.is_symlink() or (backup_root.exists() and not backup_root.is_dir()):
        raise InvalidPayload("Backup root must be a regular directory")
    entries = []
    for name in OWNED:
        target = studio / name
        entries.append({"path": name, "before": tree_digest(target) if target.exists() else None,
                        "after": tree_digest(bundle / "payload" / name), "changed": False})
        print(("Replace and back up: " if target.exists() else "Create: ") + str(target))
    print("Preserve all mods, config, and other Studio files. Windows .NET 10 x64 and dwmapi=n,b are required.")
    if dry_run:
        print("Dry run: no files or directories created.")
        return None
    change_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:12]
    backup_dir = backup_root / change_id
    backup_dir.mkdir(parents=True, mode=0o700)
    receipt_path = backup_dir / "receipt.json"
    receipt = {"schema": 1, "state": "preparing", "studio_dir": str(studio),
               "source_sha": manifest["source_sha"], "entries": entries}
    write_receipt(receipt_path, receipt)
    # Copy and verify ALL originals before changing any installed loader file.
    for entry in entries:
        if entry["before"] is not None:
            saved = backup_dir / "original" / entry["path"]
            saved.parent.mkdir(parents=True, exist_ok=True)
            copy_path(studio / entry["path"], saved)
            if tree_digest(saved) != entry["before"]:
                raise InvalidPayload("Backup verification failed: " + str(saved))
    receipt["state"] = "install-failed"
    write_receipt(receipt_path, receipt)
    try:
        for entry in entries:
            target = studio / entry["path"]
            # Record first so interrupted/failed copies have a bounded recovery path.
            entry["changed"] = True
            write_receipt(receipt_path, receipt)
            if target.exists():
                remove_owned(target)
            target.parent.mkdir(parents=True, exist_ok=True)
            copy_path(bundle / "payload" / entry["path"], target)
            if tree_digest(target) != entry["after"]:
                raise InvalidPayload("Installed file verification failed: " + str(target))
        receipt["state"] = "installed"
        write_receipt(receipt_path, receipt)
    except (Exception, KeyboardInterrupt):
        print("Installation interrupted. Recovery receipt: " + str(receipt_path), file=sys.stderr)
        rollback(studio, receipt_path)
        raise
    print("Installed. Keep this recovery receipt: " + str(receipt_path))
    return receipt_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--studio-dir", required=True, type=Path,
                        help="existing directory containing RobloxStudioBeta.exe; never guessed")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--studio-closed", action="store_true", help="confirm Studio and Vinegar are closed")
    parser.add_argument("--rollback", type=Path, metavar="RECEIPT")
    args = parser.parse_args()
    try:
        studio = studio_directory(args.studio_dir)
        if not args.dry_run:
            if not args.studio_closed:
                raise InvalidPayload("Close Studio/Vinegar, then pass --studio-closed (or inspect with --dry-run)")
            running = running_studio()
            if running:
                raise InvalidPayload("Studio is still running (PIDs %s); close it before installation" % ", ".join(running))
        if args.rollback:
            rollback(studio, Path(os.path.abspath(args.rollback)), args.dry_run)
        else:
            install(studio, Path(__file__).resolve().parent, args.dry_run)
    except (InvalidPayload, OSError, ValueError, KeyError) as exc:
        parser.exit(1, "Error: %s\n" % exc)


if __name__ == "__main__":
    main()
