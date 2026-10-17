#!/usr/bin/env python3
"""Temporary ARM64 platform port of upstream ZakoVDD v0.17.5 MSBuild project.

This edits ONLY an isolated CI checkout. No driver installation or signing.
Keep upstream DLL/INF, IOCTLs, and driver GUIDs unchanged.
"""
import copy
import pathlib
import sys
import xml.etree.ElementTree as ET

if len(sys.argv) != 2:
    raise SystemExit("Usage: python arm64_vdd_project_port.py path/to/ZakoVDD.vcxproj")

p = pathlib.Path(sys.argv[1])
ET.register_namespace("", "http://schemas.microsoft.com/developer/msbuild/2003")
ns = "{http://schemas.microsoft.com/developer/msbuild/2003}"
tree = ET.parse(p)
root = tree.getroot()

pc = None
for group in root.findall(ns + "ItemGroup"):
    if group.get("Label") == "ProjectConfigurations":
        pc = group
        break
if pc is None:
    raise RuntimeError("MSBuild ProjectConfigurations group missing")

release = None
for elem in pc.findall(ns + "ProjectConfiguration"):
    if elem.get("Include") == "Release|x64":
        release = elem
    if elem.get("Include") == "Release|ARM64":
        raise RuntimeError("ARM64 is already configured; refusing duplicate")
if release is None:
    raise RuntimeError("Release|x64 configuration not found")

arm = copy.deepcopy(release)
arm.set("Include", "Release|ARM64")
platform = arm.find(ns + "Platform")
if platform is None or platform.text != "x64":
    raise RuntimeError("Release|x64 has unexpected Platform element")
platform.text = "ARM64"
pc.insert(list(pc).index(release) + 1, arm)

count = 0
for child in list(root):
    cond = child.get("Condition", "")
    if "Release|x64" not in cond:
        continue
    if child.tag not in (ns + "PropertyGroup", ns + "ImportGroup", ns + "ItemDefinitionGroup"):
        raise RuntimeError("Unrecognized conditioned element: " + child.tag)
    arm_child = copy.deepcopy(child)
    arm_child.set("Condition", cond.replace("Release|x64", "Release|ARM64"))
    root.insert(list(root).index(child) + 1, arm_child)
    count += 1

if count < 4:
    raise RuntimeError(f"Too few Release|x64 project groups cloned: {count}")

# Make the architecture visible in project properties, but do not alter
# source, INF, GUID, service, or source-level driver behavior.
tree.write(p, encoding="utf-8", xml_declaration=True)
final = p.read_text(encoding="utf-8")
if final.count("Release|ARM64") != count + 1:
    raise RuntimeError("Failed to instantiate the expected ARM64 MSBuild groups")
print(f"Created ARM64 Release platform from upstream x64 definition: {count} groups")
print("This does not sign or install the driver.")
