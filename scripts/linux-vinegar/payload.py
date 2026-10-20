"""Shared, standard-library-only checks for the Windows payload used under Wine."""

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import struct


OWNED = ("dwmapi.dll", "roblox_modloader.dll", "RobloxModLoader/runtime")
NATIVE = ("dwmapi.dll", "roblox_modloader.dll", "RobloxModLoader/runtime/nethost.dll")
MANAGED = tuple("RobloxModLoader/runtime/" + name + ".dll" for name in (
    "RML.Core", "RML.Interop", "RML.Logging", "RML.NativeHost", "Roblox"))
CONFIGS = ("RobloxModLoader/runtime/RML.runtimeconfig.json",
           "RobloxModLoader/runtime/RobloxModLoader.runtimeconfig.json")
DEPS = ("RobloxModLoader/runtime/RML.Core.deps.json",
        "RobloxModLoader/runtime/RML.NativeHost.deps.json")


class InvalidPayload(ValueError):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def safe_name(name):
    path = PurePosixPath(name)
    if (not name or "\\" in name or ":" in name or path.is_absolute()
            or any(part in ("", ".", "..") for part in name.split("/"))):
        raise InvalidPayload("Unsafe relative path: " + name)
    return path


def check_tree(path):
    """Reject links/special files, including broken links, before reading or copying."""
    mode = path.lstat().st_mode
    if stat.S_ISREG(mode):
        return
    if not stat.S_ISDIR(mode):
        raise InvalidPayload("Link or special file is not allowed: " + str(path))
    for directory, dirs, files in os.walk(path, followlinks=False):
        for name in dirs + files:
            child = Path(directory) / name
            child_mode = child.lstat().st_mode
            if not (stat.S_ISREG(child_mode) or stat.S_ISDIR(child_mode)):
                raise InvalidPayload("Link or special file is not allowed: " + str(child))


def pe_info(path):
    """Read actual DOS/COFF/optional headers; extensions alone are insufficient."""
    try:
        data = path.read_bytes()
        if len(data) < 64 or data[:2] != b"MZ":
            raise ValueError("missing DOS header")
        offset = struct.unpack_from("<I", data, 60)[0]
        if offset < 64 or data[offset:offset + 4] != b"PE\0\0":
            raise ValueError("missing PE signature")
        machine, sections = struct.unpack_from("<HH", data, offset + 4)
        size, characteristics = struct.unpack_from("<HH", data, offset + 20)
        optional = offset + 24
        magic = struct.unpack_from("<H", data, optional)[0]
        minimum = {0x10B: 224, 0x20B: 240}.get(magic)
        if (not minimum or size < minimum or not sections
                or optional + size + sections * 40 > len(data)):
            raise ValueError("truncated or invalid PE headers")
        directory_offset = 112 if magic == 0x20B else 96
        clr_rva, clr_size = struct.unpack_from("<II", data, optional + directory_offset + 14 * 8)
        return machine, magic, bool(characteristics & 0x2000), bool(clr_rva and clr_size)
    except (ValueError, struct.error) as exc:
        raise InvalidPayload("Invalid PE file %s: %s" % (path, exc)) from exc


def validate_payload(root):
    if not root.is_dir():
        raise InvalidPayload("Missing payload directory: " + str(root))
    check_tree(root)
    files = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}
    required = set(NATIVE + MANAGED + CONFIGS + DEPS)
    missing = required - files
    if missing:
        raise InvalidPayload("Missing payload files: " + ", ".join(sorted(missing)))
    for name in files:
        safe_name(name)
        if name not in NATIVE[:2] and not name.startswith("RobloxModLoader/runtime/"):
            raise InvalidPayload("Unexpected payload file: " + name)
        if name.lower().endswith((".exe", ".so", ".dylib")) or "/dotnet/" in name:
            raise InvalidPayload("Unexpected executable/runtime payload: " + name)
    for name in NATIVE:
        machine, magic, dll, managed = pe_info(root / name)
        if (machine, magic, dll, managed) != (0x8664, 0x20B, True, False):
            raise InvalidPayload("Expected a native PE32+ AMD64 DLL: " + name)
    for name in MANAGED:
        machine, magic, dll, managed = pe_info(root / name)
        if not managed or not dll or machine not in (0x14C, 0x8664):
            raise InvalidPayload("Expected a managed PE DLL: " + name)
    for name in CONFIGS:
        try:
            options = json.loads((root / name).read_text(encoding="utf-8-sig"))["runtimeOptions"]
            framework = options["framework"]
            if (options["tfm"] != "net10.0" or framework["name"] != "Microsoft.NETCore.App"
                    or not framework["version"].startswith("10.0.")):
                raise ValueError("expected net10.0 Microsoft.NETCore.App 10.0.x")
        except (KeyError, TypeError, ValueError) as exc:
            raise InvalidPayload("Invalid runtime config %s: %s" % (name, exc)) from exc
    for name in files:
        if name.endswith(".deps.json"):
            try:
                dependencies = json.loads((root / name).read_text(encoding="utf-8-sig"))
                if (not isinstance(dependencies["targets"], dict)
                        or not isinstance(dependencies["libraries"], dict)
                        or dependencies["runtimeTarget"]["name"] != ".NETCoreApp,Version=v10.0"):
                    raise ValueError("expected .NET 10 dependency targets and libraries")
            except (ValueError, KeyError, TypeError) as exc:
                raise InvalidPayload("Invalid dependency JSON: " + name) from exc
    return {name: sha256(root / name) for name in sorted(files)}


def tree_digest(path):
    check_tree(path)
    if path.is_file():
        return {"kind": "file", "sha256": sha256(path)}
    return {"kind": "directory", "files": {
        p.relative_to(path).as_posix(): sha256(p)
        for p in sorted(path.rglob("*")) if p.is_file()}}
