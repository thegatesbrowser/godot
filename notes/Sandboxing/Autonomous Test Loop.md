# Autonomous "write → build → run → verify" loop

Design for handing the vendoring port off to an agent that can iterate without the user in the loop. Optimised for keeping the agent's context window clean while still letting it find what it needs.

## What an agent needs

The agent must be able to do four things in a tight loop, with no human in the middle:

1. **Edit code.** Easy — its existing tools handle this.
2. **Build.** Run `scons` and read the exit code; if non-zero, see the error.
3. **Launch the full launcher + renderer flow end-to-end.** Open a gate, spawn the renderer, exercise the sandbox path the real user would.
4. **Verify.** Determine deterministically whether the sandbox engaged, whether the renderer rendered, and whether anything else broke — without dumping thousands of log lines into its context.

The challenge is verify. The other three steps are mechanical. Verify is the one that decides whether the loop iterates or exits successfully, and it must produce a small, stable signal the agent can grep.

## C++ side or GDScript side

**Both, in different roles.**

C++ is where the actual sandbox work lives — the bindings, the broker spawn, the lower-token call, the policy configuration. No way around that. The agent iterates on C++ files.

GDScript is the right home for the *test harness* — the thing that makes the launcher load a gate at startup, run for N seconds, and quit. The harness is small (maybe 30 lines) and lives in `app/`. Writing the harness in C++ would mean either a second test binary or rewiring the renderer-manager flow; using GDScript means we exercise the real launcher-renderer path every test run, which is exactly what we want to verify.

So: the agent edits C++ for implementation, edits a small GDScript hook to control test runs, and lets the existing renderer-manager spawn flow do its job. No parallel test infrastructure.

## The diagnostic primitive — the thing that actually verifies

The agent can't see a window. It can't open Process Explorer. It needs the running renderer to *self-report* its sandbox state in a format the agent can grep with one query.

Add a small C++ helper, call it `SandboxDiagnostics::dump()`. It runs at one specific moment: immediately after `lower_token()` returns. It collects:

- Process integrity level (`GetTokenInformation(TokenIntegrityLevel)` → SID → human label)
- Applied mitigation flags (`GetProcessMitigationPolicy` for each policy class, OR'd together)
- Token level claim from `SandboxingWin` (what we tried to set)
- Whether the alternate desktop is in effect (`GetCurrentThreadDesktop` and compare)
- Whether `GetTargetServices()` returned non-null
- Result of a few canary operations: try to open a file outside the allow-rules, try to write to a path under the user profile, try to LoadLibrary something — record each as success/failure

Write it as one block of stdout output bracketed by stable markers:

```
=== SANDBOX-DIAG-BEGIN ===
{"engaged": true, "integrity": "untrusted", "integrity_sid": "S-1-16-0",
 "mitigations_dep": "0x...", "mitigations_aslr": "0x...", "mitigations_win32k": "0x...",
 "alt_desktop": true, "target_services": true,
 "canary_file_write": "blocked", "canary_load_library": "blocked",
 "broker_pid": 1234, "target_pid": 5678}
=== SANDBOX-DIAG-END ===
```

The agent's verify step is then one Grep:

```
Grep(renderer_log, pattern="SANDBOX-DIAG-BEGIN", -A=2)
```

Parse the JSON; assert what should be true; decide pass/fail. The full output is one line of JSON plus two markers — five lines total max. No context pollution.

The same primitive works for both vendoring and bootstrap paths. It just reads what actually happened, not what was supposed to happen.

## The launcher harness

Two small additions to `app/`:

**A `--gate-url <URL>` command-line argument** for the launcher. When set, the launcher boots straight into that gate instead of showing the menu. Wire it into `App.gd` or wherever the startup happens:

```gdscript
# In the launcher's startup code, after Navigation is ready:
var gate_url = OS.get_cmdline_user_args()  # parses --gate-url
if not gate_url.is_empty():
    Navigation.open(gate_url[0])
```

Looking at `app/scripts/navigation.gd`, `Navigation.open(location)` already emits the right signal to load a gate. The harness just calls it on boot.

**A `--autotest-timeout <seconds>` argument** that calls `get_tree().quit()` after N seconds:

```gdscript
var timeout_args = OS.get_cmdline_args().filter(...)  # parse --autotest-timeout
if timeout > 0:
    get_tree().create_timer(timeout).timeout.connect(get_tree().quit)
```

That's it for the GDScript side. ~30 lines. The renderer manager already exists and spawns the renderer when a gate loads.

## Logging strategy

The current state writes launcher output to stdout/stderr and renderer output to a file under `%AppData%\Roaming\Godot\app_userdata\TheGates\logs\<host>\<path>\log.txt`. Two changes make this agent-friendly:

**Force structured log markers.** Anywhere we want the agent to be able to find something, prefix the log line with a stable tag: `[SANDBOX]`, `[RENDERER-START]`, `[RENDERER-EXIT]`, `[VERIFY-OK]`, `[VERIFY-FAIL]`. The agent greps tags, not free text.

**Move noisy diagnostics out of `--verbose`.** The Vulkan warnings, HTTP cache logs, analytics requests etc. are useful for humans but kill an agent's context. The renderer should default to a "quiet but tagged" mode: only tagged lines, errors, and the SANDBOX-DIAG block. Add `--verbose` only when the user (or agent in a deep-debug session) opts in.

**Known log paths the agent reads.** Standardise:

- Launcher stdout: pipe to `%TEMP%\thegates-autotest\launcher.log`
- Renderer log: already at `%APPDATA%\Roaming\Godot\app_userdata\TheGates\logs\<host>\<path>\log.txt`. Could symlink/copy to the autotest temp dir at end of run.
- Build output: `%TEMP%\thegates-autotest\build.log`

The agent reads via the `Read` tool with explicit line offsets, never `cat`s a whole file.

## The runner script

A single PowerShell wrapper (`godot/tools/run-sandbox-test.ps1`) that the agent invokes per iteration. It does the full sequence and returns one number — exit code 0 if everything passed, non-zero with a tagged reason otherwise:

```powershell
# tools/run-sandbox-test.ps1
param([string]$GateUrl = "https://thegates.io/worlds/tutorial.gate",
      [int]$Timeout = 30)

$temp = "$env:TEMP\thegates-autotest"
New-Item -ItemType Directory -Force $temp | Out-Null

# Build
scons -j8 dev_build=yes tg_renderer=no tg_sandbox=yes ... 2>&1 > $temp\build-launcher.log
if ($LASTEXITCODE -ne 0) { Write-Host "[VERIFY-FAIL] launcher build"; exit 11 }

scons -j8 dev_build=yes tg_renderer=yes target=template_debug tg_sandbox=yes ... 2>&1 > $temp\build-renderer.log
if ($LASTEXITCODE -ne 0) { Write-Host "[VERIFY-FAIL] renderer build"; exit 12 }

# Launch (background) with autotest harness
$launcher = Start-Process -FilePath "$PSScriptRoot\..\bin\godot.windows.editor.dev.x86_64.llvm.exe" `
    -ArgumentList "--path","$PSScriptRoot\..\..\app","--","--gate-url",$GateUrl,"--autotest-timeout",$Timeout `
    -RedirectStandardOutput "$temp\launcher.log" -RedirectStandardError "$temp\launcher.err" `
    -PassThru -NoNewWindow

# Wait for completion (or kill after timeout + buffer)
$exited = $launcher.WaitForExit(($Timeout + 10) * 1000)
if (-not $exited) { $launcher.Kill(); Write-Host "[VERIFY-FAIL] timeout"; exit 13 }

# Find the renderer log
$renderer_log = Get-ChildItem "$env:APPDATA\Roaming\Godot\app_userdata\TheGates\logs" -Recurse -Filter log.txt |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1

if (-not $renderer_log) { Write-Host "[VERIFY-FAIL] renderer log missing"; exit 14 }

# Copy to autotest dir for easy reading
Copy-Item $renderer_log.FullName "$temp\renderer.log"

# Check for the diagnostic block
$diag = Select-String -Path "$temp\renderer.log" -Pattern "SANDBOX-DIAG-BEGIN" -Context 0,2
if (-not $diag) { Write-Host "[VERIFY-FAIL] no SANDBOX-DIAG block"; exit 15 }

# Single-line summary the agent can grep
Write-Host "[VERIFY-OK] sandbox engaged; see $temp\renderer.log"
exit 0
```

The agent calls this once per iteration: `pwsh tools/run-sandbox-test.ps1`, reads the exit code, greps the small set of tagged output lines. If exit code is non-zero, it Greps just the failure tag and the build log's last 50 lines.

Context cost per iteration: maybe 30 lines read at most. Sustainable across many iterations.

## The agent's loop, end to end

```
while not done:
    1. Read [[Reference Material]] and [[Vendoring Option]] once at start
    2. Edit C++ source files (sandbox bindings, broker spawn, lower_token, diag dump)
    3. Bash: pwsh tools/run-sandbox-test.ps1
       → exit code 0 = pass; non-zero = specific failure mode
    4. If exit 0: done, summarise diff and stop.
    5. If exit non-zero:
       a. Grep the runner output for [VERIFY-FAIL] tag → know which stage failed
       b. If build failure: Read last 50 lines of $temp\build-*.log
       c. If runtime failure: Read $temp\renderer.log around SANDBOX-DIAG markers
       d. Reason about the failure, edit code, iterate
    6. Hard limit: N iterations or M minutes; if exceeded, hand back to human with a summary.
```

## The C++ side: keep the surface small

The C++ work that needs to be done for the vendoring path is large (vendor the library, port the wrapper). The work the agent does iteratively after the first compile-clean state is small:

- Fix policy configuration bugs
- Tune the allow-rules
- Fix DLL preload ordering
- Get the diagnostic dump correct
- Handle errors that surface in the diag output

Each of these is a small, localised edit. The agent doesn't need to understand the whole vendored library; it needs the wrapper API surface (the equivalent of Firefox's `SandboxBroker`) and the diagnostic primitive.

To make this tractable, expose **a single C++ entry point** the agent works against: `TgSandbox::ConfigureAndLockdown()` for the renderer side, `TgSandbox::LaunchSandboxedRenderer(path, args)` for the broker side. Each is a few dozen lines that delegates into the vendored Chromium library. The agent reasons about these two functions, not about the full sandbox API.

## Failure-mode catalog (what each VERIFY-FAIL means)

| Tag | Meaning | What the agent should look at |
|---|---|---|
| `[VERIFY-FAIL] launcher build` | scons of launcher binary failed | `$temp\build-launcher.log` last 100 lines |
| `[VERIFY-FAIL] renderer build` | scons of renderer binary failed | `$temp\build-renderer.log` last 100 lines |
| `[VERIFY-FAIL] timeout` | launcher didn't exit within autotest timeout | `$temp\launcher.log` for hang location |
| `[VERIFY-FAIL] renderer log missing` | renderer never spawned or never wrote log | launcher log for spawn error |
| `[VERIFY-FAIL] no SANDBOX-DIAG block` | renderer reached gate load but `lower_token` never ran or crashed before diag dump | renderer log around last `[SANDBOX]` line |
| `[VERIFY-FAIL] diag integrity != untrusted` | sandbox engaged but at wrong integrity level | parse diag JSON; check policy config |
| `[VERIFY-FAIL] diag canary_file_write != blocked` | the sandbox isn't actually blocking what it should | policy too permissive |
| `[VERIFY-OK]` | sandbox engaged at correct integrity, canaries blocked, no errors after lockdown | done |

Adding new failure tags is cheap; they're just `Write-Host` lines in the script.

## What this lets the user do

Start the agent with a goal — "port Firefox's vendored Chromium sandbox subset into our fork, get the diag block to show `integrity: untrusted` with canaries blocked, on a real gate load" — and walk away. The agent runs hundreds of iterations against a stable verify primitive. The user only checks back when the agent reports success or hits its iteration cap.

The investment to enable this is small: ~50 lines of GDScript for the harness, ~200 lines of C++ for `SandboxDiagnostics`, one PowerShell script. Build it once; reuse it for the whole vendoring effort, and again for any future sandbox tightening (mitigations, brokered IPC, the second renderer process if we split GPU work).

## Things to be aware of

**The agent might game the verify.** If the diag block is the only signal, an agent that gets stuck might modify the diag function to lie about what it found. The script should compute *some* signal independently — e.g., spawn a separate small `verify-sandbox.exe` that queries the renderer's PID via Win32 directly and asserts integrity level there. Cross-check.

**Output drift.** Log line formats change over time. The agent must keep its grep patterns close to the source. When it edits the diag dump, it must also update its own grep queries — or the script handles parsing for it.

**The renderer log location is not deterministic.** The current code embeds the gate URL into the path (`thegates.io/worlds/tutorial/log.txt`). The script must find it by recency or by passing a fixed `--log-file` override to the renderer. Adding `--log-file <path>` to the renderer (and the launcher passing it through to the renderer) makes the lookup trivial.

**Stale processes.** If a previous run's renderer didn't exit, the next launch can conflict. Script kills any running `godot.windows.template_debug.dev.renderer.*` before launching.

**Context-window-friendly grep.** When reading `$temp\renderer.log` with Grep, use `head_limit` and only ask for lines around the SANDBOX-DIAG markers. Avoid `cat`.

## What to build first (in order)

1. The diagnostic dump function in C++. Standalone, callable from anywhere. Test it by running a normal renderer once and confirming the JSON block appears.
2. The launcher's `--gate-url` and `--autotest-timeout` GDScript handlers.
3. The PowerShell runner script.
4. A `verify-sandbox.exe` cross-check tool (small standalone, queries a PID).
5. Then start the agent on the vendoring port.

Maybe two days of harness work, after which an agent can drive the vendoring loop autonomously for as long as we let it.
