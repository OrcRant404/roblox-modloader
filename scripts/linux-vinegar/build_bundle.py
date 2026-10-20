#!/usr/bin/env python3
"""Repackage a validated Windows CI archive for explicit Vinegar installation."""

import argparse
import json
from pathlib import Path
import re
import shutil
import stat
import tarfile
import tempfile
import zipfile

from payload import InvalidPayload, safe_name, sha256, validate_payload


def build(args):
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_sha):
        raise InvalidPayload("--source-sha must be the exact 40-character Git commit")
    if not re.fullmatch(r"https://github\.com/[\w.-]+/[\w.-]+/actions/runs/[0-9]+", args.source_run_url):
        raise InvalidPayload("--source-run-url must identify the Windows build's GitHub Actions run")
    if args.output.exists():
        raise InvalidPayload("Output already exists: " + str(args.output))
    script_dir = Path(__file__).resolve().parent
    repo = script_dir.parents[1]
    with tempfile.TemporaryDirectory(prefix="rml-bundle-") as temporary:
        bundle = Path(temporary) / "roblox-modloader-linux-vinegar"
        payload = bundle / "payload"
        payload.mkdir(parents=True)
        seen = set()
        with zipfile.ZipFile(args.windows_package) as source:
            for member in source.infolist():
                name = member.filename.rstrip("/")
                safe_name(name)
                if name.casefold() in seen:
                    raise InvalidPayload("Duplicate/case-colliding ZIP member: " + name)
                seen.add(name.casefold())
                mode = member.external_attr >> 16
                if stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR):
                    raise InvalidPayload("ZIP links/special files are forbidden: " + name)
                selected = (name in ("dwmapi.dll", "roblox_modloader.dll")
                            or name.startswith("RobloxModLoader/runtime/"))
                if not member.is_dir() and selected:
                    destination = payload / name
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with source.open(member) as reader, destination.open("xb") as writer:
                        shutil.copyfileobj(reader, writer)
                elif not member.is_dir() and name not in (
                        "RobloxModLoader/config.toml", "roblox_modloader.pdb", "LICENSE",
                        "RobloxModLoader/libraries/bridge.luau", "RobloxModLoader/libraries/logger.luau"):
                    raise InvalidPayload("Unexpected input file (refusing redistribution): " + name)
        files = validate_payload(payload)
        manifest = {
            "schema": 1, "platform": "windows-x64-under-vinegar-wine",
            "source_sha": args.source_sha, "source_run_url": args.source_run_url,
            "windows_package_sha256": sha256(args.windows_package), "files": files,
            "support": "experimental; filesystem/PE tests do not establish live Studio compatibility",
            "requires": "Windows x64 Microsoft.NETCore.App 10.0.x in the same Wine prefix",
        }
        if args.provenance:
            provenance = json.loads(args.provenance.read_text())
            if (provenance["source_sha"] != args.source_sha
                    or provenance["source_run_url"] != args.source_run_url
                    or provenance["windows_package_sha256"] != manifest["windows_package_sha256"]):
                raise InvalidPayload("Artifact provenance does not match the Windows input")
            manifest["artifact_provenance"] = provenance
        for name in ("install.py", "payload.py"):
            shutil.copyfile(script_dir / name, bundle / name)
        shutil.copyfile(repo / "LICENSE", bundle / "LICENSE")
        shutil.copyfile(repo / "docs/linux-vinegar.md", bundle / "README.md")
        shutil.copytree(script_dir / "licenses", bundle / "licenses")
        shutil.copyfile(script_dir / "THIRD-PARTY-NOTICES.md", bundle / "THIRD-PARTY-NOTICES.md")
        (bundle / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(args.output, "w:gz") as archive:
            archive.add(bundle, arcname=bundle.name, filter=normalize)
    digest = sha256(args.output)
    args.output.with_name(args.output.name + ".sha256").write_text(digest + "  " + args.output.name + "\n")
    print("Validated %d payload files; created %s\nSHA256 %s" % (len(files), args.output, digest))


def normalize(info):
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    info.mode = 0o755 if info.isdir() else 0o644
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--windows-package", required=True, type=Path)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--source-run-url", required=True)
    parser.add_argument("--provenance", type=Path, help="verified artifact metadata from fetch_artifact.py")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        build(args)
    except (InvalidPayload, OSError, ValueError, zipfile.BadZipFile) as exc:
        parser.exit(1, "Error: %s\n" % exc)


if __name__ == "__main__":
    main()
