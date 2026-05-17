#!/usr/bin/env bash
# Autonomous launcher+renderer sandbox verification loop (Linux).
#
# Mirror of tools/run-sandbox-test.ps1: same exit codes, same [VERIFY-FAIL]
# tags, same SANDBOX-DIAG-BEGIN/END parsing, same modes (default,
# negative-fail-closed, negative-signature). See the .ps1 docstring for the
# full catalog.

set -u
set -o pipefail

usage() {
    cat <<'EOF'
Usage: run-sandbox-test.sh [options]

Options:
  --gate-url URL        Gate to load (default https://thegates.io/worlds/tutorial.gate)
  --timeout SEC         Launcher run budget before self-quit (default 25)
  --build               Rebuild launcher + renderer first
  --no-sandbox          Combined with --build: pass tg_sandbox=no (faster iter)
  --launcher-bin PATH   Override launcher binary
  --renderer-bin PATH   Override renderer binary (diagnostic only; launcher
                        resolves via app/resources/renderer_executable.tres)
  --verbose             Pass --verbose to launcher
  --results-dir PATH    Where to write launcher.log/launcher.err/verify.json
  --mode MODE           default | negative-fail-closed | negative-signature
  --cycles N            Re-opens after the initial gate (cycles=2 -> 3 total)
  --cycle-delay SEC     Delay between cycle re-opens (default 5.0)
  --help                Show this help
EOF
}

GATE_URL="https://thegates.io/worlds/tutorial.gate"
TIMEOUT=25
BUILD=0
NO_SANDBOX=0
LAUNCHER_BIN=""
RENDERER_BIN=""
VERBOSE_LOGS=0
RESULTS_DIR=""
MODE="default"
CYCLES=0
CYCLE_DELAY="5.0"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --gate-url)     GATE_URL="$2"; shift 2 ;;
        --timeout)      TIMEOUT="$2"; shift 2 ;;
        --build)        BUILD=1; shift ;;
        --no-sandbox)   NO_SANDBOX=1; shift ;;
        --launcher-bin) LAUNCHER_BIN="$2"; shift 2 ;;
        --renderer-bin) RENDERER_BIN="$2"; shift 2 ;;
        --verbose)      VERBOSE_LOGS=1; shift ;;
        --results-dir)  RESULTS_DIR="$2"; shift 2 ;;
        --mode)         MODE="$2"; shift 2 ;;
        --cycles)       CYCLES="$2"; shift 2 ;;
        --cycle-delay)  CYCLE_DELAY="$2"; shift 2 ;;
        --help|-h)      usage; exit 0 ;;
        *)              echo "unknown arg: $1" >&2; usage; exit 2 ;;
    esac
done

case "$MODE" in
    default|negative-fail-closed|negative-signature) ;;
    *) echo "invalid --mode: $MODE" >&2; exit 2 ;;
esac

# Negative modes flip env vars the sandbox code reads at runtime to fail-closed.
unset TG_SANDBOX_FORCE_FAIL TG_SIGNATURE_FORCE_FAIL || true
if [[ "$MODE" == "negative-fail-closed" ]]; then
    export TG_SANDBOX_FORCE_FAIL=1
elif [[ "$MODE" == "negative-signature" ]]; then
    export TG_SIGNATURE_FORCE_FAIL=1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GODOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$GODOT_DIR/.." && pwd)"
APP_DIR="$REPO_DIR/app"
BIN_DIR="$GODOT_DIR/bin"

if [[ -z "$LAUNCHER_BIN" ]]; then
    LAUNCHER_BIN="$BIN_DIR/godot.linuxbsd.editor.dev.x86_64.llvm"
fi
if [[ -z "$RENDERER_BIN" ]]; then
    RENDERER_BIN="$BIN_DIR/godot.linuxbsd.template_debug.dev.renderer.x86_64.llvm"
fi
if [[ -z "$RESULTS_DIR" ]]; then
    RESULTS_DIR="${TMPDIR:-/tmp}/thegates-autotest"
fi

mkdir -p "$RESULTS_DIR"
LAUNCHER_LOG="$RESULTS_DIR/launcher.log"
LAUNCHER_ERR="$RESULTS_DIR/launcher.err"
RENDERER_LOG_COPY="$RESULTS_DIR/renderer.log"
BUILD_LOG="$RESULTS_DIR/build.log"
VERIFY_JSON="$RESULTS_DIR/verify.json"

rm -f -- "$LAUNCHER_LOG" "$LAUNCHER_ERR" "$RENDERER_LOG_COPY" "$VERIFY_JSON"

emit_fail() {
    local reason="$1" code="$2"
    echo "[VERIFY-FAIL] $reason"
    echo "  results=$RESULTS_DIR"
    exit "$code"
}

emit_pass() {
    local summary="$1"
    echo "[VERIFY-OK] $summary"
    echo "  results=$RESULTS_DIR"
    exit 0
}

if [[ "$BUILD" -eq 1 ]]; then
    BUILD_EXTRA=()
    [[ "$NO_SANDBOX" -eq 1 ]] && BUILD_EXTRA+=("--no-sandbox")

    echo "[BUILD] launcher via build.py"
    if ! python3 "$SCRIPT_DIR/build.py" launcher "${BUILD_EXTRA[@]}" 2>&1 | tee "$BUILD_LOG"; then
        emit_fail "build_launcher" 11
    fi
    if [[ "${PIPESTATUS[0]}" -ne 0 ]]; then
        emit_fail "build_launcher" 11
    fi

    echo "[BUILD] renderer via build.py"
    if ! python3 "$SCRIPT_DIR/build.py" renderer "${BUILD_EXTRA[@]}" 2>&1 | tee -a "$BUILD_LOG"; then
        emit_fail "build_renderer" 12
    fi
    if [[ "${PIPESTATUS[0]}" -ne 0 ]]; then
        emit_fail "build_renderer" 12
    fi
fi

[[ -x "$LAUNCHER_BIN" ]] || emit_fail "launcher_bin_missing path=$LAUNCHER_BIN" 10
[[ -x "$RENDERER_BIN" ]] || emit_fail "renderer_bin_missing path=$RENDERER_BIN" 10

# Kill stale runners.
pkill -f 'godot.linuxbsd.template_debug.dev.renderer' >/dev/null 2>&1 || true
pkill -f 'godot.linuxbsd.editor.dev' >/dev/null 2>&1 || true
sleep 0.2

USER_DATA_ROOT="${HOME}/.local/share/godot/app_userdata/TheGates"
LOGS_ROOT="$USER_DATA_ROOT/logs"
mkdir -p "$LOGS_ROOT"
LAUNCH_START_EPOCH=$(date +%s)

# Launch in the background so we can apply a hard timeout.
LAUNCHER_ARGS=(
    --path "$APP_DIR"
    --
    --autotest
    --gate-url "$GATE_URL"
    --autotest-timeout "$TIMEOUT"
)
if [[ "$CYCLES" -gt 0 ]]; then
    LAUNCHER_ARGS+=(--autotest-cycles "$CYCLES" --autotest-cycle-delay "$CYCLE_DELAY")
fi
[[ "$VERBOSE_LOGS" -eq 1 ]] && LAUNCHER_ARGS+=(--verbose)

echo "[RUN] $LAUNCHER_BIN ${LAUNCHER_ARGS[*]}"
"$LAUNCHER_BIN" "${LAUNCHER_ARGS[@]}" >"$LAUNCHER_LOG" 2>"$LAUNCHER_ERR" &
LAUNCHER_PID=$!

# Generous grace period after autotest quit: launcher autoloads
# (AnalyticsEvents, HTTPClientPool, Backend) drain in-flight HTTP on shutdown,
# which can take a chunk of seconds when the network is slow.
WAIT_BUDGET=$((TIMEOUT + 25))
WAITED=0
LAUNCHER_EXIT="?"
while kill -0 "$LAUNCHER_PID" 2>/dev/null; do
    if [[ "$WAITED" -ge "$WAIT_BUDGET" ]]; then
        echo "[KILL] launcher did not exit, terminating"
        kill "$LAUNCHER_PID" 2>/dev/null || true
        sleep 1
        kill -9 "$LAUNCHER_PID" 2>/dev/null || true
        pkill -f 'godot.linuxbsd.template_debug.dev.renderer' >/dev/null 2>&1 || true
        emit_fail "launcher_no_exit pid=$LAUNCHER_PID" 13
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done
wait "$LAUNCHER_PID" 2>/dev/null || true
LAUNCHER_EXIT=$?

# negative-signature: broker must refuse to spawn -> no renderer log produced
# after $LAUNCH_START_EPOCH.
find_recent_renderer_log() {
    local since="$1"
    find "$LOGS_ROOT" -name log.txt -type f -newermt "@$since" 2>/dev/null \
        | head -n 1
}

if [[ "$MODE" == "negative-signature" ]]; then
    NS_LOG="$(find_recent_renderer_log "$LAUNCH_START_EPOCH" || true)"
    if [[ -n "$NS_LOG" ]]; then
        emit_fail "negative_signature_renderer_started log=$NS_LOG" 30
    fi
    emit_pass "negative-signature: broker refused to spawn on forced verify_binary failure as expected"
fi

# Locate this run's renderer log (newest log.txt with mtime >= launch start).
RENDERER_LOG_FILE=""
if [[ -d "$LOGS_ROOT" ]]; then
    RENDERER_LOG_FILE="$(find "$LOGS_ROOT" -name log.txt -type f -newermt "@$LAUNCH_START_EPOCH" \
        -printf '%T@ %p\n' 2>/dev/null \
        | sort -rn | head -n 1 | cut -d' ' -f2-)"
fi
if [[ -z "$RENDERER_LOG_FILE" ]]; then
    RENDERER_LOG_FILE="$(find "$LOGS_ROOT" -name log.txt -type f \
        -printf '%T@ %p\n' 2>/dev/null \
        | sort -rn | head -n 1 | cut -d' ' -f2-)"
fi
if [[ -z "$RENDERER_LOG_FILE" || ! -f "$RENDERER_LOG_FILE" ]]; then
    emit_fail "renderer_never_started no_log_file" 14
fi

cp -f "$RENDERER_LOG_FILE" "$RENDERER_LOG_COPY"

# Use the LAST occurrence of each marker (log file accumulates across runs).
last_line_match() {
    grep -n -- "$1" "$RENDERER_LOG_COPY" | tail -n 1 | cut -d: -f1
}

START_LINE="$(last_line_match '\[RENDERER-START\]' || true)"
if [[ -z "$START_LINE" ]]; then
    emit_fail "renderer_never_started no_start_marker" 14
fi
READY_LINE="$(last_line_match '\[RENDERER-READY\]' || true)"

if [[ "$MODE" == "negative-fail-closed" ]]; then
    if [[ -n "$READY_LINE" && "$READY_LINE" -gt "$START_LINE" ]]; then
        emit_fail "negative_fail_closed_not_aborted" 29
    fi
    emit_pass "negative-fail-closed: renderer aborted on forced lower_token failure as expected"
fi

if [[ -z "$READY_LINE" || "$READY_LINE" -lt "$START_LINE" ]]; then
    emit_fail "renderer_no_ready" 15
fi

DIAG_BEGIN_LINE="$(grep -n -- '=== SANDBOX-DIAG-BEGIN ===' "$RENDERER_LOG_COPY" | tail -n 1 | cut -d: -f1)"
DIAG_END_LINE="$(grep -n -- '=== SANDBOX-DIAG-END ===' "$RENDERER_LOG_COPY" | tail -n 1 | cut -d: -f1)"
if [[ -z "$DIAG_BEGIN_LINE" || -z "$DIAG_END_LINE" \
      || "$DIAG_END_LINE" -le "$DIAG_BEGIN_LINE" \
      || "$DIAG_BEGIN_LINE" -lt "$START_LINE" ]]; then
    emit_fail "no_diag_block" 16
fi

DIAG_JSON_BODY="$(sed -n "$((DIAG_BEGIN_LINE + 1)),$((DIAG_END_LINE - 1))p" "$RENDERER_LOG_COPY")"
if ! echo "$DIAG_JSON_BODY" | jq -e . >"$VERIFY_JSON" 2>/dev/null; then
    echo "$DIAG_JSON_BODY" >"$VERIFY_JSON"
    emit_fail "diag_parse_failed see=$VERIFY_JSON" 17
fi

# Gate health checks (launcher side).
GATE_ENTERED_COUNT="$(grep -c -- '\[AUTOTEST-GATE-ENTERED\]' "$LAUNCHER_LOG" || true)"
GATE_ENTERED_COUNT="${GATE_ENTERED_COUNT:-0}"
if [[ "$GATE_ENTERED_COUNT" -lt 1 ]]; then
    emit_fail "gate_not_entered" 18
fi

EXPECTED_ENTERED=$((CYCLES + 1))
if [[ "$GATE_ENTERED_COUNT" -lt "$EXPECTED_ENTERED" ]]; then
    emit_fail "multi_gate_cycles_missing expected=$EXPECTED_ENTERED got=$GATE_ENTERED_COUNT" 31
fi

# External-texture import path reached.
EXT_LINE="$(grep -n -- 'TGExternalTexture' "$RENDERER_LOG_COPY" | tail -n 1 | cut -d: -f1)"
if [[ -z "$EXT_LINE" || "$EXT_LINE" -lt "$START_LINE" ]]; then
    emit_fail "renderer_no_external_texture" 19
fi

# Errors AFTER READY are real (anything before is loader noise).
POST_READY_TAIL="$(sed -n "$((READY_LINE + 1)),\$p" "$RENDERER_LOG_COPY")"
FIRST_ERR="$(echo "$POST_READY_TAIL" | grep -E 'ERROR:|FATAL|CRASH|Segmentation' | head -n 1 || true)"
if [[ -n "$FIRST_ERR" ]]; then
    emit_fail "renderer_errors first=${FIRST_ERR:0:80}" 20
fi

diag_get() { jq -r "$1 // empty" "$VERIFY_JSON" 2>/dev/null; }

DIAG_INTEGRITY="$(diag_get '.integrity')"
DIAG_PID="$(diag_get '.pid')"
DIAG_BUILD="$(diag_get '.build')"
DIAG_CANARY_FILE="$(diag_get '.canaries.canary_file_write')"
DIAG_CANARY_USER="$(diag_get '.canaries.canary_user_dir_write')"
DIAG_CANARY_SIBLING="$(diag_get '.canaries.canary_sibling_gate_write')"
DIAG_CANARY_PCK="$(diag_get '.canaries.canary_pck_read')"
[[ -z "$DIAG_INTEGRITY"     ]] && DIAG_INTEGRITY="?"
[[ -z "$DIAG_PID"           ]] && DIAG_PID="?"
[[ -z "$DIAG_BUILD"         ]] && DIAG_BUILD="?"
[[ -z "$DIAG_CANARY_FILE"   ]] && DIAG_CANARY_FILE="?"
[[ -z "$DIAG_CANARY_USER"   ]] && DIAG_CANARY_USER="?"
[[ -z "$DIAG_CANARY_SIBLING" ]] && DIAG_CANARY_SIBLING="?"
[[ -z "$DIAG_CANARY_PCK"    ]] && DIAG_CANARY_PCK="?"

# Per-gate dir presence + non-empty.
GATE_FOLDER="$(echo "$GATE_URL" | sed -E 's/\?.*//' | sed -E 's#^https?://##' | sed -E 's/\.gate$//' | tr ':' '_')"
PER_GATE_DIR="$USER_DATA_ROOT/gates_storage/$GATE_FOLDER"
if [[ -d "$PER_GATE_DIR" ]]; then
    PER_GATE_FILES="$(find "$PER_GATE_DIR" -type f 2>/dev/null | wc -l)"
    if [[ "$PER_GATE_FILES" -lt 1 ]]; then
        emit_fail "per_gate_dir_empty path=$PER_GATE_DIR (renderer wrote nothing under its user://)" 21
    fi
else
    emit_fail "per_gate_dir_missing path=$PER_GATE_DIR (launcher never created the per-gate user dir)" 22
fi

if [[ "$DIAG_CANARY_USER" != "allowed" ]]; then
    emit_fail "canary_user_dir_blocked value=$DIAG_CANARY_USER (sandbox blocks FileAccess.WRITE at root of user://)" 23
fi
if [[ "$DIAG_CANARY_SIBLING" != "blocked" ]]; then
    emit_fail "canary_sibling_gate_allowed value=$DIAG_CANARY_SIBLING (cross-gate isolation broken)" 24
fi
if [[ "$DIAG_CANARY_PCK" != "allowed" && "$DIAG_CANARY_PCK" != "skipped_no_main_scene" \
      && "$DIAG_CANARY_PCK" != "skipped_no_pck_path" ]]; then
    emit_fail "canary_pck_read_blocked value=$DIAG_CANARY_PCK (gate cannot load resources from .pck post-lockdown)" 25
fi

# Optional broker / renderer cross-check.
BROKER_POLICY_PATH="$(dirname "$RENDERER_LOG_FILE")/broker_policy.json"
BROKER_XCHECK="skipped"
if [[ -f "$BROKER_POLICY_PATH" ]]; then
    cp -f "$BROKER_POLICY_PATH" "$RESULTS_DIR/broker_policy.json"
    if ! jq -e . "$BROKER_POLICY_PATH" >/dev/null 2>&1; then
        emit_fail "broker_policy_parse_failed see=$BROKER_POLICY_PATH" 26
    fi
    BROKER_INTEGRITY="$(jq -r '.integrity_target // empty' "$BROKER_POLICY_PATH" | tr '[:upper:]' '[:lower:]')"
    if [[ -n "$BROKER_INTEGRITY" && "$BROKER_INTEGRITY" != "?" \
          && -n "$DIAG_INTEGRITY" && "$DIAG_INTEGRITY" != "?" \
          && "$BROKER_INTEGRITY" != "$DIAG_INTEGRITY" ]]; then
        emit_fail "broker_renderer_integrity_mismatch broker=$BROKER_INTEGRITY renderer=$DIAG_INTEGRITY" 27
    fi
    BROKER_TOKEN_LOCKDOWN="$(jq -r '.token_lockdown // empty' "$BROKER_POLICY_PATH")"
    if [[ "$BROKER_TOKEN_LOCKDOWN" == "USER_LOCKDOWN" ]]; then
        emit_fail "broker_token_lockdown_regression value=USER_LOCKDOWN" 28
    fi
    BROKER_XCHECK="ok"
fi

emit_pass "integrity=$DIAG_INTEGRITY renderer_pid=$DIAG_PID canary_file=$DIAG_CANARY_FILE canary_user_dir=$DIAG_CANARY_USER canary_sibling=$DIAG_CANARY_SIBLING canary_pck=$DIAG_CANARY_PCK per_gate_files=${PER_GATE_FILES:-?} broker_xcheck=$BROKER_XCHECK build=$DIAG_BUILD launcher_exit=$LAUNCHER_EXIT"
