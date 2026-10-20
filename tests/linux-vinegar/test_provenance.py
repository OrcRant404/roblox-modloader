import argparse
from contextlib import redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.request
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts/linux-vinegar"))
import fetch_artifact


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.args = argparse.Namespace(repository="owner/repo", run_id="123", source_sha="a" * 40,
                                       output=Path(self.temporary.name) / "verified")
        self.run = {"head_sha": "a" * 40, "conclusion": "success", "status": "completed",
                    "path": ".github/workflows/ci.yml", "head_repository": {"full_name": "owner/repo"},
                    "html_url": "https://github.com/owner/repo/actions/runs/123"}
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w") as archive:
            archive.writestr("package-windows.zip", b"Windows package fixture")
        self.download = stream.getvalue()
        self.artifact = {"name": "package-windows", "expired": False, "id": 456,
                         "digest": "sha256:" + hashlib.sha256(self.download).hexdigest()}

    def request(self, request, timeout):
        url = request.full_url
        if url.endswith("/actions/runs/123"):
            return io.BytesIO(json.dumps(self.run).encode())
        if url.endswith("/artifacts?per_page=100"):
            return io.BytesIO(json.dumps({"artifacts": [self.artifact]}).encode())
        if url.endswith("/actions/artifacts/456/zip"):
            return io.BytesIO(self.download)
        self.fail("Unexpected API URL: " + url)

    def fetch(self):
        with patch.dict(os.environ, {"GH_TOKEN": "test-token"}), \
             patch.object(fetch_artifact.urllib.request.OpenerDirector, "open", side_effect=self.request), \
             redirect_stdout(io.StringIO()):
            fetch_artifact.fetch(self.args)

    def test_exact_source_and_digest_preserved(self):
        self.fetch()
        self.assertEqual((self.args.output / "package-windows.zip").read_bytes(), b"Windows package fixture")
        metadata = json.loads((self.args.output / "provenance.json").read_text())
        self.assertEqual(metadata["artifact_digest"], self.artifact["digest"])
        self.assertEqual(metadata["artifact_id"], 456)
        self.assertEqual(metadata["source_sha"], "a" * 40)

    def test_wrong_commit_repo_workflow_or_failed_run_rejected(self):
        changes = (("head_sha", "b" * 40), ("head_repository", {"full_name": "other/repo"}),
                   ("path", ".github/workflows/other.yml"), ("conclusion", "failure"))
        for key, value in changes:
            with self.subTest(key=key):
                original = self.run[key]
                self.run[key] = value
                with self.assertRaisesRegex(ValueError, "successful same-repository"):
                    self.fetch()
                self.assertFalse(self.args.output.exists())
                self.run[key] = original

    def test_artifact_digest_mismatch_rejected_before_output(self):
        self.download += b"modified"
        with self.assertRaisesRegex(ValueError, "SHA256 digest"):
            self.fetch()
        self.assertFalse(self.args.output.exists())

    def test_expired_artifact_rejected(self):
        self.artifact["expired"] = True
        with self.assertRaisesRegex(ValueError, "exactly one live"):
            self.fetch()

    def test_github_token_not_forwarded_to_artifact_storage(self):
        request = urllib.request.Request("https://api.github.com/repos/owner/repo/actions/artifacts/456/zip",
                                         headers={"Authorization": "Bearer test-token"})
        redirected = fetch_artifact.ArtifactRedirect().redirect_request(
            request, None, 302, "Found", {}, "https://example.blob.core.windows.net/signed-artifact")
        self.assertFalse(redirected.has_header("Authorization"))


if __name__ == "__main__":
    unittest.main()
