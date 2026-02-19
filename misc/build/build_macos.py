#!/usr/bin/env python3
"""
Build script for The Gates Godot project
Builds all tasks except "build" and "build renderer", creates universal binaries,
and organizes output files in the correct locations.
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict


def detect_editor():
    """Detect available code editor (VS Code or Cursor)"""
    editors = ["cursor", "code"]
    for editor in editors:
        try:
            result = subprocess.run([editor, "--version"], capture_output=True, text=True)
            if result.returncode == 0:
                return editor
        except FileNotFoundError:
            continue
    return None


def get_cpu_count():
    """Get the number of CPU cores for parallel builds"""
    try:
        import multiprocessing

        return str(multiprocessing.cpu_count())
    except Exception:
        return "4"  # fallback


def get_godot_version(project_root: Path) -> str:
    """Read Godot version from version.py and return '<major>.<minor>'"""
    version_file = project_root / "version.py"
    try:
        namespace: Dict[str, Any] = {}
        with open(version_file, "r") as f:
            code = f.read()
        exec(code, namespace)
        major = namespace.get("major")
        minor = namespace.get("minor")
        return f"{major}.{minor}"
    except Exception as e:
        print(f"Error reading version.py: {e}")
        sys.exit(1)


def parse_tasks_json(project_root):
    """Parse tasks.json and extract build tasks excluding 'build' and 'build renderer'"""
    tasks_file = project_root / ".vscode" / "tasks.json"

    if not tasks_file.exists():
        print(f"Error: tasks.json not found at {tasks_file}")
        sys.exit(1)

    try:
        # Read file content and remove comments
        with open(tasks_file, "r") as f:
            content = f.read()

        # Remove single-line comments
        lines = content.split("\n")
        cleaned_lines = []
        for line in lines:
            # Remove comments starting with //
            if "//" in line:
                line = line.split("//")[0]
            cleaned_lines.append(line)

        # Join lines back together
        cleaned_content = "\n".join(cleaned_lines)

        # Parse as JSON
        tasks_data = json.loads(cleaned_content)

    except json.JSONDecodeError as e:
        print(f"Error parsing tasks.json: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"Error reading tasks.json: {e}")
        sys.exit(1)

    build_tasks = []
    # Only include these specific tasks (excluding "build" and "build renderer")
    included_tasks = [
        "build (template_release)",
        "build renderer (template_release)",
        "build (template_release, x86_64)",
        "build renderer (template_release, x86_64)",
    ]

    for task in tasks_data.get("tasks", []):
        task_label = task.get("label", "")
        if task_label in included_tasks:
            # Extract command and args
            command = task.get("command", "")
            args = task.get("args", [])

            build_tasks.append({"name": task_label, "command": [command], "args": args})

    return build_tasks


def run_command(command, description):
    """Run a command and handle errors"""
    print(f"Running: {description}")

    # Handle shell expansions like $(nproc)
    processed_command = []
    for arg in command:
        if arg == "$(nproc)":
            processed_command.append(get_cpu_count())
        else:
            processed_command.append(str(arg))

    # Convert Path objects to strings for display
    command_str = " ".join(processed_command)
    print(f"Command: {command_str}")

    result = subprocess.run(processed_command, capture_output=True, text=True)

    if result.returncode != 0:
        print(f"Error running {description}:")
        print(f"STDOUT: {result.stdout}")
        print(f"STDERR: {result.stderr}")
        sys.exit(1)

    print(f"✓ {description} completed successfully")
    return result


def create_universal_binary(x86_64_path, arm64_path, universal_path, description):
    """Create a universal binary using lipo"""
    if not os.path.exists(x86_64_path):
        print(f"Warning: {x86_64_path} does not exist, skipping {description}")
        return False

    if not os.path.exists(arm64_path):
        print(f"Warning: {arm64_path} does not exist, skipping {description}")
        return False

    # Create universal binary using lipo
    command = ["lipo", "-create", x86_64_path, arm64_path, "-output", universal_path]
    run_command(command, f"Creating universal binary for {description}")
    return True


def main():
    # Get the project root directory
    project_root = Path(__file__).parent.parent.parent
    bin_dir = project_root / "bin"

    print("=== The Gates Godot Build Script ===")
    print(f"Project root: {project_root}")
    print(f"Bin directory: {bin_dir}")

    # Ensure we're in the project root
    os.chdir(project_root)

    # Detect available editor
    editor = detect_editor()
    if not editor:
        print("Error: Neither 'code' (VS Code) nor 'cursor' found in PATH")
        print("Please ensure one of these editors is installed and available in your PATH")
        sys.exit(1)

    print(f"Using editor: {editor}")

    # Parse build tasks from tasks.json
    build_tasks = parse_tasks_json(project_root)

    if not build_tasks:
        print("Error: No build tasks found in tasks.json")
        sys.exit(1)

    print(f"Found {len(build_tasks)} build tasks:")
    for task in build_tasks:
        print(f"  - {task['name']}")

    print("\n=== Building Tasks ===")
    for task in build_tasks:
        command = task["command"] + task["args"]
        run_command(command, f"Building {task['name']}")

    print("\n=== Creating Universal Binaries ===")

    # Define the binary paths
    binaries = [
        {
            "name": "godot.macos.template_release",
            "x86_64": bin_dir / "godot.macos.template_release.x86_64",
            "arm64": bin_dir / "godot.macos.template_release.arm64",
            "universal": bin_dir / "godot.macos.template_release.universal",
        },
        {
            "name": "godot.macos.template_release.renderer",
            "x86_64": bin_dir / "godot.macos.template_release.renderer.x86_64",
            "arm64": bin_dir / "godot.macos.template_release.renderer.arm64",
            "universal": bin_dir / "godot.macos.template_release.renderer.universal",
        },
    ]

    # Create universal binaries
    for binary in binaries:
        create_universal_binary(binary["x86_64"], binary["arm64"], binary["universal"], binary["name"])

    print("\n=== Organizing App Template ===")

    # First, copy the original template to a working directory
    original_template_dir = project_root / "misc" / "build" / "macos_template.app"
    working_template_dir = project_root / "misc" / "build" / "macos_template_working.app"

    # Remove working directory if it exists
    if working_template_dir.exists():
        shutil.rmtree(working_template_dir)
        print("✓ Removed existing working template directory")

    # Copy original template to working directory
    shutil.copytree(original_template_dir, working_template_dir)
    print("✓ Copied original template to working directory")

    # Set up paths for the working template
    working_template_contents = working_template_dir / "Contents"
    working_template_macos = working_template_contents / "MacOS"
    working_template_frameworks = working_template_contents / "Frameworks"

    # Ensure directories exist
    working_template_macos.mkdir(parents=True, exist_ok=True)
    working_template_frameworks.mkdir(parents=True, exist_ok=True)

    # Copy universal binaries with appropriate names
    version_str = get_godot_version(project_root)
    renderer_bin_name = f"Renderer-godot_v{version_str}.universal"
    universal_binaries = [
        {
            "source": bin_dir / "godot.macos.template_release.universal",
            "dest": working_template_macos / "godot_macos_release.universal",
            "description": "godot_macos_release.universal",
        },
        {
            "source": bin_dir / "godot.macos.template_release.renderer.universal",
            "dest": working_template_frameworks / renderer_bin_name,
            "description": renderer_bin_name,
        },
    ]

    for binary in universal_binaries:
        if binary["source"].exists():
            shutil.copy2(binary["source"], binary["dest"])
            print(f"✓ Copied {binary['description']} to working template")
        else:
            print(f"Warning: {binary['source']} does not exist, skipping copy")

    print("\n=== Moving App Template to Bin Directory ===")

    # Move the working app template to bin directory
    final_app_template = bin_dir / "macos_template.app"

    # Remove existing app template if it exists
    if final_app_template.exists():
        shutil.rmtree(final_app_template)
        print("✓ Removed existing macos_template.app")

    # Move the working template to bin directory
    shutil.move(str(working_template_dir), str(final_app_template))
    print("✓ Moved working template to bin/macos_template.app")

    print("\n=== Creating macOS Template Archive ===")

    # Create zip file of the app template
    import zipfile

    zip_path = bin_dir / "macos.zip"

    # Remove existing zip if it exists
    if zip_path.exists():
        zip_path.unlink()
        print("✓ Removed existing macos.zip")

    # Create zip file
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zipf:
        # Walk through the app template directory
        for root, dirs, files in os.walk(final_app_template):
            for file in files:
                file_path = Path(root) / file
                # Calculate relative path from the app template root
                arcname = file_path.relative_to(final_app_template)
                zipf.write(file_path, arcname)

    print(f"✓ Created macOS template archive: {zip_path}")

    print("\n=== Build Complete ===")
    print("Universal binaries created:")
    for binary in binaries:
        if binary["universal"].exists():
            print(f"  - {binary['universal']}")

    renderer_full_path = final_app_template / "Contents" / "Frameworks" / renderer_bin_name
    print(f"Renderer binary: {renderer_full_path}")

    print(f"App template available at: {final_app_template}")
    print(f"macOS template archive: {zip_path}")


if __name__ == "__main__":
    main()
