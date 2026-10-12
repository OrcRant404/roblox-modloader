#!/usr/bin/env python3
"""Build only Workspace against the rebuilt RML loader and public SDK."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


from runtime_paths import APP, PROJECT, REPO, test_headers


def export_test_plugins() -> Path:
    destination = PROJECT / 'build/workspace-test/plugins'
    destination.mkdir(parents=True, exist_ok=True)
    consumer = (PROJECT / 'scripts/test-fixtures/workspace-consumer.luau').read_text()
    for label in ('A', 'B'):
        root = ET.Element('roblox', {'version': '4'})
        ET.SubElement(root, 'External').text = 'null'
        ET.SubElement(root, 'External').text = 'nil'
        item = ET.SubElement(root, 'Item', {'class': 'Script', 'referent': 'RBX_WORKSPACE_' + label})
        props = ET.SubElement(item, 'Properties')
        ET.SubElement(props, 'string', {'name': 'Name'}).text = 'WorkspaceConsumer' + label
        ET.SubElement(props, 'ProtectedString', {'name': 'Source'}).text = consumer.replace('__VIEW_LABEL__', label)
        ET.SubElement(props, 'bool', {'name': 'Disabled'}).text = 'false'
        ET.ElementTree(root).write(destination / ('WorkspaceConsumer' + label + '.rbxmx'),
                                   encoding='utf-8', xml_declaration=True)
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=REPO)
    parser.add_argument('--loader', type=Path, required=True, help='Rebuilt RML library with the built-in native API')
    parser.add_argument('--test-plugins', action='store_true',
                        help='Export the ordinary A/B Workspace regression plugins')
    args = parser.parse_args()
    build = PROJECT / 'build/workspace'
    include = test_headers()
    headers = args.sdk.resolve() / 'code/roblox_modloader/include'
    loader = args.loader.resolve()
    if not loader.is_file():
        parser.error('Rebuilt RML loader is missing')
    macos = Path(subprocess.check_output(['/usr/bin/xcrun', '--show-sdk-path'], text=True).strip())
    executable = APP / 'Contents/MacOS/RobloxStudio'
    sources = [PROJECT / 'native-workspace' / name for name in (
        'main.cpp', 'workspace_host.cpp', 'workspace_lua.cpp', 'view_stack.cpp', 'docking.cpp',
        'layout_snapshot.cpp', 'qt_inspection.cpp')]
    output = build / 'Workspace.dylib'
    environment = os.environ.copy()
    environment.pop('ROBLOX_STUDIO_PATH', None)
    subprocess.run([
        '/usr/bin/clang++', '-std=c++23', '-dynamiclib', '-fvisibility=hidden',
        '-Wall', '-Wextra', '-Wpedantic', '-Wno-invalid-constexpr',
        '-I', str(include), '-I', str(headers), '-I', str(macos / 'usr/include/libxml2'),
        '-DRML_TEST_EXECUTABLE=' + json.dumps(str(executable)),
        *map(str, sources), str(loader), '-lxml2',
        '-Wl,-undefined,error', '-o', str(output)], env=environment, check=True)
    subprocess.run(['/usr/bin/install_name_tool', '-change',
                    '@rpath/libroblox_modloader.dylib',
                    '@loader_path/../../../../roblox_modloader.dylib', str(output)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(output)], check=True)
    result = {'workspace_mod': str(output)}
    if args.test_plugins:
        result['plugins'] = str(export_test_plugins())
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
