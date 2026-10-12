#!/usr/bin/env python3
"""Run the workspace mod's native regression checks without launching Studio."""

import argparse
import os
from pathlib import Path
import subprocess
from runtime_paths import REPO, test_headers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", default=REPO, type=Path)
    parser.add_argument("--core-build", type=Path, help="Configured RML Core build directory (default: SDK/build/core-release)")
    parser.add_argument("--compiler", default="/usr/bin/clang++")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    output = project / "build" / "workspace-checks"
    output.mkdir(parents=True, exist_ok=True)
    sdk = args.sdk.resolve()
    core_build = (args.core_build or sdk / "build/core-release").resolve()
    sdk_include = sdk / "code/roblox_modloader/include"
    generated_include = test_headers()
    macos = Path(subprocess.check_output(["/usr/bin/xcrun", "--show-sdk-path"], text=True).strip())
    common = [args.compiler, "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    sanitized = common + ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    engine_includes = ["-I", str(generated_include), "-I", str(sdk_include)]
    cases = [
        ("core", sanitized + ["-Wno-deprecated-declarations",
                              "-I", str(project / "scripts/test-support"),
                              "-I", str(core_build / "code/roblox_modloader/generated/native_api")] + engine_includes,
         ["scripts/test_core_injector.cpp", str(sdk / "code/roblox_modloader/src/native_api/api_registry.cpp")]),
        ("registry", sanitized + engine_includes,
         ["scripts/test_api_registry.cpp", str(sdk / "code/roblox_modloader/src/native_api/api_registry.cpp")]),
        ("stack", sanitized, ["scripts/test_workspace_stack.cpp", "native-workspace/view_stack.cpp"]),
        ("lua", sanitized + engine_includes,
         ["scripts/test_workspace_lua.cpp"]),
        ("host", sanitized + ["-I", str(project / "scripts/test-support")],
         ["scripts/test_workspace_host.cpp", "native-workspace/workspace_host.cpp", "native-workspace/view_stack.cpp"]),
        ("docking", common + ["-O2", "-ffunction-sections", "-fdata-sections", "-Wl,-dead_strip",
                             "-Wno-invalid-constexpr", "-I", str(generated_include),
                             "-I", str(sdk_include), "-I",
                             str(macos / "usr/include/libxml2")],
         ["scripts/test_workspace_docking.cpp"]),
    ]
    for name, flags, sources in cases:
        binary = output / ("test_" + name)
        command = flags + [str(project / source) for source in sources]
        if name == "docking":
            command.append("-lxml2")
        subprocess.run(command + ["-o", str(binary)], cwd=project, check=True)
        environment = os.environ.copy()
        if name == "host":
            # The production host intentionally lives until process exit.
            environment["ASAN_OPTIONS"] = "detect_leaks=0"
        arguments = [str(binary)]
        if name == "docking":
            arguments.append(str(project / "scripts/test-fixtures/workspace-a-layout.xml"))
        subprocess.run(arguments, cwd=project, env=environment, check=True)
        print(name + ": passed", flush=True)


if __name__ == "__main__":
    main()
