# Downloads Dear ImGui and MinHook into third_party/ for static compilation.
# Run this from the root of the project.
#
#   PS> .\scripts\get-imgui-minhook.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tp   = Join-Path $root "third_party"

$imgui_files = @(
    "imgui.h", "imgui.cpp", "imgui_internal.h", 
    "imstb_rectpack.h", "imstb_textedit.h", "imstb_truetype.h",
    "imgui_draw.cpp", "imgui_widgets.cpp", "imgui_tables.cpp", 
    "imgui_demo.cpp", "imconfig.h",
    "backends/imgui_impl_win32.h", "backends/imgui_impl_win32.cpp",
    "backends/imgui_impl_dx12.h", "backends/imgui_impl_dx12.cpp"
)

$minhook_files = @(
    "include/MinHook.h",
    "src/buffer.c", "src/buffer.h",
    "src/trampoline.c", "src/trampoline.h",
    "src/hook.c",
    "src/hde/hde64.c", "src/hde/hde64.h",
    "src/hde/table64.h", "src/hde/pstdint.h"
)

# Fetch ImGui
Write-Host "-> Fetching Dear ImGui..." -ForegroundColor Green
foreach ($file in $imgui_files) {
    $url = "https://raw.githubusercontent.com/ocornut/imgui/master/$file"
    # Map backends/ to root of imgui for simple compilation includes
    $local_path = $file
    if ($file.StartsWith("backends/")) {
        $local_path = $file.Substring(9)
    }
    $target = Join-Path $tp "imgui/$local_path"
    $dir    = Split-Path -Parent $target
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Write-Host "  -> imgui/$local_path" -ForegroundColor Cyan
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $target
}

# Fetch MinHook
Write-Host "`n-> Fetching MinHook..." -ForegroundColor Green
foreach ($file in $minhook_files) {
    $url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/$file"
    # Flatten minhook directory structure
    $name = Split-Path -Leaf $file
    if ($file.Contains("hde/")) {
        $target = Join-Path $tp "minhook/hde/$name"
    } else {
        $target = Join-Path $tp "minhook/$name"
    }
    $dir = Split-Path -Parent $target
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Write-Host "  -> minhook/$name" -ForegroundColor Cyan
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $target
}

Write-Host "`nAll ImGui and MinHook files fetched." -ForegroundColor Green
