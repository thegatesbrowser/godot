#!/usr/bin/env python3
"""Unified scons entry point for the TheGates Godot fork.

One source of truth for the flag combinations used by VSCode tasks,
the autonomous test loop, agent instructions, and the README.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
GODOT_DIR = SCRIPT_DIR.parent

# --- Profiles -------------------------------------------------------------
# Each profile is the scons arg list that defines a canonical build flavor.
# Optional toggles (-j, --arch, --sandbox, extra args) are layered on top.

PROFILES: dict[str, list[str]] = {
    "launcher": [
        "dev_build=yes",
        "tg_renderer=no",
        "compiledb=yes",
        "use_llvm=yes",
        "linker=lld",
        "disable_exceptions=no",
    ],
    "renderer": [
        "dev_build=yes",
        "tg_renderer=yes",
        "target=template_debug",
        "compiledb=yes",
        "use_llvm=yes",
        "linker=lld",
        "disable_exceptions=no",
    ],
    "launcher-release": [
        "production=yes",
        "tools=no",
        "target=template_release",
        "optimize=speed",
        "tg_renderer=no",
        "disable_exceptions=no",
    ],
    "renderer-release": [
        "production=yes",
        "tools=no",
        "target=template_release",
        "optimize=speed",
        "tg_renderer=yes",
        "disable_exceptions=no",
    ],
}


def default_jobs() -> int:
    # Leave a couple of cores for the OS so PowerShell, the editor, and
    # Defender stay responsive during long compile jobs.
    cpu = os.cpu_count() or 4
    return max(1, cpu - 2)


def format_profile_table() -> str:
    rows = []
    for name in sorted(PROFILES):
        flags = " ".join(PROFILES[name])
        rows.append(f"  {name:<20} {flags}")
    return "\n".join(rows)


HELP_EPILOG = f"""\
profiles:
{format_profile_table()}

examples:
  build.py launcher                       # dev launcher (the everyday build)
  build.py renderer                       # dev renderer
  build.py renderer --no-sandbox          # dev renderer without the chromium sandbox
  build.py launcher-release               # production launcher (host arch)
  build.py renderer-release --mac-intel   # production renderer for Intel Macs
  build.py launcher -j 4                  # cap parallelism manually
  build.py launcher --dry-run             # print the scons command and exit
  build.py launcher -- verbose=yes        # forward extra args to scons

notes:
  - Runs scons from the godot/ submodule regardless of cwd.
  - Default -j is cpu_count - 2 so PowerShell + the OS stay responsive
    during long builds. Override with -j N.
  - tg_sandbox is on by default in scons; --no-sandbox opts out for
    faster iteration.
  - Anything after `--` is forwarded to scons verbatim (e.g. verbose=yes,
    a different target=, custom CCFLAGS).
"""


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="build.py",
        description=(
            "Unified entry point for building the TheGates Godot fork. "
            "Wraps scons with the canonical flag combinations used by "
            "VSCode tasks, the autonomous test loop, agent instructions, "
            "and the README - one source of truth so the flag set "
            "doesn't drift."
        ),
        epilog=HELP_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "profile",
        choices=sorted(PROFILES.keys()),
        metavar="profile",
        help="build profile (see list below)",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=default_jobs(),
        metavar="N",
        help=f"parallel jobs (default: cpu_count - 2 = {default_jobs()})",
    )
    parser.add_argument(
        "--mac-intel",
        dest="mac_intel",
        action="store_true",
        help="target Intel Macs (passes arch=x86_64; only meaningful on macOS)",
    )
    parser.add_argument(
        "--no-sandbox",
        dest="no_sandbox",
        action="store_true",
        help="opt out of the chromium sandbox (passes tg_sandbox=no); sandbox is on by default",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the resolved scons command and exit without running it",
    )
    # Manually split on `--` so scons-bound flags don't shadow our own.
    argv = sys.argv[1:]
    if "--" in argv:
        sep = argv.index("--")
        before, extra = argv[:sep], argv[sep + 1 :]
    else:
        before, extra = argv, []
    args = parser.parse_args(before)

    scons = shutil.which("scons")
    if scons is None:
        print("error: scons not found in PATH", file=sys.stderr)
        return 127

    cmd: list[str] = [scons, f"-j{args.jobs}"]
    cmd.extend(PROFILES[args.profile])
    if args.mac_intel:
        cmd.append("arch=x86_64")
    if args.no_sandbox:
        cmd.append("tg_sandbox=no")
    cmd.extend(extra)

    print(f"[build] cwd={GODOT_DIR}")
    print(f"[build] cmd={' '.join(cmd)}", flush=True)
    if args.dry_run:
        return 0

    try:
        return subprocess.call(cmd, cwd=str(GODOT_DIR))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
