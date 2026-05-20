#!/usr/bin/env python3
"""Build the macOS release templates: x86_64 + arm64 -> universal binary.

Runs the four production builds (launcher + renderer, each for Intel and
Apple Silicon) via tools/build.py, lipos the per-arch binaries into
universal binaries, drops them into the macOS .app template, and zips
the result for distribution.

Run from anywhere; the script resolves the godot/ submodule itself.
"""

import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent     # tools/macos/
TOOLS_DIR = SCRIPT_DIR.parent                    # tools/
GODOT_DIR = TOOLS_DIR.parent                     # godot/
BIN_DIR = GODOT_DIR / "bin"
BUILD_PY = TOOLS_DIR / "build.py"
TEMPLATE_SRC = SCRIPT_DIR / "macos_template.app"

# The four production builds we need before we can lipo. Each entry maps to
# `python tools/build.py <profile> [--mac-intel]`.
RELEASE_BUILDS = [
    ("launcher-release", []),                  # arm64 (host) launcher
    ("renderer-release", []),                  # arm64 renderer
    ("launcher-release", ["--mac-intel"]),     # x86_64 launcher
    ("renderer-release", ["--mac-intel"]),     # x86_64 renderer
]


def run(cmd, description):
    print(f"Running: {description}")
    print(f"Command: {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, cwd=str(GODOT_DIR))
    if result.returncode != 0:
        print(f"Error running {description} (exit {result.returncode})")
        sys.exit(result.returncode)
    print(f"OK: {description}")


def run_release_builds():
    for profile, flags in RELEASE_BUILDS:
        label = f"{profile} {' '.join(flags)}".strip()
        run([sys.executable, str(BUILD_PY), profile] + flags, f"build {label}")


def get_godot_version() -> str:
    # Parse godot/version.py for major / minor without evaluating it.
    text = (GODOT_DIR / "version.py").read_text()
    major = re.search(r"^\s*major\s*=\s*(\d+)", text, re.MULTILINE)
    minor = re.search(r"^\s*minor\s*=\s*(\d+)", text, re.MULTILINE)
    if not major or not minor:
        print("Error: could not parse major/minor from version.py")
        sys.exit(1)
    return f"{major.group(1)}.{minor.group(1)}"


def create_universal_binary(x86_64_path, arm64_path, universal_path, description):
    if not os.path.exists(x86_64_path):
        print(f"Warning: {x86_64_path} does not exist, skipping {description}")
        return False
    if not os.path.exists(arm64_path):
        print(f"Warning: {arm64_path} does not exist, skipping {description}")
        return False
    run(
        ["lipo", "-create", str(x86_64_path), str(arm64_path), "-output", str(universal_path)],
        f"lipo {description}",
    )
    return True


def main():
    print("=== TheGates macOS build ===")
    print(f"godot dir: {GODOT_DIR}")
    print(f"bin dir:   {BIN_DIR}")

    print("\n=== Building release tasks ===")
    run_release_builds()

    print("\n=== Creating universal binaries ===")
    binaries = [
        {
            "name": "godot.macos.template_release",
            "x86_64": BIN_DIR / "godot.macos.template_release.x86_64",
            "arm64": BIN_DIR / "godot.macos.template_release.arm64",
            "universal": BIN_DIR / "godot.macos.template_release.universal",
        },
        {
            "name": "godot.macos.template_release.renderer",
            "x86_64": BIN_DIR / "godot.macos.template_release.renderer.x86_64",
            "arm64": BIN_DIR / "godot.macos.template_release.renderer.arm64",
            "universal": BIN_DIR / "godot.macos.template_release.renderer.universal",
        },
    ]
    for b in binaries:
        create_universal_binary(b["x86_64"], b["arm64"], b["universal"], b["name"])

    print("\n=== Assembling app template ===")
    working_template = SCRIPT_DIR / "macos_template_working.app"
    if working_template.exists():
        shutil.rmtree(working_template)
        print("Removed existing working template")

    shutil.copytree(TEMPLATE_SRC, working_template)
    print(f"Copied template src -> {working_template}")

    working_macos = working_template / "Contents" / "MacOS"
    working_frameworks = working_template / "Contents" / "Frameworks"
    working_macos.mkdir(parents=True, exist_ok=True)
    working_frameworks.mkdir(parents=True, exist_ok=True)

    version_str = get_godot_version()
    renderer_bin_name = f"Renderer-godot_v{version_str}.universal"
    payloads = [
        {
            "source": BIN_DIR / "godot.macos.template_release.universal",
            "dest": working_macos / "godot_macos_release.universal",
            "description": "godot_macos_release.universal",
        },
        {
            "source": BIN_DIR / "godot.macos.template_release.renderer.universal",
            "dest": working_frameworks / renderer_bin_name,
            "description": renderer_bin_name,
        },
    ]
    for p in payloads:
        if p["source"].exists():
            shutil.copy2(p["source"], p["dest"])
            print(f"Copied {p['description']}")
        else:
            print(f"Warning: {p['source']} does not exist, skipping")

    print("\n=== Moving app template to bin/ ===")
    final_app = BIN_DIR / "macos_template.app"
    if final_app.exists():
        shutil.rmtree(final_app)
        print("Removed existing macos_template.app in bin/")
    shutil.move(str(working_template), str(final_app))
    print(f"Moved -> {final_app}")

    print("\n=== Creating macOS template archive ===")
    zip_path = BIN_DIR / "macos.zip"
    if zip_path.exists():
        zip_path.unlink()
        print("Removed existing macos.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zipf:
        for root, _dirs, files in os.walk(final_app):
            for file in files:
                file_path = Path(root) / file
                arcname = file_path.relative_to(final_app)
                zipf.write(file_path, arcname)
    print(f"Created {zip_path}")

    print("\n=== Done ===")
    print("Universal binaries:")
    for b in binaries:
        if b["universal"].exists():
            print(f"  {b['universal']}")
    print(f"App template: {final_app}")
    print(f"Archive:      {zip_path}")


if __name__ == "__main__":
    main()
