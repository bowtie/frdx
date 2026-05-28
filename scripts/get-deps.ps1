# Downloads all third-party dependencies into third_party/.
# Run this once after cloning the project, before configuring CMake.
#
#   PS> .\scripts\get-deps.ps1
#
# Re-runs are safe -- existing files are overwritten.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tp   = Join-Path $root "third_party"

function Fetch-File([string]$url, [string]$target) {
    $dir = Split-Path -Parent $target
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Write-Host "  -> $(Split-Path -Leaf $target)" -ForegroundColor Cyan
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $target
}

# ── nlohmann/json single header ───────────────────────────────────────────
Write-Host "-> nlohmann/json.hpp" -ForegroundColor Green
Fetch-File `
    "https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp" `
    (Join-Path $tp "nlohmann/nlohmann/json.hpp")

# ── toml11 single header ──────────────────────────────────────────────────
Write-Host "-> toml11.hpp" -ForegroundColor Green
Fetch-File `
    "https://raw.githubusercontent.com/ToruNiina/toml11/main/single_include/toml.hpp" `
    (Join-Path $tp "toml11/toml.hpp")

# ── Dear ImGui ────────────────────────────────────────────────────────────
Write-Host "-> Dear ImGui" -ForegroundColor Green
$imgui_files = @(
    "imgui.h", "imgui.cpp", "imgui_internal.h",
    "imstb_rectpack.h", "imstb_textedit.h", "imstb_truetype.h",
    "imgui_draw.cpp", "imgui_widgets.cpp", "imgui_tables.cpp",
    "imgui_demo.cpp", "imconfig.h",
    "backends/imgui_impl_win32.h", "backends/imgui_impl_win32.cpp",
    "backends/imgui_impl_dx12.h", "backends/imgui_impl_dx12.cpp"
)
foreach ($file in $imgui_files) {
    $url = "https://raw.githubusercontent.com/ocornut/imgui/master/$file"
    # Map backends/ to root of imgui for simple compilation includes
    $local = if ($file.StartsWith("backends/")) { $file.Substring(9) } else { $file }
    Fetch-File $url (Join-Path $tp "imgui/$local")
}

# ── MinHook ───────────────────────────────────────────────────────────────
Write-Host "-> MinHook" -ForegroundColor Green
$minhook_files = @(
    "include/MinHook.h",
    "src/buffer.c", "src/buffer.h",
    "src/trampoline.c", "src/trampoline.h",
    "src/hook.c",
    "src/hde/hde64.c", "src/hde/hde64.h",
    "src/hde/table64.h", "src/hde/pstdint.h"
)
foreach ($file in $minhook_files) {
    $url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/$file"
    # Flatten: hde/ files go in minhook/hde/, rest in minhook/
    $name = Split-Path -Leaf $file
    $sub  = if ($file.Contains("hde/")) { "hde" } else { "" }
    $target = if ($sub) { Join-Path $tp "minhook/$sub/$name" } else { Join-Path $tp "minhook/$name" }
    Fetch-File $url $target
}

Write-Host "`nAll dependencies fetched into $tp." -ForegroundColor Green
