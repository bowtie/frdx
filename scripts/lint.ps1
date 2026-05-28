#   PS> .\scripts\lint.ps1                # format + check (write changes)
#   PS> .\scripts\lint.ps1 -CheckOnly     # no changes, exit non-zero on issues
#
# Runs:
#   * clang-format on src/**.cpp + include/**.hpp
#   * run-clang-tidy -j8 -p build on src/  (requires compile_commands.json)
#   * prettier on ui/dist/**.{js,css,html}
#   * eslint on ui/dist/**.js
#   * stylelint on ui/dist/**.css

param(
    [switch]$CheckOnly,
    [switch]$SkipCpp,
    [switch]$SkipWeb,
    [switch]$SkipTidy
)

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"

function Find-LlvmTool([string]$exe) {
    # Check PATH first (winget/choco add it there after a restart or refresh).
    # Try with and without .exe — some tools like run-clang-tidy are
    # Python scripts with no extension.
    $cmd = Get-Command $exe -ErrorAction SilentlyContinue
    if (-not $cmd) { $cmd = Get-Command "$exe.exe" -ErrorAction SilentlyContinue }
    if ($cmd) { return $cmd.Source }

    $candidates = @()

    # Visual Studio bundled LLVM toolchain
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -property installationPath 2>$null
        if ($vs) {
            foreach ($arch in @("x64", "ARM64", "")) {
                $candidates += Join-Path $vs "VC\Tools\Llvm\$arch\bin\$exe.exe"
                $candidates += Join-Path $vs "VC\Tools\Llvm\$arch\bin\$exe"
            }
        }
    }

    # Standalone LLVM installs (winget: C:\Program Files\LLVM, choco: same or $env:ChocolateyInstall/lib)
    $candidates += "$env:ProgramFiles\LLVM\bin\$exe.exe"
    $candidates += "$env:ProgramFiles\LLVM\bin\$exe"
    $candidates += "${env:ProgramFiles(x86)}\LLVM\bin\$exe.exe"
    $candidates += "${env:ProgramFiles(x86)}\LLVM\bin\$exe"

    foreach ($p in $candidates) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

$fails = @()

if (-not $SkipCpp) {
    $clangFormat = Find-LlvmTool "clang-format"
    if (-not $clangFormat) {
        Write-Host "clang-format not found (install VS C++ workload or LLVM); skipping C++ format." -ForegroundColor Yellow
    } else {
        Write-Host "-> clang-format ($([IO.Path]::GetFileName($clangFormat)))" -ForegroundColor Cyan
        $cppFiles = @()
        $cppFiles += Get-ChildItem -Path (Join-Path $root "src")     -Recurse -Include *.cpp, *.hpp -File
        $cppFiles += Get-ChildItem -Path (Join-Path $root "include") -Recurse -Include *.hpp        -File
        $cppPaths = $cppFiles | ForEach-Object { $_.FullName }
        if ($CheckOnly) {
            & $clangFormat --dry-run --Werror @cppPaths
            if ($LASTEXITCODE -ne 0) { $fails += "clang-format" }
        } else {
            & $clangFormat -i @cppPaths
            if ($LASTEXITCODE -ne 0) { $fails += "clang-format" }
        }
    }

    if (-not $SkipTidy) {
        $runClangTidy = Find-LlvmTool "run-clang-tidy"
        if (-not $runClangTidy) {
            Write-Host "run-clang-tidy not found; skipping." -ForegroundColor Yellow
        } else {
            Write-Host "-> clang-tidy" -ForegroundColor Cyan
            $compileDb = Join-Path $build "compile_commands.json"
            if (-not (Test-Path $compileDb)) {
                Write-Host "compile_commands.json not found in build/; skipping." -ForegroundColor Yellow
            } else {
                # run-clang-tidy is a Python script on some installs — use
                # python.exe to invoke it so it works inside a pipeline.
                $tidyExe = $runClangTidy
                $tidyArgs = @("-j8", "-p", $build)
                if ((Get-Item $runClangTidy).Extension -eq '' -and
                    (Get-Command python -ErrorAction SilentlyContinue)) {
                    $tidyExe = (Get-Command python).Source
                    $tidyArgs = @($runClangTidy) + $tidyArgs
                }
                $prevEAP = $ErrorActionPreference
                $ErrorActionPreference = "Continue"
                $out = & $tidyExe @tidyArgs 2>&1 | Out-String
                $ErrorActionPreference = $prevEAP
                # Parse output: group warnings by file.
                $lines  = $out -split "`r?`n"
                $byFile = @{}
                foreach ($l in $lines) {
                    if ($l -match "^(.+\.cpp):(\d+):\d+: (warning|error): (.+)") {
                        $fname = Split-Path $Matches[1] -Leaf
                        if (-not $byFile.ContainsKey($fname)) { $byFile[$fname] = @() }
                        $byFile[$fname] += $l
                    }
                }
                if ($byFile.Count -gt 0) {
                    foreach ($fname in ($byFile.Keys | Sort-Object)) {
                        $diag = $byFile[$fname]
                        Write-Host "  ${fname}: $($diag.Count) diagnostic(s)" -ForegroundColor Yellow
                        $diag | ForEach-Object { Write-Host "    $_" }
                        $fails += "clang-tidy ($fname)"
                    }
                }
            }
        }
    }
}

if (-not $SkipWeb) {
    $npm = Get-Command npm -ErrorAction SilentlyContinue
    if (-not $npm) {
        Write-Host "npm not found; skipping web formatting/linting." -ForegroundColor Yellow
    } else {
        if (-not (Test-Path (Join-Path $root "node_modules"))) {
            Write-Host "-> npm install (first run)" -ForegroundColor Cyan
            Push-Location $root
            try   { & npm.cmd install --no-audit --no-fund | Out-Host }
            finally { Pop-Location }
            if ($LASTEXITCODE -ne 0) { $fails += "npm install"; }
        }

        Push-Location $root
        try {
            if ($CheckOnly) {
                Write-Host "-> npm run check" -ForegroundColor Cyan
                & npm.cmd run check
                if ($LASTEXITCODE -ne 0) { $fails += "web check" }
            } else {
                Write-Host "-> npm run fix" -ForegroundColor Cyan
                & npm.cmd run fix
                if ($LASTEXITCODE -ne 0) { $fails += "web fix" }
            }
        }
        finally { Pop-Location }
    }
}

if ($fails.Count -gt 0) {
    Write-Host ""
    Write-Host ("Failed: " + ($fails -join ", ")) -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "OK." -ForegroundColor Green
