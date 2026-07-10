#!/usr/bin/env python3
"""Autonomous launcher+renderer sandbox verification loop.

Single source of truth for Windows, macOS, and Linux. Boots TheGates launcher
with an autotest payload, lets it open a gate, waits for the renderer to spawn
and reach its first frame, then parses the renderer's SANDBOX-DIAG-BEGIN/END
JSON block plus a few health checks. Emits exactly one [VERIFY-OK] or
[VERIFY-FAIL] <reason> line plus the parsed JSON on success. Exit code 0 = pass;
non-zero = specific failure mode (see FAIL_CATALOG below).

Designed for an agent loop: every run produces <30 lines of output and a stable
exit code. Logs are kept on disk for the agent to grep with explicit patterns.

Modes:
  default                   Tutorial gate happy path. Asserts sandbox engaged + IPC works
                            and that all renderer raw-socket() canaries are denied
                            (the FD-passing broker is the only path to network).
  negative-fail-closed      Forces lower_token to fail; renderer must abort.
  negative-signature        Forces verify_binary to fail; broker must refuse spawn.
  negative-broker           Forces CIDRPolicy::is_allowed to return false; any renderer
                            request through the broker returns ST_DENIED_FORCE_FAIL.
                            The renderer launches but all NetSocket open calls fail
                            cleanly (legacy 'negative-network-filter' alias retained).
  crash-upload              Points the launcher's API at a local HTTP sink, SIGKILLs the
                            renderer after first frame, then quits the launcher via a
                            window close request. Asserts the crash log reaches the sink
                            (send_logs POST with the crash header) before the app exits.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, NoReturn, Optional

SCRIPT_DIR = Path(__file__).resolve().parent
GODOT_DIR = SCRIPT_DIR.parent
REPO_DIR = GODOT_DIR.parent
APP_DIR = REPO_DIR / "app"
BIN_DIR = GODOT_DIR / "bin"

# [VERIFY-FAIL] tag -> exit code. Keep stable: agents key off these.
FAIL_CATALOG: dict[str, int] = {
    "launcher_bin_missing": 10,
    "renderer_bin_missing": 10,
    "build_launcher": 11,
    "build_renderer": 12,
    "launcher_no_exit": 13,
    "renderer_never_started": 14,
    "renderer_no_ready": 15,
    "no_diag_block": 16,
    "diag_parse_failed": 17,
    "gate_not_entered": 18,
    "renderer_no_external_texture": 19,
    "renderer_errors": 20,
    "per_gate_dir_empty": 21,
    "per_gate_dir_missing": 22,
    "canary_user_dir_blocked": 23,
    "canary_sibling_gate_allowed": 24,
    "canary_pck_read_blocked": 25,
    "broker_policy_parse_failed": 26,
    "broker_renderer_integrity_mismatch": 27,
    "broker_token_lockdown_regression": 28,
    "negative_fail_closed_not_aborted": 29,
    "negative_fail_closed_no_attempt": 36,
    "negative_signature_renderer_started": 30,
    "negative_signature_no_gate_error": 37,
    "multi_gate_cycles_missing": 31,
    "main_thread_frozen": 32,
    "gate_no_first_frame": 33,
    "gate_not_responding": 34,
    "gate_error": 35,
    "canary_private_ip_reachable": 38,
    "canary_localhost_reachable": 39,
    "canary_public_ip_unreachable": 40,
    "negative_network_filter_not_aborted": 41,
    "canary_raw_socket_allowed": 42,
    "negative_broker_not_denied": 43,
    "canary_socketpair_allowed": 44,
    "canary_netlink_allowed": 45,
    "canary_userfaultfd_allowed": 46,
    "canary_proc_net_readable": 47,
    "canary_brokered_connect_denied": 48,
    "canary_brokered_connect_unblocked_in_force_fail": 49,
    "crash_upload_missing": 50,
    "crash_upload_malformed": 51,
    "crash_upload_kill_failed": 52,
}

MAX_TICK_GAP_MS = 500


def is_windows() -> bool:
    return sys.platform == "win32"


def is_macos() -> bool:
    return sys.platform == "darwin"


def host_arch() -> str:
    return platform.machine().lower() or "x86_64"


def default_launcher_bin() -> Path:
    if is_windows():
        return BIN_DIR / "godot.windows.editor.dev.x86_64.llvm.console.exe"
    if is_macos():
        return BIN_DIR / f"godot.macos.editor.dev.{host_arch()}"
    return BIN_DIR / "godot.linuxbsd.editor.dev.x86_64.llvm"


def default_renderer_bin() -> Path:
    if is_windows():
        return BIN_DIR / "godot.windows.template_debug.dev.renderer.x86_64.llvm.console.exe"
    if is_macos():
        return BIN_DIR / f"godot.macos.template_debug.dev.renderer.{host_arch()}"
    return BIN_DIR / "godot.linuxbsd.template_debug.dev.renderer.x86_64.llvm"


def default_results_dir() -> Path:
    if is_windows():
        return Path(os.environ.get("TEMP", "C:/Windows/Temp")) / "thegates-autotest"
    return Path(os.environ.get("TMPDIR", "/tmp")) / "thegates-autotest"


def user_data_root() -> Path:
    if is_windows():
        return Path(os.environ["APPDATA"]) / "Godot" / "app_userdata" / "TheGates"
    if is_macos():
        return Path.home() / "Library" / "Application Support" / "godot" / "app_userdata" / "TheGates"
    return Path.home() / ".local" / "share" / "godot" / "app_userdata" / "TheGates"


def emit_fail(reason: str, results_dir: Path) -> NoReturn:
    code = FAIL_CATALOG.get(reason.split(" ", 1)[0], 1)
    print(f"[VERIFY-FAIL] {reason}")
    print(f"  results={results_dir}")
    sys.exit(code)


def emit_pass(summary: str, results_dir: Path) -> NoReturn:
    print(f"[VERIFY-OK] {summary}")
    print(f"  results={results_dir}")
    sys.exit(0)


def kill_stale_processes() -> None:
    """Kill any lingering launcher/renderer from a previous run."""
    if is_windows():
        for image in (
            "godot.windows.template_debug.dev.renderer.x86_64.llvm.console.exe",
            "godot.windows.template_debug.dev.renderer.x86_64.llvm.exe",
            "godot.windows.editor.dev.x86_64.llvm.console.exe",
            "godot.windows.editor.dev.x86_64.llvm.exe",
        ):
            subprocess.run(
                ["taskkill", "/F", "/IM", image],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
        return

    plat = "macos" if is_macos() else "linuxbsd"
    for pattern in (
        f"godot.{plat}.template_debug.dev.renderer",
        f"godot.{plat}.editor.dev",
    ):
        subprocess.run(
            ["pkill", "-f", pattern],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )


def run_build(no_sandbox: bool, build_log: Path, results_dir: Path) -> None:
    extra: list[str] = ["--no-sandbox"] if no_sandbox else []
    build_log.write_text("")
    for profile, fail_reason in (("launcher", "build_launcher"), ("renderer", "build_renderer")):
        print(f"[BUILD] {profile} via build.py")
        with build_log.open("a") as f:
            proc = subprocess.run(
                [sys.executable, str(SCRIPT_DIR / "build.py"), profile, *extra],
                stdout=f,
                stderr=subprocess.STDOUT,
                check=False,
            )
        if proc.returncode != 0:
            emit_fail(fail_reason, results_dir)


def launch_with_timeout(
    launcher_bin: Path,
    args: list[str],
    stdout_path: Path,
    stderr_path: Path,
    timeout_sec: int,
    results_dir: Path,
) -> int:
    """Spawn launcher, redirect stdout/stderr, wait until exit or timeout."""
    with stdout_path.open("w") as out, stderr_path.open("w") as err:
        proc = subprocess.Popen(
            [str(launcher_bin), *args],
            stdout=out,
            stderr=err,
            cwd=str(GODOT_DIR),
        )

    deadline = time.monotonic() + timeout_sec
    while proc.poll() is None:
        if time.monotonic() >= deadline:
            print("[KILL] launcher did not exit, terminating")
            try:
                if is_windows():
                    proc.terminate()
                else:
                    proc.send_signal(signal.SIGTERM)
            except OSError:
                pass
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
            kill_stale_processes()
            emit_fail(f"launcher_no_exit pid={proc.pid}", results_dir)
        time.sleep(0.25)
    return proc.returncode


def find_renderer_log_for_run(logs_root: Path, since_epoch: float) -> Optional[Path]:
    """Most-recently-modified log.txt under logs_root, preferring mtime >= since."""
    if not logs_root.is_dir():
        return None
    candidates = [p for p in logs_root.rglob("log.txt") if p.is_file()]
    if not candidates:
        return None
    fresh = [p for p in candidates if p.stat().st_mtime >= since_epoch - 1.0]
    pool = fresh if fresh else candidates
    return max(pool, key=lambda p: p.stat().st_mtime)


def last_line_match(text: str, pattern: str) -> Optional[int]:
    """Return the 1-based line number of the LAST match, or None."""
    rx = re.compile(pattern)
    last = None
    for i, line in enumerate(text.splitlines(), start=1):
        if rx.search(line):
            last = i
    return last


def gate_url_to_folder(url: str) -> str:
    cleaned = re.sub(r"\?.*$", "", url)
    cleaned = re.sub(r"^https?://", "", cleaned)
    cleaned = re.sub(r"\.gate$", "", cleaned)
    return cleaned.replace(":", "_")


class UploadSink:
    """Local stand-in for the app API; records every POST body to disk."""

    def __init__(self, results_dir: Path):
        self.uploads: list[dict[str, Any]] = []
        self.lock = threading.Lock()
        sink = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length) if length else b""
                with sink.lock:
                    idx = len(sink.uploads)
                    upload_file = results_dir / f"upload_{idx:02d}.txt"
                    upload_file.write_bytes(body)
                    sink.uploads.append({"path": self.path, "bytes": len(body), "file": upload_file})
                self.send_response(200)
                self.end_headers()

            def do_GET(self) -> None:
                self.send_response(404)
                self.end_headers()

            def log_message(self, fmt: str, *log_args: Any) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def send_logs_uploads(self) -> list[dict[str, Any]]:
        with self.lock:
            return [u for u in self.uploads if u["path"].startswith("/api/send_logs")]


def kill_renderer_when_rendering(launcher_log: Path, budget_sec: float, state: dict[str, Any]) -> None:
    """Wait for the gate's first frame in the launcher log, then SIGKILL the renderer."""
    deadline = time.monotonic() + budget_sec
    pid = None
    while time.monotonic() < deadline:
        text = launcher_log.read_text(encoding="utf-8", errors="replace") if launcher_log.exists() else ""
        if "[AUTOTEST-FIRST-FRAME]" in text:
            pids = re.findall(r"spawned pid=(\d+)", text)
            if pids:
                pid = int(pids[-1])
            break
        time.sleep(0.5)
    if pid is None:
        state["error"] = "no renderer pid after first frame (or first frame never reached)"
        return
    try:
        if is_windows():
            subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True, check=False)
        else:
            os.kill(pid, signal.SIGKILL)
    except OSError as e:
        state["error"] = f"kill pid={pid} failed: {e}"
        return
    state["killed_pid"] = pid


def parse_diag_block(renderer_text: str, start_line: int) -> Optional[dict[str, Any]]:
    """Extract the last SANDBOX-DIAG-BEGIN/END JSON block after start_line."""
    lines = renderer_text.splitlines()
    begin = end = None
    for i, line in enumerate(lines, start=1):
        if "=== SANDBOX-DIAG-BEGIN ===" in line and i >= start_line:
            begin = i
        elif "=== SANDBOX-DIAG-END ===" in line and begin is not None and i > begin:
            end = i
    if begin is None or end is None or end <= begin:
        return None
    body = "\n".join(lines[begin : end - 1])
    try:
        parsed: dict[str, Any] = json.loads(body)
        return parsed
    except json.JSONDecodeError:
        return None


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="run-sandbox-test.py",
        description="Cross-platform sandbox verification harness for the renderer.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
exit codes: see FAIL_CATALOG in the source; 0 = [VERIFY-OK].

examples:
  python tools/run-sandbox-test.py
  python tools/run-sandbox-test.py negative-fail-closed
  python tools/run-sandbox-test.py negative-signature
  python tools/run-sandbox-test.py --build --gate-url https://thegates.io/worlds/world.gate
""",
    )
    parser.add_argument(
        "mode",
        nargs="?",
        default="default",
        choices=[
            "default",
            "negative-fail-closed",
            "negative-signature",
            "negative-broker",
            "negative-network-filter",
            "crash-upload",
        ],
        help="harness mode (default: default); negative-network-filter is a legacy alias for negative-broker",
    )
    parser.add_argument("--gate-url", default=None)
    parser.add_argument("--timeout", type=int, default=None, help="seconds the launcher runs before self-quit")
    parser.add_argument("--build", action="store_true", help="rebuild launcher + renderer first")
    parser.add_argument("--no-sandbox", action="store_true", help="combined with --build: pass tg_sandbox=no")
    parser.add_argument("--launcher-bin", default="")
    parser.add_argument("--renderer-bin", default="")
    parser.add_argument("--verbose", action="store_true", help="pass --verbose to launcher (drastic log growth)")
    parser.add_argument("--results-dir", default="")
    parser.add_argument(
        "--cycles", type=int, default=0, help="re-opens after the initial gate (cycles=2 means 3 spawns)"
    )
    parser.add_argument("--cycle-delay", type=float, default=5.0)
    args = parser.parse_args()

    if args.gate_url is None:
        # negative-broker needs a gate the renderer actually phones home from;
        # tutorial.gate is fully cached and never triggers the broker.
        if args.mode in ("negative-broker", "negative-network-filter"):
            args.gate_url = "https://thegates.io/worlds/world.gate"
        else:
            args.gate_url = "https://thegates.io/worlds/tutorial.gate"

    if args.timeout is None:
        # crash-upload needs first frame + the 10s heartbeat window + upload
        args.timeout = 40 if args.mode == "crash-upload" else 25

    launcher_bin = Path(args.launcher_bin) if args.launcher_bin else default_launcher_bin()
    renderer_bin = Path(args.renderer_bin) if args.renderer_bin else default_renderer_bin()
    results_dir = Path(args.results_dir) if args.results_dir else default_results_dir()
    results_dir.mkdir(parents=True, exist_ok=True)

    launcher_log = results_dir / "launcher.log"
    launcher_err = results_dir / "launcher.err"
    renderer_log_copy = results_dir / "renderer.log"
    build_log = results_dir / "build.log"
    verify_json = results_dir / "verify.json"
    for p in (launcher_log, launcher_err, renderer_log_copy, verify_json):
        p.unlink(missing_ok=True)

    # Pop on os.environ directly; dict.update on a copy would never unset.
    os.environ.pop("TG_SANDBOX_FORCE_FAIL", None)
    os.environ.pop("TG_SIGNATURE_FORCE_FAIL", None)
    os.environ.pop("TG_NETWORK_BROKER_FORCE_FAIL", None)
    os.environ.pop("TG_NETWORK_FILTER_FORCE_FAIL", None)
    if args.mode == "negative-fail-closed":
        os.environ["TG_SANDBOX_FORCE_FAIL"] = "1"
    elif args.mode == "negative-signature":
        os.environ["TG_SIGNATURE_FORCE_FAIL"] = "1"
    elif args.mode in ("negative-broker", "negative-network-filter"):
        os.environ["TG_NETWORK_BROKER_FORCE_FAIL"] = "1"

    if args.build:
        run_build(args.no_sandbox, build_log, results_dir)

    if not launcher_bin.is_file():
        emit_fail(f"launcher_bin_missing path={launcher_bin}", results_dir)
    if not renderer_bin.is_file():
        emit_fail(f"renderer_bin_missing path={renderer_bin}", results_dir)

    kill_stale_processes()
    time.sleep(0.2)

    logs_root = user_data_root() / "logs"
    logs_root.mkdir(parents=True, exist_ok=True)
    launch_start = time.time()

    launcher_args = [
        "--path",
        str(APP_DIR),
        "--",
        "--autotest",
        "--gate-url",
        args.gate_url,
        "--autotest-timeout",
        str(args.timeout),
    ]
    if args.cycles > 0:
        launcher_args += ["--autotest-cycles", str(args.cycles), "--autotest-cycle-delay", str(args.cycle_delay)]
    if args.verbose:
        launcher_args.append("--verbose")

    sink: Optional[UploadSink] = None
    kill_state: dict[str, Any] = {}
    if args.mode == "crash-upload":
        sink = UploadSink(results_dir)
        launcher_args += ["--api-url", f"http://127.0.0.1:{sink.port}", "--autotest-quit-close-request"]
        threading.Thread(
            target=kill_renderer_when_rendering,
            args=(launcher_log, args.timeout - 12, kill_state),
            daemon=True,
        ).start()

    print(f"[RUN] {launcher_bin} {' '.join(launcher_args)}")
    # Launcher autoloads (AnalyticsEvents, HTTPClientPool, Backend) drain
    # in-flight HTTP on shutdown; 25s grace covers that on slow networks.
    wait_budget = args.timeout + 25
    launcher_exit = launch_with_timeout(
        launcher_bin,
        launcher_args,
        launcher_log,
        launcher_err,
        wait_budget,
        results_dir,
    )

    if args.mode == "crash-upload":
        assert sink is not None
        if "killed_pid" not in kill_state:
            emit_fail(f"crash_upload_kill_failed {kill_state.get('error', 'unknown')}", results_dir)
        uploads = sink.send_logs_uploads()
        if not uploads:
            emit_fail(
                f"crash_upload_missing killed_pid={kill_state['killed_pid']} "
                f"posts_seen={len(sink.uploads)} (crash log never reached the API)",
                results_dir,
            )
        body = uploads[0]["file"].read_bytes()
        if b"=== TheGates renderer crash ===" not in body or b"reason:" not in body:
            emit_fail(f"crash_upload_malformed bytes={uploads[0]['bytes']} file={uploads[0]['file']}", results_dir)
        reason_match = re.search(rb"reason: (\S+)", body)
        reason = reason_match.group(1).decode() if reason_match else "?"
        emit_pass(
            f"crash-upload: killed_pid={kill_state['killed_pid']} uploads={len(uploads)} "
            f"bytes={uploads[0]['bytes']} reason={reason} launcher_exit={launcher_exit}",
            results_dir,
        )

    # negative-signature: broker must refuse to spawn -> no fresh renderer log
    # AND the launcher must surface the refusal as a user-visible gate_error.
    if args.mode == "negative-signature":
        ns_log = find_renderer_log_for_run(logs_root, launch_start)
        if ns_log and ns_log.stat().st_mtime >= launch_start - 1.0:
            emit_fail(f"negative_signature_renderer_started log={ns_log}", results_dir)
        launcher_text = launcher_log.read_text(encoding="utf-8", errors="replace") if launcher_log.exists() else ""
        if "[AUTOTEST-GATE-ERROR]" not in launcher_text:
            emit_fail("negative_signature_no_gate_error launcher_did_not_surface_refusal", results_dir)
        emit_pass(
            "negative-signature: broker refused to spawn and launcher surfaced gate_error as expected", results_dir
        )

    # negative-broker: TG_NETWORK_BROKER_FORCE_FAIL makes CIDRPolicy::is_allowed
    # return false unconditionally. Renderer launches (broker is in-process so
    # it starts) but every BrokeredNetSocket::connect_to_host returns
    # ERR_UNAUTHORIZED. The broker prints "[NETWORK-BROKER] DENY ..." to the
    # launcher's stdout (broker thread runs in the launcher process).
    if args.mode in ("negative-broker", "negative-network-filter"):
        launcher_text = launcher_log.read_text(encoding="utf-8", errors="replace") if launcher_log.exists() else ""
        if "[NETWORK-BROKER] DENY" not in launcher_text:
            emit_fail("negative_broker_not_denied no broker DENY line in launcher log", results_dir)
        emit_pass(
            "negative-broker: broker denied socket requests with TG_NETWORK_BROKER_FORCE_FAIL=1 as expected",
            results_dir,
        )

    renderer_log_file = find_renderer_log_for_run(logs_root, launch_start)
    if not renderer_log_file or not renderer_log_file.is_file():
        emit_fail("renderer_never_started no_log_file", results_dir)

    shutil.copyfile(renderer_log_file, renderer_log_copy)
    renderer_text = renderer_log_copy.read_text(encoding="utf-8", errors="replace")

    start_line = last_line_match(renderer_text, r"\[RENDERER-START\]")
    if start_line is None:
        emit_fail("renderer_never_started no_start_marker", results_dir)
    ready_line = last_line_match(renderer_text, r"\[RENDERER-READY\]")

    if args.mode == "negative-fail-closed":
        if ready_line is not None and ready_line > start_line:
            emit_fail("negative_fail_closed_not_aborted", results_dir)
        attempt_line = last_line_match(renderer_text, r"\[LOCKDOWN-ATTEMPT\]")
        if attempt_line is None or attempt_line < start_line:
            emit_fail("negative_fail_closed_no_attempt renderer_aborted_before_reaching_lower_token", results_dir)
        emit_pass(
            "negative-fail-closed: renderer reached lockdown attempt then aborted on forced lower_token failure as expected",
            results_dir,
        )

    if ready_line is None or ready_line < start_line:
        emit_fail("renderer_no_ready", results_dir)

    diag = parse_diag_block(renderer_text, start_line)
    if diag is None:
        emit_fail(f"no_diag_block see={renderer_log_copy}", results_dir)
    verify_json.write_text(json.dumps(diag, indent=2))

    # Gate health (launcher-side).
    launcher_text = launcher_log.read_text(encoding="utf-8", errors="replace") if launcher_log.exists() else ""
    gate_entered = len(re.findall(r"\[AUTOTEST-GATE-ENTERED\]", launcher_text))
    if gate_entered < 1:
        emit_fail("gate_not_entered", results_dir)

    expected_entered = args.cycles + 1
    if gate_entered < expected_entered:
        emit_fail(f"multi_gate_cycles_missing expected={expected_entered} got={gate_entered}", results_dir)

    first_frame_count = len(re.findall(r"\[AUTOTEST-FIRST-FRAME\]", launcher_text))
    if first_frame_count < expected_entered:
        emit_fail(
            f"gate_no_first_frame expected={expected_entered} entered={gate_entered} first_frame={first_frame_count}",
            results_dir,
        )

    if "[AUTOTEST-NOT-RESPONDING]" in launcher_text:
        nr_first = next(line for line in launcher_text.splitlines() if "[AUTOTEST-NOT-RESPONDING]" in line)
        nr_count = launcher_text.count("[AUTOTEST-NOT-RESPONDING]")
        emit_fail(f"gate_not_responding count={nr_count} first={nr_first[:140]}", results_dir)

    if "[AUTOTEST-GATE-ERROR]" in launcher_text:
        ge_first = next(line for line in launcher_text.splitlines() if "[AUTOTEST-GATE-ERROR]" in line)
        ge_count = launcher_text.count("[AUTOTEST-GATE-ERROR]")
        emit_fail(f"gate_error count={ge_count} first={ge_first[:140]}", results_dir)

    # Main-thread responsiveness: process_frame must keep firing during a gate
    # switch. Before verify_binary moved to a worker thread, SHA-256 hashing
    # blocked the main loop for 6s+ and the launcher froze on the old gate's
    # last frame. max_tick_gap > 500ms means a blocking call slipped back in.
    for line in launcher_text.splitlines():
        if "[AUTOTEST-GATE-ENTERED]" not in line or "max_tick_gap=" not in line:
            continue
        m = re.search(r"max_tick_gap=(\d+)", line)
        if m and int(m.group(1)) > MAX_TICK_GAP_MS:
            emit_fail(f"main_thread_frozen max_ms={MAX_TICK_GAP_MS} line={line[:140]}", results_dir)

    ext_line = last_line_match(renderer_text, r"TGExternalTexture")
    if ext_line is None or ext_line < start_line:
        emit_fail("renderer_no_external_texture", results_dir)

    post_ready = "\n".join(renderer_text.splitlines()[ready_line:])
    err_match = re.search(r"(ERROR:|FATAL|CRASH|Segmentation).*", post_ready)
    if err_match:
        emit_fail(f"renderer_errors first={err_match.group(0)[:80]}", results_dir)

    # Diag fields + canaries.
    def diag_get(*keys: str) -> str:
        node = diag
        for k in keys:
            if not isinstance(node, dict) or k not in node:
                return "?"
            node = node[k]
        return str(node)

    # Canaries are uniform {status, error?} dicts. Older string canaries used
    # to be flat strings; this helper handles both during transition (and stays
    # correct after the dict migration completes).
    def canary_status(name: str) -> str:
        return diag_get("canaries", name, "status")

    diag_integrity = diag_get("integrity")
    diag_pid = diag_get("pid")
    diag_build = diag_get("build")
    canary_file = canary_status("canary_file_write")
    canary_user = canary_status("canary_user_dir_write")
    canary_sibling = canary_status("canary_sibling_gate_write")
    canary_pck = canary_status("canary_pck_read")

    gate_folder = gate_url_to_folder(args.gate_url)
    per_gate_dir = user_data_root() / "gates_storage" / gate_folder
    per_gate_files = 0
    if per_gate_dir.is_dir():
        # is_file() can fail on Windows AF_UNIX socket files (WinError 1920);
        # skip those defensively instead of crashing the harness.
        for p in per_gate_dir.rglob("*"):
            try:
                if p.is_file():
                    per_gate_files += 1
            except OSError:
                pass
        if per_gate_files < 1:
            emit_fail(f"per_gate_dir_empty path={per_gate_dir} (renderer wrote nothing under its user://)", results_dir)
    else:
        emit_fail(
            f"per_gate_dir_missing path={per_gate_dir} (launcher never created the per-gate user dir)", results_dir
        )

    if canary_user != "allowed":
        emit_fail(
            f"canary_user_dir_blocked value={canary_user} (sandbox blocks FileAccess.WRITE at root of user://)",
            results_dir,
        )
    if canary_sibling != "blocked":
        emit_fail(f"canary_sibling_gate_allowed value={canary_sibling} (cross-gate isolation broken)", results_dir)
    if canary_pck not in ("allowed", "skipped_no_main_scene", "skipped_no_pck_path"):
        emit_fail(
            f"canary_pck_read_blocked value={canary_pck} (gate cannot load resources from .pck post-lockdown)",
            results_dir,
        )

    # Raw-socket canaries. The primary network-isolation enforcement is the
    # FD-passing broker on all platforms; the OS denies direct AF_INET as
    # defense-in-depth on top.
    #   Linux:   seccomp denies __NR_socket.
    #   macOS:   Seatbelt denies SYS_socket via (deny syscall-unix
    #            (syscall-number 97)) — inherited FDs from SCM_RIGHTS survive.
    #   Windows: AppContainer profile with no networking capabilities; WFP
    #            blocks every direct AF_INET connect() at ALE_AUTH_CONNECT.
    #            socket() itself still succeeds (USER_LIMITED + AFD DACL),
    #            so the canary measures connect() blocked rather than
    #            socket() denied — same observable outcome.
    canary_raw = canary_status("canary_raw_socket_denied")
    canary_priv = canary_status("canary_private_ip_blocked")
    canary_loop = canary_status("canary_localhost_blocked")
    canary_pub = canary_status("canary_public_ip_allowed")
    network_canaries_present = "?" not in (canary_raw, canary_priv, canary_loop, canary_pub)
    if network_canaries_present:
        if canary_raw != "blocked":
            emit_fail(
                f"canary_raw_socket_allowed value={canary_raw} "
                "(renderer can still open AF_INET sockets; sandbox not denying)",
                results_dir,
            )
        if canary_priv != "blocked":
            emit_fail(
                f"canary_private_ip_reachable value={canary_priv} (raw socket to RFC 1918 succeeded; broker bypass)",
                results_dir,
            )
        if canary_loop != "blocked":
            emit_fail(
                f"canary_localhost_reachable value={canary_loop} (raw socket to 127.0.0.1 succeeded; broker bypass)",
                results_dir,
            )
        if canary_pub != "blocked":
            emit_fail(
                f"canary_public_ip_unreachable value={canary_pub} "
                "(raw socket to 1.1.1.1 succeeded; sandbox not denying socket())",
                results_dir,
            )

    # Linux-only loophole canaries: socketpair, AF_NETLINK, userfaultfd,
    # /proc/self/net. All must be "blocked" in default mode. Other platforms
    # report "?" so the block is a no-op there.
    canary_sp = canary_status("canary_socketpair_denied")
    canary_nl = canary_status("canary_netlink_denied")
    canary_uffd = canary_status("canary_userfaultfd_denied")
    canary_procnet = canary_status("canary_proc_net_blocked")
    if canary_sp != "?" and canary_sp != "blocked":
        emit_fail(
            f"canary_socketpair_allowed value={canary_sp} "
            "(renderer can create AF_UNIX socketpair; seccomp not denying __NR_socketpair)",
            results_dir,
        )
    if canary_nl != "?" and canary_nl != "blocked":
        emit_fail(
            f"canary_netlink_allowed value={canary_nl} "
            "(renderer can open AF_NETLINK socket; seccomp not denying __NR_socket)",
            results_dir,
        )
    if canary_uffd != "?" and canary_uffd != "blocked":
        emit_fail(
            f"canary_userfaultfd_allowed value={canary_uffd} "
            "(renderer can call __NR_userfaultfd; remove from seccomp allowlist)",
            results_dir,
        )
    if canary_procnet != "?" and canary_procnet != "blocked":
        emit_fail(
            f"canary_proc_net_readable value={canary_procnet} "
            "(renderer can read /proc/self/net/tcp; landlock granting /proc/self as a tree)",
            results_dir,
        )

    # Brokered-connect canary: in default mode the broker honors the request
    # and returns a connected FD ("allowed"). The negative-broker branch
    # above already exited before this point, so reaching here means default
    # mode and the canary must succeed.
    canary_broker = canary_status("canary_brokered_connect")
    if canary_broker != "?" and canary_broker != "allowed":
        emit_fail(
            f"canary_brokered_connect_denied value={canary_broker} "
            "(broker refused a public-IP request in default mode; check broker FD inheritance)",
            results_dir,
        )

    # Broker / renderer cross-check via broker_policy.json (Windows writes it;
    # other platforms skip silently).
    broker_xcheck = "skipped"
    broker_policy_path = renderer_log_file.parent / "broker_policy.json"
    if broker_policy_path.is_file():
        shutil.copyfile(broker_policy_path, results_dir / "broker_policy.json")
        try:
            broker_policy = json.loads(broker_policy_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            emit_fail(f"broker_policy_parse_failed see={broker_policy_path}", results_dir)
        broker_integrity = str(broker_policy.get("integrity_target", "")).lower()
        if broker_integrity and diag_integrity != "?" and broker_integrity != diag_integrity:
            emit_fail(
                f"broker_renderer_integrity_mismatch broker={broker_integrity} renderer={diag_integrity}", results_dir
            )
        if broker_policy.get("token_lockdown") == "USER_LOCKDOWN":
            emit_fail("broker_token_lockdown_regression value=USER_LOCKDOWN", results_dir)
        broker_xcheck = "ok"

    emit_pass(
        f"integrity={diag_integrity} renderer_pid={diag_pid} "
        f"gates_entered={gate_entered} first_frames={first_frame_count} "
        f"canary_file={canary_file} canary_user_dir={canary_user} "
        f"canary_sibling={canary_sibling} canary_pck={canary_pck} "
        f"per_gate_files={per_gate_files} broker_xcheck={broker_xcheck} "
        f"build={diag_build} launcher_exit={launcher_exit}",
        results_dir,
    )


if __name__ == "__main__":
    main()
