#!/usr/bin/env python3
"""
Build script for The Gates Godot project
Builds all tasks except "build" and "build sandbox", creates universal binaries,
and organizes output files in the correct locations.
"""

import os
import sys
import subprocess
import shutil
from pathlib import Path

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

def run_command(command, description):
	"""Run a command and handle errors"""
	print(f"Running: {description}")
	# Convert Path objects to strings for display
	command_str = ' '.join(str(arg) for arg in command)
	print(f"Command: {command_str}")
	
	result = subprocess.run(command, capture_output=True, text=True)
	
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
	
	# Build tasks (excluding "build" and "build sandbox")
	build_tasks = [
		"build (template_release)",
		"build sandbox (template_release)",
		"build (template_release, x86_64)",
		"build sandbox (template_release, x86_64)"
	]
	
	print("\n=== Building Tasks ===")
	for task in build_tasks:
		command = [editor, "--command", f"workbench.action.tasks.runTask", "--args", task]
		run_command(command, f"Building {task}")
	
	print("\n=== Creating Universal Binaries ===")
	
	# Define the binary paths
	binaries = [
		{
			"name": "godot.macos.template_release",
			"x86_64": bin_dir / "godot.macos.template_release.x86_64",
			"arm64": bin_dir / "godot.macos.template_release.arm64",
			"universal": bin_dir / "godot.macos.template_release.universal"
		},
		{
			"name": "godot.macos.template_release.sandbox",
			"x86_64": bin_dir / "godot.macos.template_release.sandbox.x86_64",
			"arm64": bin_dir / "godot.macos.template_release.sandbox.arm64",
			"universal": bin_dir / "godot.macos.template_release.sandbox.universal"
		}
	]
	
	# Create universal binaries
	for binary in binaries:
		create_universal_binary(
			binary["x86_64"],
			binary["arm64"],
			binary["universal"],
			binary["name"]
		)
	
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
	universal_binaries = [
		{
			"source": bin_dir / "godot.macos.template_release.universal",
			"dest": working_template_macos / "godot_macos_release.universal",
			"description": "godot_macos_release.universal"
		},
		{
			"source": bin_dir / "godot.macos.template_release.sandbox.universal",
			"dest": working_template_frameworks / "Sandbox.universal",
			"description": "Sandbox.universal"
		}
	]
	
	for binary in universal_binaries:
		if binary["source"].exists():
			shutil.copy2(binary["source"], binary["dest"])
			print(f"✓ Copied {binary['description']} to working template")
		else:
			print(f"Warning: {binary['source']} does not exist, skipping copy")
	
	# Copy libzmq libraries if they exist
	libzmq_files = ["libzmq.dylib", "libzmq.5.dylib"]
	for libzmq_file in libzmq_files:
		source = bin_dir / libzmq_file
		dest = working_template_frameworks / libzmq_file
		if source.exists():
			shutil.copy2(source, dest)
			print(f"✓ Copied {libzmq_file} to working template")
		else:
			print(f"Warning: {libzmq_file} does not exist, skipping copy")
	
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
	
	print("\n=== Build Complete ===")
	print("Universal binaries created:")
	for binary in binaries:
		if binary["universal"].exists():
			print(f"  - {binary['universal']}")
	
	print(f"App template available at: {final_app_template}")

if __name__ == "__main__":
	main()
