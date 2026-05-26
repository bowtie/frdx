# Pulls the FH6 radio-station overlay files (RadioInfo XMLs, FMOD banks,
# Anthem.zip UI atlas) out of an existing radio-mod ZIP. They're modified
# copies of game assets, so the repo doesn't ship them.
#
#   PS> .\scripts\fetch-media.ps1 -Source "C:\path\to\radio-mod.zip"
#
# Output lands in dist/media/ ready for install.ps1.

param(
    [Parameter(Mandatory = $true)] [string] $Source,
    [string] $Out = (Join-Path (Split-Path -Parent $PSScriptRoot) "dist\media")
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $Source)) { throw "Source not found: $Source" }
if (-not (Test-Path $Out))    { New-Item -ItemType Directory -Force -Path $Out | Out-Null }

$tmp = Join-Path $env:TEMP ("fh6-media-" + [guid]::NewGuid())
Expand-Archive -Path $Source -DestinationPath $tmp -Force

# ZIP can have a top-level media\ folder or the assets at root.
$src = Get-ChildItem -Recurse -Filter "media" -Directory -Path $tmp |
       Select-Object -First 1
if (-not $src) {
    $src = Get-ChildItem -Recurse -Filter "RadioInfo_EN.xml" -Path $tmp |
           Select-Object -First 1 | ForEach-Object { $_.Directory.Parent }
}
if (-not $src) { Remove-Item -Recurse -Force $tmp; throw "No media files found inside $Source" }

Copy-Item -Recurse -Force (Join-Path $src.FullName "*") $Out
Remove-Item -Recurse -Force $tmp

Write-Host "Media overlay extracted to $Out" -ForegroundColor Green
Get-ChildItem -Recurse -File $Out | ForEach-Object {
    "  $($_.FullName.Substring($Out.Length + 1))  ($([math]::Round($_.Length/1KB,1)) KB)"
}
