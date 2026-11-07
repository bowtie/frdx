# One-shot build script. Configures CMake (Release, Ninja), compiles, then
# stages everything that needs to ship in dist\.
#
#   PS> .\scripts\build.ps1                # incremental (auto jobs)
#   PS> .\scripts\build.ps1 -Clean         # wipe build/ and start fresh
#   PS> .\scripts\build.ps1 -Jobs 16       # build with 16 parallel jobs
#   PS> .\scripts\build.ps1 -NoCache       # skip ccache/sccache even if installed
#
# Output:
#   dist\version.dll            the proxy DLL (drops next to forzahorizon6.exe)
#   dist\mods\frdx\config.toml           seeded from config.example.toml
#   dist\mods\frdx\stations.json         seeded from stations.example.json
#   dist\mods\frdx\frdx-helper.exe       DualSense/download helper
#   dist\mods\frdx\fonts\                fonts used by the overlay
#   dist\mods\frdx\logos\                station logos

param(
    [switch]$Clean,
    [int]$Jobs = 0,
    [switch]$NoCache
)

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$dist  = Join-Path $root "dist"

$sw = [System.Diagnostics.Stopwatch]::StartNew()

# Build-cache inconsistency guard.
# Detect the silent-stale-DLL trap: if some .obj files for src/overlay
# are missing or zero-byte while source files have newer mtimes, ninja's
# incremental tracker can decide "no work to do" and produce a binary
# that doesn't actually reflect the current source.  Force a clean
# rebuild in that case so cmake re-stages every object.
if (Test-Path $build) {
    $overlayCppFiles = @(Get-ChildItem -Path (Join-Path $root "src\overlay") `
        -Include *.cpp -Recurse -ErrorAction SilentlyContinue)
    $overlayObjDir = Join-Path $build "CMakeFiles\version.dir\src\overlay"
    $overlayObjFiles = @(Get-ChildItem -Path $overlayObjDir `
        -Filter *.obj -ErrorAction SilentlyContinue)
    $emptyObjCount = ($overlayObjFiles | Where-Object { $_.Length -eq 0 }).Count
    $objMissing    = ($overlayObjFiles.Count -lt $overlayCppFiles.Count)
    $objEmpty      = ($emptyObjCount -gt 0)
    if ($objMissing -or $objEmpty) {
        Write-Host ("Build cache INCONSISTENT (src/overlay has {0} .cpp but {1} .obj, {2} empty); forcing clean rebuild." -f $overlayCppFiles.Count, $overlayObjFiles.Count, $emptyObjCount) -ForegroundColor Yellow
        $Clean = $true
    }
}

# Locate cmake.exe. Prefer the one on PATH; otherwise look inside any VS
# install (which always ships CMake when the C++ workload is selected),
# then fall back to the standalone CMake installer's default location.
function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsRoots = & $vswhere -all -products * -property installationPath
        foreach ($vs in $vsRoots) {
            $p = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $p) { return $p }
        }
    }
    foreach ($p in @(
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
    )) { if (Test-Path $p) { return $p } }

    throw @"
cmake.exe not found. Either:
  - install Visual Studio 2022/2026 with the "Desktop development with C++"
    workload (CMake is bundled), or
  - install CMake from https://cmake.org/download/ (tick "Add CMake to PATH").
"@
}

function Find-Ninja {
    $cmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    # winget installs a shim; also check common standalone paths
    foreach ($p in @(
        "${env:ProgramFiles}\Ninja\ninja.exe",
        "${env:LOCALAPPDATA}\Microsoft\WinGet\Links\ninja.exe"
    )) { if (Test-Path $p) { return $p } }
    return $null
}

# Prefer sccache (Rust-based, better Windows + clang-cl support) over ccache.
# Returns the executable path and its short name ("sccache" or "ccache").
function Find-Ccache {
    foreach ($name in @("sccache", "ccache")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return @{ Exe = $cmd.Source; Name = $name } }
        # winget / scoop / cargo install paths
        foreach ($p in @(
            "${env:ProgramFiles}\$name\bin\$name.exe",
            "${env:LOCALAPPDATA}\Microsoft\WinGet\Links\$name.exe",
            "${env:USERPROFILE}\scoop\shims\$name.exe",
            "${env:USERPROFILE}\.cargo\bin\$name.exe"
        )) { if (Test-Path $p) { return @{ Exe = $p; Name = $name } } }
    }
    return $null
}

# If clang-cl isn't on PATH, prepend the LLVM bin directory so
# cmake's auto-detection can find the full toolchain (clang-cl +
# lld-link + rc.exe from the MSVC environment).  Passing
# -DCMAKE_C_COMPILER explicitly would bypass that detection.
$clangCl = (Get-Command clang-cl -ErrorAction SilentlyContinue)
if (-not $clangCl) {
    $llvmBin = "${env:ProgramFiles}\LLVM\bin"
    if (Test-Path (Join-Path $llvmBin "clang-cl.exe")) {
        $env:PATH = "$llvmBin;$env:PATH"
        $clangCl = Get-Command clang-cl -ErrorAction SilentlyContinue
    }
}
if ($clangCl) {
    Write-Host "Using clang-cl: $($clangCl.Source)" -ForegroundColor DarkGray
}

$cmake = Find-CMake
Write-Host "Using cmake: $cmake" -ForegroundColor DarkGray

$ninja = Find-Ninja
if ($ninja) {
    Write-Host "Using ninja:  $ninja" -ForegroundColor DarkGray
} else {
    Write-Host "Ninja not found -- falling back to MSBuild (slower)." -ForegroundColor Yellow
    Write-Host "Install it:  winget install Ninja-build.Ninja" -ForegroundColor Yellow
}

$ccache = if (-not $NoCache) { Find-Ccache } else { $null }
if ($ccache) {
    Write-Host "Using $($ccache.Name): $($ccache.Exe)" -ForegroundColor DarkGray
} elseif (-not $NoCache) {
    Write-Host "ccache/sccache not found (install: winget install sccache)." -ForegroundColor DarkGray
}

if (-not (Test-Path (Join-Path $root "third_party\nlohmann\nlohmann\json.hpp"))) {
    Write-Host "third_party/ is empty -- running get-deps.ps1 first." -ForegroundColor Yellow
    & (Join-Path $PSScriptRoot "get-deps.ps1")
}

if ($Clean -and (Test-Path $build)) {
    Write-Host "-> clean build/" -ForegroundColor Cyan
    Remove-Item -Recurse -Force $build
}

Write-Host "-> cmake configure" -ForegroundColor Cyan
$cmakeArgs = @("-S", $root, "-B", $build)
if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
    if ($ninja) {
        $cmakeArgs += @("-G", "Ninja")
        $cmakeArgs += @("-DCMAKE_BUILD_TYPE=Release")
    } else {
        $cmakeArgs += @("-A", "x64")
    }
    if ($clangCl) {
        $cmakeArgs += @(
            "-DCMAKE_C_COMPILER=clang-cl",
            "-DCMAKE_CXX_COMPILER=clang-cl"
        )
    }
    if ($ccache) {
        $cmakeArgs += @(
            "-DCMAKE_C_COMPILER_LAUNCHER=$($ccache.Name)",
            "-DCMAKE_CXX_COMPILER_LAUNCHER=$($ccache.Name)"
        )
    }
    if (-not $clangCl) {
        Write-Host "clang-cl not on PATH; cmake may auto-detect MSVC instead." -ForegroundColor DarkGray
    }
} elseif ($NoCache) {
    # Explicitly clear stale launcher vars from a previous cached build.
    $cmakeArgs += @(
        "-DCMAKE_C_COMPILER_LAUNCHER=",
        "-DCMAKE_CXX_COMPILER_LAUNCHER="
    )
}
& $cmake @cmakeArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "-> cmake build (Release)" -ForegroundColor Cyan
$buildArgs = @("--build", $build)
if ($Jobs -gt 0) { $buildArgs += @("--parallel", $Jobs) }
if (-not $ninja) { $buildArgs += @("--config", "Release") }
& $cmake @buildArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

if (Test-Path $dist) {
    # Retry removal — files may be briefly locked by AV or the game.
    $maxRetries = 3
    for ($i = 0; $i -lt $maxRetries; $i++) {
        try {
            Remove-Item -Recurse -Force $dist -ErrorAction Stop
            break
        } catch {
            if ($i -eq $maxRetries - 1) {
                Write-Host "Warning: could not fully remove dist/ — stale files may persist." -ForegroundColor Yellow
            } else {
                Start-Sleep -Milliseconds 500
            }
        }
    }
}
New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dist "mods\frdx\fonts") | Out-Null

# Ninja outputs to build/ directly; MSBuild puts artifacts under build/Release/
$dllSrc = if ($ninja) { Join-Path $build "version.dll" } else { Join-Path $build "Release\version.dll" }
Copy-Item $dllSrc $dist
$helperSrc = if ($ninja) { Join-Path $build "frdx-helper.exe" } else { Join-Path $build "Release\frdx-helper.exe" }
Copy-Item $helperSrc (Join-Path $dist "mods\frdx")
# frdx_overlay.exe was removed — the overlay is now in-process (version.dll)
if (Test-Path (Join-Path $root "assets\fonts")) {
    Copy-Item -Recurse (Join-Path $root "assets\fonts\*") (Join-Path $dist "mods\frdx\fonts")
}
# Station logos referenced as "logos/..." by stations.example.json
# (stream artwork_url / metadata.static_artwork_url).
if (Test-Path (Join-Path $root "assets\logos")) {
    New-Item -ItemType Directory -Force -Path (Join-Path $dist "mods\frdx\logos") | Out-Null
    Copy-Item -Recurse (Join-Path $root "assets\logos\*") (Join-Path $dist "mods\frdx\logos")
}
Copy-Item (Join-Path $root "config.example.toml") (Join-Path $dist "mods\frdx\config.toml")
Copy-Item (Join-Path $root "stations.example.json") (Join-Path $dist "mods\frdx\stations.json")


$builtDll = Join-Path $dist "version.dll"

$sw.Stop()
Write-Host ("`nBuilt + staged in $dist  ({0:f1}s)" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
Get-ChildItem -Recurse -File $dist | ForEach-Object {
    "  $($_.FullName.Substring($dist.Length + 1))"
}
