"""Paths for the guarded Workspace example; Core lives in the RML repository."""
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
REPO = PROJECT.parents[1]
MANIFEST = REPO / 'code/roblox_modloader/src/native_api/engine-bindings.arm64.json'
TEST_ID = 'com.orcrant.rml-workspace-test'
APP = PROJECT / 'build/test-studio/RMLWorkspaceTest.app'


def test_headers():
    """Supply the generated export macro for standalone macOS compilation."""
    include = PROJECT / 'build/workspace/include'
    target = include / 'RobloxModLoader/rml_export.hpp'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text('#pragma once\n#define RML_EXPORT __attribute__((visibility("default")))\n#define RML_NO_EXPORT __attribute__((visibility("hidden")))\n')
    return include
