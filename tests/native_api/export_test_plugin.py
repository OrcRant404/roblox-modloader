#!/usr/bin/env python3
"""Export an optional ordinary plugin fixture for a Core-only Studio check."""
from pathlib import Path
import xml.etree.ElementTree as ET

repository = Path(__file__).resolve().parents[2]
root = ET.Element("roblox", {"version": "4"})
ET.SubElement(root, "External").text = "null"
ET.SubElement(root, "External").text = "nil"
item = ET.SubElement(root, "Item", {"class": "Script", "referent": "RBX_RML_CORE_CONSUMER"})
properties = ET.SubElement(item, "Properties")
ET.SubElement(properties, "string", {"name": "Name"}).text = "RmlCoreConsumer"
ET.SubElement(properties, "ProtectedString", {"name": "Source"}).text = (
    Path(__file__).resolve().parent / "fixtures/core-consumer.luau").read_text()
ET.SubElement(properties, "bool", {"name": "Disabled"}).text = "false"
destination = repository / "build/native-api-test/plugins/RmlCoreConsumer.rbxmx"
destination.parent.mkdir(parents=True, exist_ok=True)
ET.ElementTree(root).write(destination, encoding="utf-8", xml_declaration=True)
print(destination)
