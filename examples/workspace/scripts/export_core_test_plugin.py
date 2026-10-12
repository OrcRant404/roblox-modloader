#!/usr/bin/env python3
"""Export an optional ordinary consumer for a Core-only Studio test."""
import xml.etree.ElementTree as ET
from runtime_paths import PROJECT

root = ET.Element('roblox', {'version': '4'})
ET.SubElement(root, 'External').text = 'null'
ET.SubElement(root, 'External').text = 'nil'
item = ET.SubElement(root, 'Item', {'class': 'Script', 'referent': 'RBX_RML_CORE_CONSUMER'})
props = ET.SubElement(item, 'Properties')
ET.SubElement(props, 'string', {'name': 'Name'}).text = 'RmlCoreConsumer'
ET.SubElement(props, 'ProtectedString', {'name': 'Source'}).text = (PROJECT / 'scripts/test-fixtures/core-consumer.luau').read_text()
ET.SubElement(props, 'bool', {'name': 'Disabled'}).text = 'false'
destination = PROJECT / 'build/global-test/plugins/RmlCoreConsumer.rbxmx'
destination.parent.mkdir(parents=True, exist_ok=True)
ET.ElementTree(root).write(destination, encoding='utf-8', xml_declaration=True)
print(destination)
