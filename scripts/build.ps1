<#
.SYNOPSIS
    Configures, builds and (optionally) tests PulseDB.

.DESCRIPTION
    Local development convenience script for Windows + MinGW-w64.
    The toolchain root defaults to C:/mingw64-posix; override it by
    setting the PULSEDB_MINGW_ROOT environment variable.

.EXAMPLE
    ./scripts/build.ps1 -Test
    ./scripts/build.ps1 -BuildType Debug -Clean
#>
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType = "Release",
    [switch]$Test,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$mingwRoot = if ($env:PULSEDB_MINGW_ROOT) { $env:PULSEDB_MINGW_ROOT } else { "C:/mingw64-posix" }
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build"

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

cmake -S $repoRoot -B $buildDir -G "MinGW Makefiles" `
    -DCMAKE_C_COMPILER="$mingwRoot/bin/gcc.exe" `
    -DCMAKE_CXX_COMPILER="$mingwRoot/bin/g++.exe" `
    -DCMAKE_MAKE_PROGRAM="$mingwRoot/bin/mingw32-make.exe" `
    -DCMAKE_BUILD_TYPE=$BuildType
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    ctest --test-dir $buildDir --output-on-failure
    exit $LASTEXITCODE
}
