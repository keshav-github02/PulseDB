<#
.SYNOPSIS
    Builds and runs the PulseDB benchmark suites.

.DESCRIPTION
    Configures the build with -DPULSEDB_BUILD_BENCHMARKS=ON, compiles the
    benchmark executables in Release, and runs them. Windows + MinGW-w64;
    override the toolchain root with PULSEDB_MINGW_ROOT.

    The suites use the small dependency-free harness in benchmark/bench_util.hpp,
    not Google Benchmark (which does not compile against this MinGW
    distribution's COM/OLE headers). There is therefore no --benchmark_filter and
    no per-suite selection: each executable runs its full set. An earlier version
    of this script advertised Google Benchmark and accepted a -Filter argument
    that was silently ignored.

.EXAMPLE
    ./scripts/bench.ps1
#>
param()

$ErrorActionPreference = "Stop"

$mingwRoot = if ($env:PULSEDB_MINGW_ROOT) { $env:PULSEDB_MINGW_ROOT } else { "C:/mingw64-posix" }
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build"

cmake -S $repoRoot -B $buildDir -G "MinGW Makefiles" `
    -DCMAKE_C_COMPILER="$mingwRoot/bin/gcc.exe" `
    -DCMAKE_CXX_COMPILER="$mingwRoot/bin/g++.exe" `
    -DCMAKE_MAKE_PROGRAM="$mingwRoot/bin/mingw32-make.exe" `
    -DCMAKE_BUILD_TYPE=Release `
    -DPULSEDB_BUILD_BENCHMARKS=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $buildDir --parallel --target pulsedb_queue_bench pulsedb_aggregation_bench pulsedb_ingest_bench
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

foreach ($exe in "pulsedb_queue_bench", "pulsedb_aggregation_bench", "pulsedb_ingest_bench") {
    Write-Host "`n=== $exe ===" -ForegroundColor Cyan
    & "$buildDir/benchmark/$exe.exe"
}
