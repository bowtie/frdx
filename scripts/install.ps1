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

Backup-AndCopy (Join-Path $dist "version.dll") (Join-Path $GameDir "version.dll") | Out-Host

$dataDir = Join-Path $GameDir "mods\frdx"
if (-not (Test-Path $dataDir)) { New-Item -ItemType Directory -Force -Path $dataDir | Out-Null }
New-Item -ItemType Directory -Force -Path (Join-Path $dataDir "fonts") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dataDir "ffmpeg") | Out-Null

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
if (Test-Path (Join-Path $dist "mods\frdx\fonts\FusionPixel12-ja.ttf")) {
    Copy-Item (Join-Path $dist "mods\frdx\fonts\*") (Join-Path $dataDir "fonts") -Force
    Write-Host "  + mods\frdx\fonts\" -ForegroundColor Yellow
}
if (Test-Path (Join-Path $dist "mods\frdx\ffmpeg\ffmpeg.exe")) {
    Copy-Item (Join-Path $dist "mods\frdx\ffmpeg\ffmpeg.exe") (Join-Path $dataDir "ffmpeg\ffmpeg.exe") -Force
    Write-Host "  + mods\frdx\ffmpeg\ffmpeg.exe" -ForegroundColor Yellow
}

Write-Host "`nDone. Launch the game, set Audio -> Radio DJ = Off, Streamer Mode = On." -ForegroundColor Green
Write-Host "The in-game overlay opens automatically; press F10 or Insert to show/hide it."
