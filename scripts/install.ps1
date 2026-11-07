# Copies the built mod into a Forza Horizon 6 install. Run after build.ps1.
#
#   PS> .\scripts\install.ps1 -GameDir "C:\XboxGames\Forza Horizon 6\Content"
#
# Existing files are backed up to *.bak before being overwritten.

param(
    [Parameter(Mandatory = $true)] [string] $GameDir
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"

if (-not (Test-Path (Join-Path $dist "version.dll"))) {
    throw "dist\version.dll not found -- run scripts\build.ps1 first."
}
if (-not (Test-Path $GameDir)) {
    throw "Game directory not found: $GameDir"
}
if (-not (Test-Path (Join-Path $GameDir "forzahorizon6.exe"))) {
    Write-Warning "forzahorizon6.exe not found in $GameDir -- make sure this is the right folder."
}

function Backup-AndCopy([string]$src, [string]$dst) {
    $dstDir = Split-Path -Parent $dst
    if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Force -Path $dstDir | Out-Null }
    if (Test-Path $dst) { Copy-Item $dst "$dst.bak" -Force }
    Copy-Item $src $dst -Force
    "  + $($dst.Substring($GameDir.Length + 1))"
}

# Lock check — Windows won't let us replace a dll that the running process
# has mapped (Forza holds version.dll open for the lifetime of the game).
# Detect that explicitly and throw a clear message instead of silently
# failing the cp step.
$targetDll = Join-Path $GameDir "version.dll"
if (Test-Path $targetDll) {
    try {
        $probe = [System.IO.File]::Open($targetDll, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        $probe.Close()
    } catch {
        throw "DEPLOY FAILED: version.dll in $GameDir is locked. Fully CLOSE Forza Horizon 6 (not just minimize the window) before installing."
    }
}

Backup-AndCopy (Join-Path $dist "version.dll") $targetDll | Out-Host

# MD5 verification — confirm the deploy actually copied the new bytes
# (catches silent cp failures, network drive quirks, file-locked races).
$installedMd5 = (Get-FileHash $targetDll -Algorithm MD5).Hash
$distMd5      = (Get-FileHash (Join-Path $dist "version.dll") -Algorithm MD5).Hash
Write-Host ("`nDeploy check  dist MD5: {0}" -f $distMd5) -ForegroundColor DarkGray
Write-Host ("               game MD5: {0}" -f $installedMd5) -ForegroundColor DarkGray
if ($installedMd5 -ne $distMd5) {
    Write-Warning "Deploy check FAILED: MD5 mismatch. Game folder still has the old version.dll."
}

$dataDir = Join-Path $GameDir "mods\frdx"
if (-not (Test-Path $dataDir)) { New-Item -ItemType Directory -Force -Path $dataDir | Out-Null }
New-Item -ItemType Directory -Force -Path (Join-Path $dataDir "fonts") | Out-Null

$cfg = Join-Path $dataDir "config.toml"
if (-not (Test-Path $cfg)) {
    Copy-Item (Join-Path $dist "mods\frdx\config.toml") $cfg
    Write-Host "  + mods\frdx\config.toml  (seeded from example)" -ForegroundColor Yellow
}
$stations = Join-Path $dataDir "stations.json"
if (-not (Test-Path $stations)) {
    Copy-Item (Join-Path $dist "mods\frdx\stations.json") $stations
    Write-Host "  + mods\frdx\stations.json  (seeded from example)" -ForegroundColor Yellow
}
$helper = Join-Path $dist "mods\frdx\frdx-helper.exe"
if (Test-Path $helper) {
    Copy-Item $helper (Join-Path $dataDir "frdx-helper.exe") -Force
    Write-Host "  + mods\frdx\frdx-helper.exe" -ForegroundColor Yellow
}
if (Test-Path (Join-Path $dist "mods\frdx\logos")) {
    New-Item -ItemType Directory -Force -Path (Join-Path $dataDir "logos") | Out-Null
    Copy-Item (Join-Path $dist "mods\frdx\logos\*") (Join-Path $dataDir "logos") -Force
    Write-Host "  + mods\frdx\logos\" -ForegroundColor Yellow
}
if (Test-Path (Join-Path $dist "mods\frdx\fonts\FusionPixel12-ja.ttf")) {
    Copy-Item (Join-Path $dist "mods\frdx\fonts\*") (Join-Path $dataDir "fonts") -Force
    Write-Host "  + mods\frdx\fonts\" -ForegroundColor Yellow
}

Write-Host "`nDone. Launch the game and set Audio -> Streamer Mode = On." -ForegroundColor Green
Write-Host "The in-game overlay opens automatically; press Insert to show/hide it."
