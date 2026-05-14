<#
.SYNOPSIS
    Copy a curated subset of Chromium sources into godot/thirdparty/chromium-sandbox/.

.DESCRIPTION
    Vendors Chromium's sandbox library, Firefox-style. Mirrors the structure of
    https://hg.mozilla.org/mozilla-central/file/tip/security/sandbox/chromium/
    so engine-side ports for Linux, macOS, and Windows can share one vendored
    tree. Tests, fuzzers, integration tests, and Android-only files are skipped.

    Source root: C:\code\chromium_git\chromium\src
    Vendor root: godot/thirdparty/chromium-sandbox/

    Files are copied (not hard-linked) so we can apply patches in-tree without
    affecting upstream.
#>

[CmdletBinding()]
param(
    [string]$SourceRoot = "C:\code\chromium_git\chromium\src",
    [string]$VendorRoot = "$PSScriptRoot\..\thirdparty\chromium-sandbox"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $SourceRoot)) {
    throw "SourceRoot does not exist: $SourceRoot"
}

New-Item -ItemType Directory -Force $VendorRoot | Out-Null

function Copy-Tree([string]$relDir, [string[]]$excludeNames) {
    $src = Join-Path $SourceRoot $relDir
    if (-not (Test-Path $src)) { return 0 }
    $dst = Join-Path $VendorRoot $relDir
    New-Item -ItemType Directory -Force $dst | Out-Null
    $count = 0
    Get-ChildItem $src -Recurse -File |
        Where-Object {
            $ext = $_.Extension.ToLower()
            ($ext -eq ".cc" -or $ext -eq ".c" -or $ext -eq ".h" -or $ext -eq ".inc") -and
            -not ($_.Name -match "test|unittest|fuzzer|mock|deathtest")
        } |
        Where-Object {
            $skip = $false
            foreach ($name in $excludeNames) {
                if ($_.FullName -like "*\$name\*" -or $_.FullName -like "*\$name") { $skip = $true; break }
            }
            -not $skip
        } |
        ForEach-Object {
            $relFile = $_.FullName.Substring($src.Length).TrimStart('\','/')
            $destFile = Join-Path $dst $relFile
            $destDir = Split-Path $destFile -Parent
            if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Force $destDir | Out-Null }
            Copy-Item $_.FullName $destFile -Force
            $count++
        }
    return $count
}

function Copy-File([string]$rel) {
    $src = Join-Path $SourceRoot $rel
    if (-not (Test-Path $src)) { return $false }
    $dst = Join-Path $VendorRoot $rel
    $dstDir = Split-Path $dst -Parent
    if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Force $dstDir | Out-Null }
    Copy-Item $src $dst -Force
    return $true
}

$totals = @{}

# === sandbox/ top level ===
Copy-File "sandbox/features.cc" | Out-Null
Copy-File "sandbox/features.h" | Out-Null
Copy-File "sandbox/sandbox_export.h" | Out-Null

# === sandbox/win/ (Windows) ===
$totals["sandbox/win"] = Copy-Tree "sandbox/win" @("tests", "tools", "fuzzer", "tests_common")

# === sandbox/linux/ ===
$totals["sandbox/linux"] = Copy-Tree "sandbox/linux" @("tests", "integration_tests", "suid", "fuzzer")

# === sandbox/mac/ ===
$totals["sandbox/mac"] = Copy-Tree "sandbox/mac" @("tests", "fuzzer")

# === sandbox/policy/ (per-process-type presets, cross-platform) ===
$totals["sandbox/policy"] = Copy-Tree "sandbox/policy" @("tests", "fuzzer")

# === LICENSE files ===
Copy-File "LICENSE" | Out-Null

Write-Host "Vendored counts:"
foreach ($k in $totals.Keys | Sort-Object) {
    Write-Host ("  {0,-22} {1}" -f $k, $totals[$k])
}

# Quick total
$total = ($totals.Values | Measure-Object -Sum).Sum
Write-Host "Total: $total files under sandbox/*"
