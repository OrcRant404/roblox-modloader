#!/usr/bin/env python3
"""Use the real packaged DLLs in a temporary filesystem install; never start Studio."""

from contextlib import redirect_stdout
import io
from pathlib import Path
import sys
import tarfile
import tempfile

from test_installer import fake_pe
import install
import payload


with tempfile.TemporaryDirectory(prefix="rml-real-payload-test-") as temporary:
    root = Path(temporary).resolve()
    with tarfile.open(sys.argv[1]) as archive:
        # This archive was made by build_bundle.py in the same job. Still forbid links/traversal.
        for member in archive.getmembers():
            payload.safe_name(member.name)
            if not (member.isfile() or member.isdir()):
                raise ValueError("Unexpected tar member: " + member.name)
        archive.extractall(root)
    bundle = root / "roblox-modloader-linux-vinegar"
    count = len(payload.validate_payload(bundle / "payload"))
    studio = root / "synthetic Studio"
    fake_pe(studio / "RobloxStudioBeta.exe", dll=False)
    config = studio / "RobloxModLoader/config.toml"
    config.parent.mkdir()
    config.write_text("user configuration sentinel")
    with redirect_stdout(io.StringIO()):
        install.install(install.studio_directory(studio), bundle, dry_run=True)
        receipt = install.install(studio, bundle)
        for name in payload.OWNED:
            assert payload.tree_digest(studio / name) == payload.tree_digest(bundle / "payload" / name)
        install.rollback(studio, receipt)
    assert config.read_text() == "user configuration sentinel"
    assert not (studio / "dwmapi.dll").exists()
    print("PASS: %d real payload files validated, dry-run/install/rollback passed on a synthetic Studio directory." % count)
    print("This does not execute DLLs, Wine, Vinegar, Roblox Studio, or mods.")
