<#
.SYNOPSIS
    Re-merge a fresh cef_sandbox.lib from out/Release_GN_x64_sandbox/obj after
    a ninja incremental build. Bypasses CEF's automate-git distrib step (which
    re-extracts depot_tools and re-clones CEF, both unnecessary for our
    iteration loop).

.DESCRIPTION
    Calls lib.exe directly on the same .lib + .obj inputs that CEF's
    combine_libs.py would use. Writes the output to the same path our build's
    LIBPATH points to:

      C:\code\chromium_git\chromium\src\cef\binary_distrib\
        cef_binary_137.0.10+g7e14fe1+chromium-137.0.7151.69_windows64_sandbox\
        Release\cef_sandbox.lib
#>

[CmdletBinding()]
param(
    [string]$BuildDir = "C:\code\chromium_git\chromium\src\out\Release_GN_x64_sandbox",
    [string]$OutLib = "C:\code\chromium_git\chromium\src\cef\binary_distrib\cef_binary_137.0.10+g7e14fe1+chromium-137.0.7151.69_windows64_sandbox\Release\cef_sandbox.lib",
    [string]$LibExe = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.43.34808\bin\Hostx64\x64\lib.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $LibExe)) {
    throw "lib.exe not found at $LibExe"
}

# The exact input list combine_libs.py would have used, scraped from a prior
# automate-git distrib run. Order matters: lib.exe processes objects in
# specified order, so symbols defined earlier win on conflict.
$inputs = @(
    "obj\base\base.lib",
    "obj\base\base_static.lib",
    "obj\base\third_party\cityhash\cityhash\city.obj",
    "obj\base\third_party\double_conversion\double_conversion.lib",
    "obj\base\third_party\superfasthash\superfasthash\superfasthash.obj",
    "obj\base\win\pe_image.lib",
    "obj\cef\cef_sandbox.lib",
    "obj\sandbox\common\features.obj",
    "obj\sandbox\win\sandbox.lib",
    "obj\sandbox\win\service_resolver\resolver.obj",
    "obj\sandbox\win\service_resolver\resolver_64.obj",
    "obj\sandbox\win\service_resolver\service_resolver.obj",
    "obj\sandbox\win\service_resolver\service_resolver_64.obj",
    "obj\third_party\abseil-cpp\absl\base\base\cycleclock.obj",
    "obj\third_party\abseil-cpp\absl\base\base\spinlock.obj",
    "obj\third_party\abseil-cpp\absl\base\base\sysinfo.obj",
    "obj\third_party\abseil-cpp\absl\base\base\thread_identity.obj",
    "obj\third_party\abseil-cpp\absl\base\base\unscaledcycleclock.obj",
    "obj\third_party\abseil-cpp\absl\base\log_severity\log_severity.obj",
    "obj\third_party\abseil-cpp\absl\base\malloc_internal\low_level_alloc.obj",
    "obj\third_party\abseil-cpp\absl\base\raw_logging_internal\raw_logging.obj",
    "obj\third_party\abseil-cpp\absl\base\spinlock_wait\spinlock_wait.obj",
    "obj\third_party\abseil-cpp\absl\base\strerror\strerror.obj",
    "obj\third_party\abseil-cpp\absl\base\throw_delegate\throw_delegate.obj",
    "obj\third_party\abseil-cpp\absl\base\tracing_internal\tracing.obj",
    "obj\third_party\abseil-cpp\absl\debugging\debugging_internal\address_is_readable.obj",
    "obj\third_party\abseil-cpp\absl\debugging\debugging_internal\elf_mem_image.obj",
    "obj\third_party\abseil-cpp\absl\debugging\debugging_internal\vdso_support.obj",
    "obj\third_party\abseil-cpp\absl\debugging\decode_rust_punycode\decode_rust_punycode.obj",
    "obj\third_party\abseil-cpp\absl\debugging\demangle_internal\demangle.obj",
    "obj\third_party\abseil-cpp\absl\debugging\demangle_rust\demangle_rust.obj",
    "obj\third_party\abseil-cpp\absl\debugging\examine_stack\examine_stack.obj",
    "obj\third_party\abseil-cpp\absl\debugging\failure_signal_handler\failure_signal_handler.obj",
    "obj\third_party\abseil-cpp\absl\debugging\leak_check\leak_check.obj",
    "obj\third_party\abseil-cpp\absl\debugging\stacktrace\stacktrace.obj",
    "obj\third_party\abseil-cpp\absl\debugging\symbolize\symbolize.obj",
    "obj\third_party\abseil-cpp\absl\debugging\utf8_for_code_point\utf8_for_code_point.obj",
    "obj\third_party\abseil-cpp\absl\numeric\int128\int128.obj",
    "obj\third_party\abseil-cpp\absl\strings\cord\cord.obj",
    "obj\third_party\abseil-cpp\absl\strings\cord\cord_analysis.obj",
    "obj\third_party\abseil-cpp\absl\strings\cord_internal\cord_internal.obj",
    "obj\third_party\abseil-cpp\absl\strings\cord_internal\cord_rep_btree.obj",
    "obj\third_party\abseil-cpp\absl\strings\cord_internal\cord_rep_btree_navigator.obj",
    "obj\third_party\abseil-cpp\absl\strings\cord_internal\cord_rep_btree_reader.obj",
    "obj\third_party\abseil-cpp\absl\strings\cord_internal\cord_rep_consume.obj",
    "obj\third_party\abseil-cpp\absl\strings\cord_internal\cord_rep_crc.obj",
    "obj\third_party\abseil-cpp\absl\strings\cordz_functions\cordz_functions.obj",
    "obj\third_party\abseil-cpp\absl\strings\cordz_handle\cordz_handle.obj",
    "obj\third_party\abseil-cpp\absl\strings\cordz_info\cordz_info.obj",
    "obj\third_party\abseil-cpp\absl\strings\internal\escaping.obj",
    "obj\third_party\abseil-cpp\absl\strings\internal\ostringstream.obj",
    "obj\third_party\abseil-cpp\absl\strings\internal\utf8.obj",
    "obj\third_party\abseil-cpp\absl\strings\str_format_internal\arg.obj",
    "obj\third_party\abseil-cpp\absl\strings\str_format_internal\bind.obj",
    "obj\third_party\abseil-cpp\absl\strings\str_format_internal\extension.obj",
    "obj\third_party\abseil-cpp\absl\strings\str_format_internal\float_conversion.obj",
    "obj\third_party\abseil-cpp\absl\strings\str_format_internal\output.obj",
    "obj\third_party\abseil-cpp\absl\strings\str_format_internal\parser.obj",
    "obj\third_party\abseil-cpp\absl\strings\string_view\string_view.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\ascii.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\charconv.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\charconv_bigint.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\charconv_parse.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\damerau_levenshtein_distance.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\escaping.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\match.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\memutil.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\numbers.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\str_cat.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\str_replace.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\str_split.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\stringify_sink.obj",
    "obj\third_party\abseil-cpp\absl\strings\strings\substitute.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\graphcycles_internal\graphcycles.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\kernel_timeout_internal\kernel_timeout.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\barrier.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\blocking_counter.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\create_thread_identity.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\futex_waiter.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\mutex.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\notification.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\per_thread_sem.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\pthread_waiter.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\sem_waiter.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\stdcpp_waiter.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\waiter_base.obj",
    "obj\third_party\abseil-cpp\absl\synchronization\synchronization\win32_waiter.obj",
    "obj\third_party\abseil-cpp\absl\time\internal\cctz\civil_time\civil_time_detail.obj",
    "obj\third_party\abseil-cpp\absl\time\internal\cctz\time_zone\time_zone_fixed.obj",
    "obj\third_party\abseil-cpp\absl\time\internal\cctz\time_zone\time_zone_format.obj",
    "obj\third_party\abseil-cpp\absl\time\internal\cctz\time_zone\time_zone_if.obj",
    "obj\third_party\abseil-cpp\absl\time\internal\cctz\time_zone\time_zone_impl.obj",
    "obj\third_party\abseil-cpp\absl\time\internal\cctz\time_zone\time_zone_info.obj",
    "obj\third_party\abseil-cpp\absl\time\internal\cctz\time_zone\time_zone_libc.obj",
    "obj\third_party\abseil-cpp\absl\time\internal\cctz\time_zone\time_zone_lookup.obj",
    "obj\third_party\abseil-cpp\absl\time\internal\cctz\time_zone\time_zone_posix.obj",
    "obj\third_party\abseil-cpp\absl\time\internal\cctz\time_zone\zone_info_source.obj",
    "obj\third_party\abseil-cpp\absl\time\time\civil_time.obj",
    "obj\third_party\abseil-cpp\absl\time\time\clock.obj",
    "obj\third_party\abseil-cpp\absl\time\time\duration.obj",
    "obj\third_party\abseil-cpp\absl\time\time\format.obj",
    "obj\third_party\abseil-cpp\absl\time\time\time.obj"
)

# Validate every input exists before we invoke lib.exe — otherwise lib.exe
# errors with the cryptic "command line too long" guess.
$missing = @()
foreach ($rel in $inputs) {
    $abs = Join-Path $BuildDir $rel
    if (-not (Test-Path $abs)) { $missing += $rel }
}
if ($missing.Count -gt 0) {
    Write-Host "Missing inputs (need a fresh ninja build?):"
    foreach ($m in $missing) { Write-Host "  $m" }
    throw "Cannot combine: $($missing.Count) input(s) absent."
}

# Use a response file to avoid Windows command-line length issues and to make
# lib.exe failures debuggable (the file is preserved in $env:TEMP).
$rspPath = Join-Path $env:TEMP "cef_sandbox_lib.rsp"
$rspLines = @("/MACHINE:X64", "/NOLOGO", "/OUT:`"$OutLib`"")
foreach ($rel in $inputs) {
    $abs = Join-Path $BuildDir $rel
    $rspLines += "`"$abs`""
}
Set-Content -Path $rspPath -Value $rspLines -Encoding ASCII

# Ensure output dir exists.
$outDir = Split-Path $OutLib -Parent
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }

Write-Host "Running: lib.exe @$rspPath"
& $LibExe "@$rspPath"
if ($LASTEXITCODE -ne 0) {
    throw "lib.exe failed with exit code $LASTEXITCODE (response file: $rspPath)"
}

$info = Get-Item $OutLib
Write-Host "OK. Output: $OutLib"
Write-Host ("  size: {0:N0} bytes" -f $info.Length)
Write-Host ("  date: {0}" -f $info.LastWriteTime)
