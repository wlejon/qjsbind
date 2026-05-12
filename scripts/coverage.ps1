# Test coverage report for qjsbind (Windows / MSVC only).
#
# Runs the qjsbind_test suite under OpenCppCoverage via ctest and emits an
# HTML report at build/coverage/index.html.
#
# Requirements:
#   - OpenCppCoverage installed (winget install OpenCppCoverage.OpenCppCoverage)
#   - Debug build present at build/tests/Debug/qjsbind_test.exe (PDBs needed)
#
# Usage:
#   pwsh scripts/coverage.ps1
#   pwsh scripts/coverage.ps1 -Output build/cov  # custom output dir

[CmdletBinding()]
param(
    [string]$Output = 'build/coverage'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$occ = "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe"
if (-not (Test-Path $occ)) {
    throw "OpenCppCoverage not found at $occ. Install: winget install OpenCppCoverage.OpenCppCoverage"
}

$testExe = Join-Path $root 'build\tests\Debug\qjsbind_test.exe'
if (-not (Test-Path $testExe)) {
    throw "$testExe not found. Build first: cmake --build build --config Debug"
}

$outAbs = if ([System.IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $root $Output }
if (Test-Path $outAbs) { Remove-Item -Recurse -Force $outAbs }

& $occ `
    --sources "$root\include" `
    --sources "$root\tests" `
    --modules qjsbind_test.exe `
    --cover_children `
    --export_type "html:$outAbs" `
    --working_dir $root `
    --quiet `
    -- ctest --test-dir (Join-Path $root 'build') -C Debug --output-on-failure

Write-Host ""
Write-Host "Coverage report: $outAbs\index.html"
