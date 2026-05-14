<#
.SYNOPSIS
    Autonomous launcher+renderer sandbox verification loop.

.DESCRIPTION
    Boots TheGates launcher with an autotest payload, lets it open a gate,
    waits for the renderer to spawn and reach its first frame, then parses
    the renderer's SANDBOX-DIAG-BEGIN/END JSON block plus a few health
    checks. Emits exactly one [VERIFY-OK] or [VERIFY-FAIL] <reason> line
    plus the parsed JSON on success. Exit code 0 = pass; non-zero =
    specific failure mode (see CATALOG below).

    Designed for an agent loop: every run produces <30 lines of output
    and a stable exit code. Logs are kept on disk for the agent to grep
    with explicit patterns.

.PARAMETER GateUrl
    The gate to load. Defaults to https://thegates.io/worlds/tutorial.gate.

.PARAMETER Timeout
    Seconds the launcher runs before quitting itself. Default 25.

.PARAMETER Build
    If set, rebuild launcher + renderer first. Off by default to keep
    the verify cycle fast.

.PARAMETER SandboxBuild
    If set together with -Build, builds with tg_sandbox=yes.

.PARAMETER LauncherBin
    Override the launcher binary path. Defaults to the dev editor build.

.PARAMETER RendererBin
    Override the renderer binary path. Defaults to the dev template_debug
    renderer build. (Note: the renderer binary used by the launcher is
    selected via app/resources/renderer_executable.tres, not by this
    parameter — this is only used by Verbose diagnostics.)

.PARAMETER VerboseLogs
    Pass --verbose to launcher and renderer. Drastically increases log
    size; use only for deep debugging.

.PARAMETER ResultsDir
    Where launcher.log, launcher.err, renderer.log copies, and verify.json
    are written. Defaults to $env:TEMP\thegates-autotest.

.EXAMPLE
    pwsh godot/tools/run-sandbox-test.ps1
    # Default: tutorial gate, 25 sec, no rebuild

.EXAMPLE
    pwsh godot/tools/run-sandbox-test.ps1 -Build -SandboxBuild
    # Build with sandbox flag on, then verify

.NOTES
    [VERIFY-FAIL] tags emitted by this script:
      build_launcher          scons launcher exited non-zero
      build_renderer          scons renderer exited non-zero
      launcher_no_exit        launcher hung past timeout + buffer
      renderer_never_started  no renderer log found / no [RENDERER-START] line
      renderer_no_ready       [RENDERER-START] seen but [RENDERER-READY] missing
                              (renderer crashed during sandbox / external texture phase)
      no_diag_block           [RENDERER-READY] seen but no SANDBOX-DIAG block
      diag_parse_failed       SANDBOX-DIAG block present but JSON invalid
      renderer_errors         ERROR/FATAL lines after [RENDERER-READY]
      gate_not_entered        [AUTOTEST-GATE-ENTERED] never fired
                              (gate failed to load or renderer failed to spawn)
#>

[CmdletBinding()]
param(
    [string]$GateUrl = "https://thegates.io/worlds/tutorial.gate",
    [int]$Timeout = 25,
    [switch]$Build,
    [switch]$SandboxBuild,
    [string]$LauncherBin = "",
    [string]$RendererBin = "",
    [switch]$VerboseLogs,
    [string]$ResultsDir = ""
)

# --- Paths ----------------------------------------------------------------
$ScriptDir = $PSScriptRoot
$GodotDir  = Split-Path $ScriptDir -Parent
$RepoDir   = Split-Path $GodotDir -Parent
$AppDir    = Join-Path $RepoDir "app"
$BinDir    = Join-Path $GodotDir "bin"

if (-not $LauncherBin) {
    $LauncherBin = Join-Path $BinDir "godot.windows.editor.dev.x86_64.llvm.console.exe"
}
if (-not $RendererBin) {
    $RendererBin = Join-Path $BinDir "godot.windows.template_debug.dev.renderer.x86_64.llvm.console.exe"
}
if (-not $ResultsDir) {
    $ResultsDir = Join-Path $env:TEMP "thegates-autotest"
}

New-Item -ItemType Directory -Force $ResultsDir | Out-Null
$LauncherLog  = Join-Path $ResultsDir "launcher.log"
$LauncherErr  = Join-Path $ResultsDir "launcher.err"
$RendererLogCopy = Join-Path $ResultsDir "renderer.log"
$BuildLog        = Join-Path $ResultsDir "build.log"
$VerifyJson      = Join-Path $ResultsDir "verify.json"

# Clear previous artifacts
Remove-Item -Force -ErrorAction SilentlyContinue $LauncherLog,$LauncherErr,$RendererLogCopy,$VerifyJson | Out-Null

function Emit-Fail([string]$reason, [int]$code) {
    Write-Host "[VERIFY-FAIL] $reason"
    Write-Host "  results=$ResultsDir"
    exit $code
}

function Emit-Pass([string]$summary) {
    Write-Host "[VERIFY-OK] $summary"
    Write-Host "  results=$ResultsDir"
    exit 0
}

# --- Step 1: Optional build ----------------------------------------------
if ($Build) {
    Push-Location $GodotDir
    try {
        $sconsArgs = @(
            "dev_build=yes",
            "tg_renderer=no",
            "compiledb=yes",
            "use_llvm=yes",
            "linker=lld",
            "disable_exceptions=no"
        )
        if ($SandboxBuild) { $sconsArgs += "tg_sandbox=yes" }
        Write-Host "[BUILD] launcher: scons $($sconsArgs -join ' ')"
        scons -j8 @sconsArgs 2>&1 | Tee-Object -FilePath $BuildLog
        if ($LASTEXITCODE -ne 0) { Emit-Fail "build_launcher" 11 }

        $sconsArgs2 = @(
            "dev_build=yes",
            "tg_renderer=yes",
            "target=template_debug",
            "compiledb=yes",
            "use_llvm=yes",
            "linker=lld",
            "disable_exceptions=no"
        )
        if ($SandboxBuild) { $sconsArgs2 += "tg_sandbox=yes" }
        Write-Host "[BUILD] renderer: scons $($sconsArgs2 -join ' ')"
        scons -j8 @sconsArgs2 2>&1 | Tee-Object -FilePath $BuildLog -Append
        if ($LASTEXITCODE -ne 0) { Emit-Fail "build_renderer" 12 }
    } finally {
        Pop-Location
    }
}

if (-not (Test-Path $LauncherBin)) {
    Emit-Fail "launcher_bin_missing path=$LauncherBin" 10
}
if (-not (Test-Path $RendererBin)) {
    Emit-Fail "renderer_bin_missing path=$RendererBin" 10
}

# --- Step 2: Kill any stale renderer / launcher --------------------------
Get-Process -Name "godot.windows.template_debug.dev.renderer*" -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name "godot.windows.editor.dev*" -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 200

# --- Step 3: Capture the latest renderer log dir mtime BEFORE launch ----
# Renderer log path includes the gate URL; we identify "the renderer log
# from THIS run" as the most-recently-modified log.txt under the logs
# tree whose mtime is after $launchStart.
$logsRoot = Join-Path $env:APPDATA "Godot\app_userdata\TheGates\logs"
New-Item -ItemType Directory -Force $logsRoot | Out-Null
$launchStart = Get-Date

# --- Step 4: Launch launcher with autotest payload -----------------------
$launcherArgs = @(
    "--path", $AppDir,
    "--",
    "--autotest",
    "--gate-url", $GateUrl,
    "--autotest-timeout", $Timeout
)
if ($VerboseLogs) { $launcherArgs += "--verbose" }

Write-Host "[RUN] $LauncherBin $($launcherArgs -join ' ')"
$proc = Start-Process -FilePath $LauncherBin `
    -ArgumentList $launcherArgs `
    -RedirectStandardOutput $LauncherLog `
    -RedirectStandardError $LauncherErr `
    -NoNewWindow -PassThru

# Give it the timeout plus a buffer to flush + quit gracefully.
$buffer = 15
$exited = $proc.WaitForExit(($Timeout + $buffer) * 1000)
if (-not $exited) {
    Write-Host "[KILL] launcher did not exit, terminating"
    try { $proc.Kill() } catch { }
    Start-Sleep -Seconds 1
    Get-Process -Name "godot.windows.template_debug.dev.renderer*" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Emit-Fail "launcher_no_exit pid=$($proc.Id)" 13
}
# Start-Process -PassThru sometimes leaves ExitCode null until .WaitForExit()
# is called WITHOUT a timeout. Call again to settle.
$proc.WaitForExit()
$launcherExit = $proc.ExitCode
if ($null -eq $launcherExit) { $launcherExit = "?" }

# --- Step 5: Locate the renderer log produced by this run ---------------
$rendererLogFile = $null
if (Test-Path $logsRoot) {
    $rendererLogFile = Get-ChildItem $logsRoot -Recurse -Filter log.txt -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $launchStart } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
if (-not $rendererLogFile) {
    # Last fallback: the most-recently-modified renderer log overall
    $rendererLogFile = Get-ChildItem $logsRoot -Recurse -Filter log.txt -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
if (-not $rendererLogFile) {
    Emit-Fail "renderer_never_started no_log_file" 14
}

Copy-Item $rendererLogFile.FullName $RendererLogCopy -Force

# --- Step 6: Verify health markers in renderer log ----------------------
$renderer = Get-Content $RendererLogCopy

# The renderer log accumulates across runs (same path per gate URL).
# Use the LAST occurrence of each marker so we look at this run only.
$rendererStartLine = $renderer | Select-String -Pattern "\[RENDERER-START\]" | Select-Object -Last 1
if (-not $rendererStartLine) {
    Emit-Fail "renderer_never_started no_start_marker" 14
}

$rendererReadyLine = $renderer | Select-String -Pattern "\[RENDERER-READY\]" | Select-Object -Last 1
if (-not $rendererReadyLine -or $rendererReadyLine.LineNumber -lt $rendererStartLine.LineNumber) {
    Emit-Fail "renderer_no_ready" 15
}

# --- Step 7: Parse the SANDBOX-DIAG block (most recent only) ------------
$diagBeginMatch = $renderer | Select-String -Pattern "=== SANDBOX-DIAG-BEGIN ===" | Select-Object -Last 1
$diagEndMatch   = $renderer | Select-String -Pattern "=== SANDBOX-DIAG-END ===" | Select-Object -Last 1
$diagBegin = if ($diagBeginMatch) { $diagBeginMatch.LineNumber } else { 0 }
$diagEnd   = if ($diagEndMatch) { $diagEndMatch.LineNumber } else { 0 }

if (-not $diagBegin -or -not $diagEnd -or $diagEnd -le $diagBegin -or $diagBegin -lt $rendererStartLine.LineNumber) {
    Emit-Fail "no_diag_block" 16
}

# Lines between markers form one JSON object (or multiple lines if Godot
# pretty-printed it). Convert from 1-based LineNumber to 0-based array index.
$jsonLines = $renderer[$diagBegin..($diagEnd - 2)]
$jsonText = ($jsonLines -join "`n").Trim()
try {
    $diag = $jsonText | ConvertFrom-Json
} catch {
    Set-Content $VerifyJson $jsonText
    Emit-Fail "diag_parse_failed see=$VerifyJson" 17
}
$diag | ConvertTo-Json -Depth 10 | Set-Content $VerifyJson

# --- Step 8: Gate health checks -----------------------------------------
# Did the launcher consider the gate "entered" (renderer spawn returned ok)?
$launcher = Get-Content $LauncherLog -ErrorAction SilentlyContinue
$gateEntered = $launcher | Select-String -Pattern "\[AUTOTEST-GATE-ENTERED\]" | Select-Object -Last 1
if (-not $gateEntered) {
    Emit-Fail "gate_not_entered" 18
}

# Did the renderer reach the external-texture import path?
$rendererExt = $renderer | Select-String -Pattern "TGExternalTexture:" | Select-Object -Last 1
if (-not $rendererExt -or $rendererExt.LineNumber -lt $rendererStartLine.LineNumber) {
    Emit-Fail "renderer_no_external_texture" 19
}

# --- Step 9: Scan for late-phase errors / crashes -----------------------
# Errors AFTER [RENDERER-READY] are real problems (anything before may be
# Vulkan loader warnings during startup). Skip past the READY line in
# this run's segment of the log only.
$postReady = $renderer | Select-Object -Skip $rendererReadyLine.LineNumber
$errorLines = $postReady | Select-String -Pattern "ERROR:|FATAL|CRASH|Segmentation"
if ($errorLines) {
    $first = $errorLines | Select-Object -First 1
    Emit-Fail "renderer_errors first=$($first.Line.Substring(0, [Math]::Min(80, $first.Line.Length)))" 20
}

# --- Step 10: Emit pass + a one-line summary the agent can parse --------
# Note: avoid variable names that shadow the script's switch parameters
# (e.g. $Build is the script param) — PowerShell is case-insensitive.
$diagIntegrity  = "?"
$diagPid        = "?"
$diagBuild      = "?"
$diagCanaryFile = "?"
if ($diag.PSObject.Properties["integrity"]) { $diagIntegrity = [string]($diag.integrity) }
if ($diag.PSObject.Properties["pid"])       { $diagPid       = [string]($diag.pid) }
if ($diag.PSObject.Properties["build"])     { $diagBuild     = [string]($diag.build) }
if ($diag.PSObject.Properties["canaries"]) {
    if ($diag.canaries.PSObject.Properties["canary_file_write"]) {
        $diagCanaryFile = [string]($diag.canaries.canary_file_write)
    }
}

Emit-Pass "integrity=$diagIntegrity renderer_pid=$diagPid canary_file=$diagCanaryFile build=$diagBuild"
