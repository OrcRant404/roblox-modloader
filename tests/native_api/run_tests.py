#!/usr/bin/env python3
"""Run the built-in native API regressions without loading Studio or any mod."""
import argparse
from pathlib import Path
import subprocess


def main():
    repository = Path(__file__).resolve().parents[2]
    tests = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-build", type=Path, default=repository / "build/core-release",
                        help="Configured RML build containing the generated engine binding header")
    parser.add_argument("--compiler", default="/usr/bin/clang++")
    args = parser.parse_args()
    bindings = args.core_build.resolve() / "code/roblox_modloader/generated/native_api"
    if not (bindings / "engine_bindings.generated.hpp").is_file():
        parser.error("Configure the RML CMake build first, or pass --core-build /path/to/configured/build")
    output = repository / "build/native-api-checks"
    include = output / "include/RobloxModLoader"
    include.mkdir(parents=True, exist_ok=True)
    (include / "rml_export.hpp").write_text(
        '#pragma once\n#define RML_EXPORT __attribute__((visibility("default")))\n'
        '#define RML_NO_EXPORT __attribute__((visibility("hidden")))\n')
    flags = [args.compiler, "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(include.parent), "-I", str(repository / "code/roblox_modloader/include")]
    registry = repository / "code/roblox_modloader/src/native_api/api_registry.cpp"
    cases = [
        ("core", flags[:1] + ["-Wno-deprecated-declarations", "-I", str(tests / "support"),
                             "-I", str(bindings)] + flags[1:], tests / "test_core_injector.cpp"),
        ("registry", flags, tests / "test_api_registry.cpp"),
    ]
    for name, options, source in cases:
        binary = output / ("test_" + name)
        subprocess.run(options + [str(source), str(registry), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        print(name + ": passed", flush=True)


if __name__ == "__main__":
    main()
