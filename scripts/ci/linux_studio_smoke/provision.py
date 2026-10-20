#!/usr/bin/env python3
"""Provision an exact, isolated Studio deployment using Vinegar/rbxbin's APIs.

Package URLs, manifest grammar, installer .rdata directory mapping and AppSettings
follow sewnie/rbxbin@14ca138004f5, used by Vinegar. No private Studio ABI is used.
Downloaded Studio assets stay in --work and must never be uploaded as artifacts.
"""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import ssl
import stat
import struct
import tarfile
import urllib.parse
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
MIRROR = "https://setup.rbxcdn.com/channel/common/"
APP_SETTINGS = ('<?xml version="1.0" encoding="UTF-8"?>\r\n<Settings>\r\n'
                '        <ContentFolder>content</ContentFolder>\r\n'
                '        <BaseUrl>http://www.roblox.com</BaseUrl>\r\n</Settings>\r\n')
# Microsoft documents distinct HTTP (b/f) and HTTPS (sb/sf) delivery hosts:
# https://learn.microsoft.com/en-us/deployedge/microsoft-edge-security-endpoints
WEBVIEW_HTTPS_COUNTERPARTS = {
    "msedge.b.tlu.dl.delivery.mp.microsoft.com": "msedge.sb.tlu.dl.delivery.mp.microsoft.com",
    "msedge.b.dl.delivery.mp.microsoft.com": "msedge.sb.dl.delivery.mp.microsoft.com",
    "msedge.f.tlu.dl.delivery.mp.microsoft.com": "msedge.sf.tlu.dl.delivery.mp.microsoft.com",
    "msedge.f.dl.delivery.mp.microsoft.com": "msedge.sf.dl.delivery.mp.microsoft.com",
}
WEBVIEW_HTTPS_HOSTS = frozenset(WEBVIEW_HTTPS_COUNTERPARTS.values())


def digest(data):
    return hashlib.sha256(data).hexdigest()


def require_https(url, allowed_hosts=None):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https":
        raise ValueError("Only HTTPS provisioning requests are allowed")
    if allowed_hosts is not None and (parsed.hostname not in allowed_hosts
            or parsed.username is not None or parsed.password is not None
            or parsed.port not in (None, 443) or "\\" in url):
        raise ValueError("Unexpected HTTPS provisioning host")


class ProvisionRedirectHandler(urllib.request.HTTPRedirectHandler):
    def __init__(self, allowed_hosts=None):
        super().__init__()
        self.allowed_hosts = allowed_hosts

    def redirect_request(self, request, fp, code, message, headers, newurl):
        require_https(newurl, self.allowed_hosts)
        return super().redirect_request(request, fp, code, message, headers, newurl)


def get(url, limit, *, context=None, post=False, allowed_hosts=None):
    require_https(url, allowed_hosts)
    request = urllib.request.Request(url, data=b"" if post else None,
        headers={"User-Agent": "RML-Linux-Studio-Smoke/1", "Content-Type": "application/json"})
    opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=context),
                                         ProvisionRedirectHandler(allowed_hosts))
    with opener.open(request, timeout=90) as response:
        require_https(response.url, allowed_hosts)
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError("Provisioning response exceeds its bound")
    return data


def packages(data):
    lines = data.decode("ascii").splitlines()
    if not lines or lines.pop(0) != "v0" or len(lines) % 4:
        raise ValueError("Unsupported or truncated Roblox package manifest")
    result = []
    for i in range(0, len(lines), 4):
        name, md5, compressed, expanded = lines[i:i + 4]
        if (not re.fullmatch(r"[A-Za-z0-9_.-]+", name)
                or not re.fullmatch(r"[0-9a-f]{32}", md5)
                or not compressed.isdecimal() or not expanded.isdecimal()):
            raise ValueError("Invalid package manifest entry")
        entry = dict(name=name, md5=md5, compressed=int(compressed), expanded=int(expanded))
        if not 0 < entry["compressed"] <= 512 * 1024**2 or not 0 < entry["expanded"] <= 2 * 1024**3:
            raise ValueError("Package exceeds bounded smoke deployment size")
        result.append(entry)
    if len(result) > 128 or len({p["name"] for p in result}) != len(result):
        raise ValueError("Duplicate or excessive deployment packages")
    return result


def relative(name):
    name = name.replace("\\", "/")
    if name in ("", "."):
        return PurePosixPath(".")
    path = PurePosixPath(name)
    if path.is_absolute() or ":" in name or ".." in path.parts:
        raise ValueError("Unsafe deployment relative path: " + name)
    return path


def installer_directories(data):
    """Read the real PE .rdata mapping, like rbxbin.BinaryDirectories, not a guessed map."""
    if data[:2] != b"MZ" or len(data) < 64:
        raise ValueError("Invalid Roblox installer DOS header")
    pe = struct.unpack_from("<I", data, 60)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Invalid Roblox installer PE header")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    found = []
    for i in range(count):
        offset = pe + 24 + optional + 40 * i
        if data[offset:offset + 8].rstrip(b"\0") != b".rdata":
            continue
        size, start = struct.unpack_from("<II", data, offset + 16)
        section = data[start:start + size]
        for match in re.finditer(rb'\x00(\{"[^\x00]{30,65536}\})\x00', section):
            try:
                mapping = json.loads(match[1])
            except ValueError:
                continue
            if isinstance(mapping, dict) and "RobloxStudio.zip" in mapping:
                if not all(isinstance(k, str) and isinstance(v, str) for k, v in mapping.items()):
                    raise ValueError("Invalid installer directory map")
                found.append({k: str(relative(v)) for k, v in mapping.items()})
    if len(found) != 1:
        raise ValueError("Expected one actual installer package-directory mapping")
    return found[0]


def extract_zip(data, destination):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        if sum(x.file_size for x in archive.infolist()) > 3 * 1024**3:
            raise ValueError("ZIP expands beyond deployment bound")
        for entry in archive.infolist():
            mode = entry.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise ValueError("Deployment ZIP contains a symbolic link")
            name = entry.filename.replace("\\", "/")
            directory = entry.is_dir() or entry.filename.endswith("\\")
            if name.startswith("/"):
                # Official Roblox Windows ZIPs include empty DOS directory markers
                # such as \\Optimized_Embedded_Signature\\ and \\Qt5\\imageformats\\.
                # Accept that metadata only; a second root, traversal or drive still
                # fails relative(), and absolute files remain forbidden.
                if not (entry.create_system == 0 and entry.external_attr & 0x10
                        and stat.S_IFMT(mode) == 0 and directory
                        and entry.file_size == 0 and entry.compress_size == 0):
                    raise ValueError("Unsafe rooted deployment ZIP entry: " + name)
                name = name[1:]
            name = relative(name)
            if directory and entry.file_size:
                raise ValueError("Deployment ZIP directory contains file data")
            target = destination / name
            if directory:
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(entry) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)


def verified(url, sha256, limit, size=None, *, allowed_hosts=None):
    data = get(url, limit, allowed_hosts=allowed_hosts)
    if digest(data) != sha256 or (size is not None and len(data) != size):
        raise ValueError("Pinned download checksum/size mismatch: " + urllib.parse.urlsplit(url).path)
    return data


def webview_delivery_url(url):
    """Use only Microsoft's documented HTTPS counterpart, retaining the CDP file/token."""
    parsed = urllib.parse.urlsplit(url)
    if (parsed.username is not None or parsed.password is not None or parsed.port is not None
            or parsed.fragment or "\\" in url
            or not re.fullmatch(r"/filestreamingservice/files/[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}",
                                parsed.path)):
        raise ValueError("Unexpected Microsoft runtime delivery URL")
    if parsed.scheme == "http" and parsed.hostname in WEBVIEW_HTTPS_COUNTERPARTS:
        host = WEBVIEW_HTTPS_COUNTERPARTS[parsed.hostname]
    elif parsed.scheme == "https" and parsed.hostname in WEBVIEW_HTTPS_HOSTS:
        host = parsed.hostname
    else:
        raise ValueError("Unexpected Microsoft runtime delivery scheme/host")
    return urllib.parse.urlunsplit(parsed._replace(scheme="https", netloc=host))


def provision(args):
    lock = json.loads((HERE / "lock.json").read_text())
    studio = dict(lock["studio"])
    for key in ("guid", "manifest_sha256", "installer_sha256"):
        if getattr(args, key, None):
            studio[key] = getattr(args, key)
    if not re.fullmatch(r"version-[0-9a-f]{16}", studio["guid"]) or any(
            not re.fullmatch(r"[0-9a-f]{64}", studio[k]) for k in ("manifest_sha256", "installer_sha256")):
        raise ValueError("Exact Studio deployment GUID and SHA256 pins are required")
    args.work.mkdir(parents=True, exist_ok=False)
    root = args.work / "studio"
    root.mkdir()
    base = MIRROR + studio["guid"] + "-"
    manifest = verified(base + "rbxPkgManifest.txt", studio["manifest_sha256"], 1024**2)
    entries = packages(manifest)
    installer = verified(base + "RobloxStudioInstaller.exe", studio["installer_sha256"], 64 * 1024**2)
    mapping = installer_directories(installer)
    if any(p["name"] not in mapping for p in entries):
        raise ValueError("Manifest package has no verified installer destination")

    def download(entry):
        data = get(base + entry["name"], entry["compressed"])
        if len(data) != entry["compressed"] or hashlib.md5(data).hexdigest() != entry["md5"]:
            raise ValueError("Official manifest package checksum/size mismatch: " + entry["name"])
        return entry, data

    records = []
    with ThreadPoolExecutor(max_workers=6) as pool:
        # Extract sequentially: packages can share parent directories.
        for entry, data in pool.map(download, entries):
            extract_zip(data, root / mapping[entry["name"]])
            records.append({**entry, "sha256": digest(data), "directory": mapping[entry["name"]]})
            print("Verified and installed", entry["name"], flush=True)
    (root / "AppSettings.xml").write_bytes(APP_SETTINGS.encode())
    exe = root / "RobloxStudioBeta.exe"
    if not exe.is_file() or exe.read_bytes()[:2] != b"MZ":
        raise ValueError("Complete deployment did not produce RobloxStudioBeta.exe")
    if (root / "dwmapi.dll").exists() or (root / "roblox_modloader.dll").exists():
        raise ValueError("Baseline deployment is not free of RML injection files")

    kombucha = lock["kombucha"]
    archive = verified(kombucha["url"], kombucha["sha256"], 128 * 1024**2)
    wine_root = args.work / "kombucha"
    wine_root.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:xz") as source:
        source.extractall(wine_root, filter="data")
    binaries = [p for p in wine_root.rglob("bin/wine") if p.is_file()]
    if len(binaries) != 1:
        raise ValueError("Kombucha archive did not contain one bin/wine")
    wine = binaries[0]

    vc = lock["vcredist"]
    (args.work / "vc_redist.x64.exe").write_bytes(verified(vc["url"], vc["sha256"], vc["size"], vc["size"]))
    web = lock["webview"]
    ca = (HERE / "microsoft-root-2011.pem").read_bytes()
    if digest(ca) != web["microsoft_ca_sha256"]:
        raise ValueError("Pinned Microsoft CDP trust anchor differs")
    context = ssl.create_default_context()
    context.load_verify_locations(cadata=ca.decode())
    api = ("https://msedge.api.cdp.microsoft.com/api/v1.1/contents/Browser/namespaces/Default/names/"
           "msedge-stable-win-x64/versions/" + web["version"] + "/files?action=GenerateDownloadInfo")
    info = json.loads(get(api, 1024**2, context=context, post=True))
    expected_name = "MicrosoftEdge_X64_" + web["version"] + ".exe"
    candidates = [x for x in info if x.get("FileId") == expected_name]
    if len(candidates) != 1 or base64.b64decode(candidates[0]["Hashes"]["Sha256"]).hex() != web["sha256"]:
        raise ValueError("Microsoft metadata differs from the pinned WebView runtime")
    url = webview_delivery_url(candidates[0]["Url"])
    (args.work / "webview2.exe").write_bytes(verified(url, web["sha256"], web["size"], web["size"],
                                                   allowed_hosts=WEBVIEW_HTTPS_HOSTS))
    return {"studio": studio, "packages": records, "studio_exe_sha256": digest(exe.read_bytes()),
            "kombucha": kombucha, "wine": str(wine), "vcredist": vc,
            "webview": {k: v for k, v in web.items() if k != "microsoft_ca_sha256"},
            "sources": lock["sources"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    for name in ("guid", "manifest-sha256", "installer-sha256"):
        parser.add_argument("--" + name)
    args = parser.parse_args()
    result = provision(args)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
