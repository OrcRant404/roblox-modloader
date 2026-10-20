#!/usr/bin/env python3
"""Fetch one successful same-repository CI Windows artifact with an exact source SHA."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import urllib.parse
import urllib.request
import zipfile


class ArtifactRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, url):
        redirected = super().redirect_request(request, fp, code, message, headers, url)
        if urllib.parse.urlparse(url).netloc != urllib.parse.urlparse(request.full_url).netloc:
            redirected.remove_header("Authorization")
        return redirected


def fetch(args):
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", args.repository):
        raise ValueError("Invalid repository")
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_sha) or not args.run_id.isdecimal():
        raise ValueError("An exact 40-character source SHA and numeric run ID are required")
    token = os.environ.get("GH_TOKEN")
    if not token:
        raise ValueError("GH_TOKEN is required (actions:read only)")
    opener = urllib.request.build_opener(ArtifactRedirect())

    def api(suffix):
        request = urllib.request.Request("https://api.github.com/repos/" + args.repository + suffix,
                                         headers={"Authorization": "Bearer " + token,
                                                  "Accept": "application/vnd.github+json",
                                                  "X-GitHub-Api-Version": "2022-11-28"})
        with opener.open(request, timeout=60) as response:
            return response.read()

    run = json.loads(api("/actions/runs/" + args.run_id))
    if (run["head_sha"] != args.source_sha or run["conclusion"] != "success"
            or run["status"] != "completed" or run["path"] != ".github/workflows/ci.yml"
            or run["head_repository"]["full_name"].casefold() != args.repository.casefold()):
        raise ValueError("Run must be a successful same-repository ci.yml build of the exact source SHA")
    artifacts = json.loads(api("/actions/runs/" + args.run_id + "/artifacts?per_page=100"))["artifacts"]
    candidates = [a for a in artifacts if a["name"] == "package-windows" and not a["expired"]]
    if len(candidates) != 1:
        raise ValueError("Expected exactly one live package-windows artifact")
    artifact = candidates[0]
    expected_digest = artifact.get("digest", "")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", expected_digest):
        raise ValueError("GitHub artifact SHA256 digest is missing")
    data = api("/actions/artifacts/%d/zip" % artifact["id"])
    if "sha256:" + hashlib.sha256(data).hexdigest() != expected_digest:
        raise ValueError("Downloaded artifact differs from GitHub's SHA256 digest")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        if archive.namelist() != ["package-windows.zip"]:
            raise ValueError("Artifact must contain only package-windows.zip")
        windows = archive.read("package-windows.zip")
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "package-windows.zip").write_bytes(windows)
    provenance = {
        "source_sha": args.source_sha, "source_run_url": run["html_url"],
        "repository": args.repository, "workflow_path": run["path"],
        "artifact_id": artifact["id"], "artifact_digest": expected_digest,
        "windows_package_sha256": hashlib.sha256(windows).hexdigest(),
    }
    (args.output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print("Verified artifact %d from %s (%s)" % (artifact["id"], run["html_url"], args.source_sha))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        fetch(args)
    except (ValueError, OSError, KeyError, zipfile.BadZipFile) as exc:
        parser.exit(1, "Error: %s\n" % exc)


if __name__ == "__main__":
    main()
