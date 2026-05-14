# Instructions for an agent driving the sandboxing work

This document is for an agent (Claude Code, Codex, similar) that has been delegated the sandbox port — typically the Firefox-style vendoring path in [[Vendoring Option]]. The harness for "write → build → run → verify" is built and verified to work on the existing 4.5 renderer. Follow this contract and you can iterate autonomously without a human in the loop.

## Read first, in order

1. [[Overview]] — what the problem is.
2. [[Vendoring Option]] — the path you're implementing (unless told otherwise).
3. [[Reference Material]] — every primary source, bug number, and file path you might need to verify a decision.
4. This document.

After those four, you understand the problem space. Don't read the other docs unless you hit something specific.

## The harness — what is built

- `godot/modules/the_gates/sandbox_diagnostics.{h,cpp}` — a small C++ helper that gathers token integrity, mitigation flags, restricted-SID count, alternate-desktop name, and two canary operations (file write into `%USERPROFILE%`, registry write into `HKCU\Software`). It always runs, regardless of whether the sandbox actually engaged. Output is a stable JSON block bracketed by `=== SANDBOX-DIAG-BEGIN ===` / `=== SANDBOX-DIAG-END ===` markers.
- `godot/main/main.cpp` calls `SandboxDiagnostics::dump()` at the end of the renderer's `Main::start`, after the sandbox lockdown block. Also emits `[RENDERER-START]` near the top and `[RENDERER-READY]` after the diag block.
- `app/scripts/autotest.gd` parses `--autotest`, `--gate-url <URL>`, `--autotest-timeout <s>` from user args. When enabled, it calls `Navigation.open(url)` after one frame, schedules a quit timer, and emits `[AUTOTEST-START]`, `[AUTOTEST-OPEN]`, `[AUTOTEST-GATE-ENTERED]`, `[AUTOTEST-TIMEOUT]`, `[AUTOTEST-EXIT]` to launcher stdout.
- `app/scripts/app.gd` preloads `autotest.gd` and triggers it in `_ready`.
- `godot/tools/run-sandbox-test.ps1` — the one-shot runner. Boots launcher, waits, parses logs, emits one `[VERIFY-OK]` or `[VERIFY-FAIL] <reason>` line plus the diag JSON in `$ResultsDir/verify.json`.

## The one command you run each iteration

```powershell
powershell.exe -ExecutionPolicy Bypass -File "C:\Users\Nordup\Documents\Projects\thegates\godot\tools\run-sandbox-test.ps1"
```

Default behavior:
- Loads `https://thegates.io/worlds/tutorial.gate`
- 25-second autotest timeout
- Reuses existing binaries (no rebuild)
- Writes results to `%TEMP%\thegates-autotest\`

Useful switches:
- `-Build` — rebuild launcher + renderer first
- `-SandboxBuild` — pass `tg_sandbox=yes` to scons (combine with `-Build`)
- `-Timeout <s>` — change the in-launcher autotest timeout
- `-GateUrl <url>` — different gate
- `-VerboseLogs` — pass `--verbose` everywhere (huge logs; only for deep debug)

Output is always two or three lines:

```
[RUN] <launcher command verbatim>
[VERIFY-OK] integrity=<level> renderer_pid=<pid> canary_file=<state> build=<flag>
  results=<temp dir>
```

or

```
[RUN] <launcher command verbatim>
[VERIFY-FAIL] <reason> [extras]
  results=<temp dir>
```

Three lines max. The agent reads stdout via Bash and decides next step from the second line's tag.

## Failure-mode catalog

When you see `[VERIFY-FAIL] <reason>`, here's what each reason means and where to look:

| Reason | Meaning | First place to look |
|---|---|---|
| `build_launcher` | scons of launcher exited non-zero | `$ResultsDir\build.log` last 100 lines |
| `build_renderer` | scons of renderer exited non-zero | `$ResultsDir\build.log` last 100 lines |
| `launcher_bin_missing` / `renderer_bin_missing` | One of the binaries doesn't exist | Run with `-Build` |
| `launcher_no_exit` | Launcher hung past timeout + 15s buffer | `$ResultsDir\launcher.log` tail; usually the autotest timer didn't fire (script parse error in autotest.gd) |
| `renderer_never_started` | No renderer log found or `[RENDERER-START]` missing | Launcher log: did renderer subprocess spawn? |
| `renderer_no_ready` | `[RENDERER-START]` seen but `[RENDERER-READY]` missing | Renderer log around `[RENDERER-START]`; likely a crash during sandbox setup or external-texture import |
| `no_diag_block` | `[RENDERER-READY]` seen but no diag block | Renderer log; means `SandboxDiagnostics::dump()` crashed before completing |
| `diag_parse_failed` | Block present but invalid JSON | `$ResultsDir\verify.json` raw |
| `gate_not_entered` | `[AUTOTEST-GATE-ENTERED]` never fired | Launcher log; gate failed to load (network, parse error in pack) |
| `renderer_no_external_texture` | Renderer didn't reach the external-texture phase | Renderer log: where did it stop? |
| `renderer_errors` | `ERROR:`/`FATAL`/`CRASH` line after `[RENDERER-READY]` | Reason text contains the first 80 chars of the offending line |

## Reading the diag JSON

After a success or `no_diag_block`-adjacent failure, the diag JSON is parsed into `$ResultsDir\verify.json`. Schema:

```json
{
  "platform": "windows",
  "pid": 12345,
  "build": "tg_sandbox=yes|no",
  "integrity_sid": "S-1-16-0",
  "integrity": "untrusted|low|medium|...",
  "restricted_sid_count": 0,
  "mitigations": {
    "dep_enable": true,
    "aslr_bottom_up": true,
    "aslr_force_relocate": false,
    "dynamic_code_prohibited": false,
    "win32k_disabled": false,
    "strict_handle_checks": false,
    "cfg_enabled": false,
    ...
  },
  "alt_desktop": "Default",
  "canaries": {
    "canary_file_write": "blocked|allowed",
    "canary_reg_write": "blocked|allowed"
  }
}
```

What you want to see when the sandbox is engaged correctly:

- `"build": "tg_sandbox=yes"`
- `"integrity": "untrusted"`
- `"restricted_sid_count": 1` (just the null SID)
- `"win32k_disabled": true` (once you've added mitigations)
- `"canary_file_write": "blocked"`
- `"canary_reg_write": "blocked"`
- `"alt_desktop"`: something other than `"Default"`

Anything else is a regression. The agent's success condition is "all of the above" for the renderer process under a real gate load.

## Context-budget rules

This is the single most important section. Without these, the loop chokes on log noise within a few iterations.

1. **Never `cat`/`Get-Content` a log file in full.** Always use `Grep` with patterns, or `Read` with explicit offsets/limits.
2. **For the renderer log, the relevant section is between the LAST `[RENDERER-START]` and the LAST `[RENDERER-READY]`.** Older content is from previous runs. The runner script already extracts the relevant slice into `verify.json` for parsed data; only read the raw log when you need surrounding context for an error.
3. **For the launcher log, the only lines that matter are `[AUTOTEST-*]` lines and any error lines.** Grep with pattern `\[AUTOTEST-|ERROR:|FATAL`.
4. **Don't run with `-VerboseLogs` unless investigating something specific.** It explodes the log size 5-10×.
5. **The runner emits exit codes** (10-20 range). When using Bash with the runner, you can also check `$LASTEXITCODE` instead of regexing the output.

## How to read the output files

Three files matter. They live in `$ResultsDir` (default `%TEMP%\thegates-autotest\`):

| File | What's in it | When to read |
|---|---|---|
| `verify.json` | The parsed `SANDBOX-DIAG` JSON block (`pid`, `integrity`, `mitigations`, `canaries`, etc.) | First. Always. Smallest payload with the most signal. Use `Read` directly — it's already trimmed. |
| `launcher.log` | The editor process's stdout — `[AUTOTEST-*]` markers, gate-loading lifecycle, HTTP, analytics | When `verify.json` is OK but you want to confirm gate lifecycle, or when a `gate_not_entered` failure points here. |
| `launcher.err` | The editor process's stderr — `SCRIPT ERROR`, parse errors, Vulkan warnings | When the launcher crashes or autotest.gd has a parse error (you'll see `Failed to load script "res://scripts/app.gd"` here). |
| `renderer.log` | Copy of the renderer subprocess's log file from `%APPDATA%\Roaming\Godot\app_userdata\TheGates\logs\<host>\<path>\log.txt` | When the renderer crashes or you need post-`[RENDERER-READY]` context. |
| `build.log` | scons stdout+stderr from `-Build` runs | Build failures only. |

**The four read recipes you'll use 95% of the time:**

```
# 1. After [VERIFY-OK], inspect the diag fields directly.
Read("C:\\Users\\Nordup\\AppData\\Local\\Temp\\thegates-autotest\\verify.json")

# 2. After [VERIFY-FAIL] renderer_errors, find the first error line and ~10 around it.
Grep(path="C:\\Users\\Nordup\\AppData\\Local\\Temp\\thegates-autotest\\renderer.log",
     pattern="ERROR:|FATAL|CRASH|Segmentation",
     output_mode="content", -C=5, head_limit=20)

# 3. After [VERIFY-FAIL] renderer_no_ready, find the last segment of the
#    renderer log (from the most recent [RENDERER-START] onwards).
Grep(path=".../renderer.log", pattern="\\[RENDERER-START\\]",
     output_mode="content", -n=true)
# Note the line number of the LAST match, then:
Read(file_path=".../renderer.log", offset=<that line number>, limit=200)

# 4. After [VERIFY-FAIL] gate_not_entered, scan launcher autotest markers.
Grep(path=".../launcher.log", pattern="\\[AUTOTEST-|ERROR|Cannot|Failed",
     output_mode="content", head_limit=30)
```

**Three rules for grep on these logs:**

- The renderer log has **ANSI color escape codes** in it (the launcher's Debug.logclr lines come through with `[38;2;...m` etc.). When grepping for log text, allow for these — match on the bare keyword (`External texture created`) rather than expecting clean output.
- The launcher log **mixes engine stdout with the launcher's GDScript prints**. The `[AUTOTEST-*]` and `[RENDERER-*]` tags are deliberately ASCII-safe so they're greppable without color stripping.
- The renderer log path embeds the gate URL (`thegates.io/worlds/tutorial/log.txt`). If you change `-GateUrl`, the path changes too. **Use `renderer.log` in `$ResultsDir`** — the runner copies the correct one into a stable location for you.

**Reading the full source-of-truth log (only if the copy is corrupt):**

```
# The renderer's authoritative log, written by the renderer subprocess.
%APPDATA%\Roaming\Godot\app_userdata\TheGates\logs\<host>\<path>\log.txt
# Example for the default tutorial gate:
%APPDATA%\Roaming\Godot\app_userdata\TheGates\logs\thegates.io\worlds\tutorial\log.txt
```

This file is **appended across runs** (the renderer reuses the same path per gate URL). The runner script copies it to `$ResultsDir\renderer.log` after each run, so prefer the copy. If you must read the original, use offset/limit and remember older content is from previous runs.

**Anti-pattern checklist:**

- Don't `Get-Content $log` (full read) — kills context. Use `Get-Content $log | Select-String <pattern>` or the Read tool with offset+limit.
- Don't run the launcher with `--verbose` and then complain about log noise — the verbose flag adds thousands of Vulkan-extension-enumeration lines on every iteration. Only enable when chasing a specific Vulkan or rendering issue.
- Don't read `launcher.err` before `verify.json` — `launcher.err` always has at least the Vulkan loader registry warning, which is benign noise. Start with the diag.

## Edit workflow

Code you can safely edit during the port:

- `godot/modules/the_gates/sandbox_diagnostics.{h,cpp}` — extend the diag to report new things you need.
- `godot/modules/the_gates/sandbox/` — the Windows sandbox code (Firefox-style vendor + wrapper goes here). The current files are placeholders from the cef_sandbox.lib era; replace as needed.
- `godot/modules/the_gates/sandboxing.{h,cpp}` — Linux seccomp; only touch if porting Linux side.
- `godot/main/main.cpp` — only the `#ifdef TG_RENDERER` block near the end. **Don't change the order**: external texture import must precede sandbox lockdown which must precede `SandboxDiagnostics::dump()`. The whole sequence must precede return from `Main::start`.
- `app/scripts/autotest.gd` and `app/scripts/app.gd` — only if extending the autotest harness. Be cautious; an error here makes every subsequent verify return `launcher_no_exit`.

Code you should NOT edit during the port:
- Upstream Godot (`godot/scene/`, `godot/core/`, `godot/servers/`, `godot/platform/` except where called out).
- The launcher's gate-loading code in `app/scripts/loading/` — that's a separate subsystem and changing it breaks unrelated things.
- `godot/tools/run-sandbox-test.ps1` — only extend if you legitimately need new failure tags. New diagnostic fields should go into `SandboxDiagnostics`, not the runner.

## When to ask the human vs. iterate

Iterate autonomously when:
- Build errors with clear fix locations
- Renderer crashes with a clear stack
- Diag JSON shows specific wrong values (wrong integrity, missing mitigation)
- Verify-fail tags map to one of the catalog entries

Stop and ask the human when:
- You've made more than ~15 iterations on the same failure tag
- The harness itself is broken (you can no longer tell what's failing)
- A decision requires architectural reasoning the docs don't answer
- You're about to delete/replace more than a hundred lines from outside `godot/modules/the_gates/sandbox/`
- You're about to commit anything to git (commits require explicit human approval per global instructions)
- You discover a bug in the harness — fixing the harness is the human's call; flag and stop

When stopping, write a brief summary to a new file `notes/Sandboxing/Agent Session <date>.md` containing: what you were doing, what the failure was, what you've tried, and what the human needs to look at next.

## Long-running operations

`scons` builds on this machine take 5-10 minutes. The Bash tool can hang on long foreground invocations. **Always run scons in background** when invoking from Bash:

```
Bash(command="scons -j8 ...", run_in_background=True)
```

Then wait for the harness notification. Do not poll.

The runner script itself caps at `Timeout + 15s` and self-terminates if the launcher hangs, so running it foreground (default Bash timeout) is safe.

## Cherry-pick rule

If you make commits on `tg-4.5`, they must also be cherry-picked to `tg-master` per the fork's branch-sync rule (see parent CLAUDE.md). Don't skip this. If you don't have authority to push, stop and ask.

## A working session in shape

A typical iteration:

```
1. Edit code (Edit tool)
2. Bash: pwsh ... run-sandbox-test.ps1 -Build -SandboxBuild   # first iter only
3. Read the 2-3 line output
4. If [VERIFY-OK]: parse the diag fields; assess against goal; either done or pick next thing to harden
5. If [VERIFY-FAIL] <reason>:
   a. Look up reason in catalog above
   b. Grep $ResultsDir\<log> for the specific surrounding context
   c. Make targeted edit
   d. Iterate without the -Build flag (faster) — only rebuild when C++ changed
6. Goto 2
```

A realistic full port (vendoring path) takes hundreds of iterations across 3-5 weeks. Most are tiny fixes — a missing include, a wrong policy enum, an over-aggressive allow-rule. The harness lets you iterate on each in roughly 30-45 seconds (the runner's wall clock).
