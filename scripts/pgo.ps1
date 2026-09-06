# Profile-Guided Optimization pipeline (clang-cl LLVM instrumentation).
#
#   PS> .\scripts\pgo.ps1                # full pipeline: train + merge + rebuild
#   PS> .\scripts\pgo.ps1 -Frames 6000   # longer training run
#   PS> .\scripts\pgo.ps1 -SkipTrain     # reuse an existing pgo\frdx.profdata
#   PS> .\scripts\pgo.ps1 -Undo          # revert build\ to FRDX_PGO=off
#
# Pipeline:
#   1. Configure build-pgo\ with -DFRDX_PGO=generate (instrumented build)
#   2. Build frdx_sandbox there and run it with --benchmark-blur=<N>:
#      a deterministic overlay + max-blur workload over the same sources
#      version.dll ships, which exits cleanly by frame count so the
#      profile runtime flushes its .profraw on normal exit.
#   3. llvm-profdata merge -> pgo\frdx.profdata
#   4. Reconfigure build\ with -DFRDX_PGO=use and rebuild — the rebuilt
#      version.dll auto-deploys to the game when FRDX_AUTO_DEPLOY is on.
#
# After this script, plain scripts\build.ps1 incremental builds keep the
# PGO flags (they live in build\'s CMake cache) until -Undo is run.

param(
    [int]$Frames = 3000,
    [switch]$SkipTrain,
    [switch]$Undo
)

$ErrorActionPreference = "Stop"
$root   = Split-Path -Parent $PSScriptRoot
$build  = Join-Path $root "build"
$train  = Join-Path $root "build-pgo"
$pgoDir = Join-Path $root "pgo"
$profdata = Join-Path $pgoDir "frdx.profdata"

function Find-Tool([string]$name, [string[]]$fallbacks) {
    # Prefer explicit fallback paths over PATH hits: under Git Bash the
    # inherited PATH can front an MSYS2 copy of the tool (e.g. devkitPro's
    # cmake) that mangles Windows path arguments.
    foreach ($p in $fallbacks) { if (Test-Path $p) { return $p } }
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source -notmatch '\\usr\\bin\\') { return $cmd.Source }
    throw "$name not found (install LLVM/CMake or add them to PATH)"
}

# Known-good install locations, in preference order.
$cmake = Find-Tool "cmake" @(
    "${env:ProgramFiles}\CMake\bin\cmake.exe",
    "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
)
$clangCl = Find-Tool "clang-cl" @("${env:ProgramFiles}\LLVM\bin\clang-cl.exe")
$llvmBin = Split-Path -Parent $clangCl
$profdataTool = Find-Tool "llvm-profdata" @((Join-Path $llvmBin "llvm-profdata.exe"))
$ninja = Find-Tool "ninja" @(
    "${env:ProgramFiles}\Ninja\ninja.exe",
    "${env:LOCALAPPDATA}\Microsoft\WinGet\Links\ninja.exe"
)
$ninjaFound = [bool]$ninja

if ($Undo) {
    Write-Host "-> reverting build\ to FRDX_PGO=off" -ForegroundColor Cyan
    & $cmake -S $root -B $build "-DFRDX_PGO=off" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cmake undo failed" }
    & $cmake --build $build | Out-Host
    Write-Host "Done — build\ is back to non-PGO." -ForegroundColor Green
    exit 0
}

if (-not $SkipTrain) {
    # ── 1. Instrumented configure ──
    Write-Host "-> configuring training build (FRDX_PGO=generate)" -ForegroundColor Cyan
    & $cmake -S $root -B $train -G Ninja "-DCMAKE_BUILD_TYPE=Release" `
        "-DCMAKE_C_COMPILER=clang-cl" "-DCMAKE_CXX_COMPILER=clang-cl" `
        "-DFRDX_PGO=generate" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

    & $cmake --build $train --target frdx_sandbox | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "training build failed" }

    # The sandbox loads fonts/config from mods\frdx\ next to its CWD —
    # reuse the staging from build\ when present, else seed from assets.
    $trainMods = Join-Path $train "mods\frdx"
    New-Item -ItemType Directory -Force -Path (Join-Path $trainMods "fonts") | Out-Null
    $srcMods = Join-Path $build "mods\frdx"
    if (Test-Path (Join-Path $srcMods "fonts")) {
        Copy-Item -Recurse -Force (Join-Path $srcMods "fonts\*") (Join-Path $trainMods "fonts")
        if (Test-Path (Join-Path $srcMods "config.toml")) {
            Copy-Item -Force (Join-Path $srcMods "config.toml") $trainMods
        }
    } else {
        Copy-Item -Force (Join-Path $root "assets\fonts\*") (Join-Path $trainMods "fonts")
    }

    # ── 2. Training run ──
    New-Item -ItemType Directory -Force -Path $pgoDir | Out-Null
    Remove-Item -Force (Join-Path $pgoDir "*.profraw") -ErrorAction SilentlyContinue
    $env:LLVM_PROFILE_FILE = Join-Path $pgoDir "sandbox-%p.profraw"
    Write-Host "-> training: frdx_sandbox --benchmark-blur=$Frames" -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath (Join-Path $train "frdx_sandbox.exe") `
        -ArgumentList "--benchmark-blur=$Frames" -WorkingDirectory $train -PassThru -Wait
    $sw.Stop()
    Remove-Item Env:\LLVM_PROFILE_FILE
    if ($proc.ExitCode -ne 0) { throw "training run failed (exit $($proc.ExitCode))" }
    Write-Host ("   trained {0} frames in {1:f1}s" -f $Frames, $sw.Elapsed.TotalSeconds)

    # ── 3. Merge ──
    Write-Host "-> merging profraw -> pgo\frdx.profdata" -ForegroundColor Cyan
    & $profdataTool merge -output $profdata (Join-Path $pgoDir "*.profraw")
    if ($LASTEXITCODE -ne 0) { throw "llvm-profdata merge failed" }
} elseif (-not (Test-Path $profdata)) {
    throw "-SkipTrain but $profdata does not exist"
}

if (-not (Test-Path $profdata)) { throw "missing $profdata" }
$profdataItem = Get-Item $profdata
Write-Host ("   profile: {0} ({1:N1} KB)" -f $profdata, ($profdataItem.Length / 1KB))

# ── 4. Rebuild the shipping target with the profile ──
Write-Host "-> reconfiguring build\ with FRDX_PGO=use" -ForegroundColor Cyan
& $cmake -S $root -B $build "-DFRDX_PGO=use" "-DFRDX_PGO_DATA=$profdata" | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake reconfigure failed" }

# The .profdata path is baked into the compile flags, so ninja sees "no
# change" when only the profile content was refreshed.  Drop the PGO
# targets' objects to force a recompile against the new profile.
foreach ($t in @("version", "frdx_sandbox")) {
    $objDir = Join-Path $build "CMakeFiles\$t.dir"
    if (Test-Path $objDir) {
        Get-ChildItem -Recurse -Filter *.obj $objDir | Remove-Item -Force
    }
}

& $cmake --build $build --target version | Out-Host
if ($LASTEXITCODE -ne 0) { throw "PGO build failed" }

Write-Host "`nDone. version.dll rebuilt with profile guidance (auto-deployed when FRDX_AUTO_DEPLOY=ON)." -ForegroundColor Green
Write-Host "Run scripts\pgo.ps1 -Undo to revert to the plain build."
