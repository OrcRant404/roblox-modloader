#!/usr/bin/env python3
"""Deploy the rebuilt RML Core and optional Workspace to the marked test app."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import time

from runtime_paths import APP, MANIFEST, PROJECT, TEST_ID
from prepare_test_studio import code_sha256, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--studio', type=Path, required=True)
    parser.add_argument('--loader', type=Path, required=True,
                        help='Rebuilt RML loader with the built-in native API')
    parser.add_argument('--workspace', action='store_true',
                        help='Install Workspace; default installs no external mods')
    args = parser.parse_args()
    source, loader = args.studio.resolve(), args.loader.resolve()
    root = APP.parent
    marker = root / 'test-copy.json'
    metadata = json.loads(marker.read_text())
    if (metadata.get('source') != str(source) or metadata.get('bundle_id') != TEST_ID
            or metadata.get('app') != str(APP)):
        parser.error('Refusing to change an unmarked or unrelated Studio copy')
    if source == APP.resolve():
        parser.error('Source and test app must be separate')
    info = plistlib.loads((APP / 'Contents/Info.plist').read_bytes())
    if info.get('CFBundleIdentifier') != TEST_ID:
        parser.error('Test app bundle identity changed')
    manifest = json.loads(MANIFEST.read_text())
    if code_sha256(APP / 'Contents/MacOS/RobloxStudio', manifest) != manifest['code_sha256']:
        parser.error('Engine image does not match the verified bindings')
    processes = subprocess.check_output(['/bin/ps', '-axo', 'command='], text=True)
    if str(APP / 'Contents/MacOS/RobloxStudio') in processes:
        parser.error('Close the test Studio before deployment')
    source_loader = source / 'Contents/MacOS/roblox_modloader.dylib'
    original_hash = sha256(source_loader)
    if metadata.get('source_loader_sha256') != original_hash:
        parser.error('Source loader changed since base preparation')
    if not loader.is_file() or loader == source_loader.resolve():
        parser.error('Supply the newly built Core, separate from the source loader')
    exports = subprocess.check_output(['/usr/bin/nm', '-gU', str(loader)], text=True)
    for symbol in ('rml_native_api_abi_version', 'rml_native_api_publish_namespace',
                   'rml_native_api_register_namespace', 'rml_native_api_dispatch_engine'):
        if '_' + symbol not in exports:
            parser.error('Rebuilt loader is missing the public native API')
    binary = PROJECT / 'build/workspace/Workspace.dylib'
    if args.workspace and not binary.is_file():
        parser.error('Build Workspace before requesting its deployment')
    installed = APP / 'Contents/MacOS/roblox_modloader.dylib'
    if installed.exists():
        shutil.copy2(installed, root / f'loader-before-native-api-{time.time_ns()}.dylib')
    shutil.copy2(loader, installed)
    mods = APP / 'Contents/MacOS/RobloxModLoader/mods'
    if mods.exists():
        mods.rename(root / f'mods-before-native-api-{time.time_ns()}')
    mods.mkdir(parents=True)
    if args.workspace:
        target = mods / 'workspace'
        (target / 'native').mkdir(parents=True)
        shutil.copy2(binary, target / 'native/Workspace.dylib')
        shutil.copy2(PROJECT / 'native-workspace/mod.toml', target / 'mod.toml')
    sign = ['/usr/bin/codesign', '--force', '--deep', '--sign', '-']
    entitlements = root / 'source-entitlements.plist'
    if entitlements.is_file():
        plistlib.loads(entitlements.read_bytes())
        sign += ['--entitlements', str(entitlements)]
    subprocess.run([*sign, str(APP)], check=True)
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(APP)], check=True)
    if sha256(source_loader) != original_hash:
        raise RuntimeError('Source loader changed during deployment')
    if code_sha256(APP / 'Contents/MacOS/RobloxStudio', manifest) != manifest['code_sha256']:
        raise RuntimeError('Signing changed the engine code identity')
    plugins = PROJECT / ('build/workspace-test/plugins' if args.workspace else 'build/global-test/plugins')
    plugins.mkdir(parents=True, exist_ok=True)
    proof = {'source_loader_unchanged': True, 'source_loader_sha256': original_hash,
             'built_loader_sha256': sha256(installed), 'built_artifact_sha256': sha256(loader),
             'test_engine_code_sha256': manifest['code_sha256'],
             'profile': 'workspace' if args.workspace else 'native-api',
             'mods': ['workspace'] if args.workspace else [],
             'plugin_folder': str(plugins)}
    deployment = PROJECT / 'build/global-test/deployment.json'
    deployment.parent.mkdir(parents=True, exist_ok=True)
    deployment.write_text(json.dumps(proof, indent=2) + '\n')
    print(json.dumps(proof, indent=2))


if __name__ == '__main__':
    main()
