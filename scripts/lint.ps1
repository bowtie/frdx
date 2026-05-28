# Lints the C++ codebase with clang-format (style) and clang-tidy (analysis).
#
#   PS> .\scripts\lint.ps1                   # check formatting + run tidy (parallel)
#   PS> .\scripts\lint.ps1 -Fix              # auto-format, then tidy (parallel)
#   PS> .\scripts\lint.ps1 -SkipTidy         # formatting only
#   PS> .\scripts\lint.ps1 -SkipFormat       # tidy only (parallel)
#   PS> .\scripts\lint.ps1 -Jobs 8           # override parallel jobs (default = all cores)
#   PS> .\scripts\lint.ps1 -Incremental      # only lint files changed since HEAD
#

param(
    [switch] $Fix,
    [switch] $SkipTidy,
    [switch] $SkipFormat,
    [switch] $Incremental,
    [int]    $Jobs = 0
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$sw = [System.Diagnostics.Stopwatch]::StartNew()

# Number of logical processors for parallel tidy
if ($Jobs -le 0) {
    $Jobs = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
}

# ── Locate tools ──────────────────────────────────────────────────────────

function Find-Tool([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return @{ Path = $cmd.Source; Interpreter = $null } }

    $llvmBin = "${env:ProgramFiles}\LLVM\bin"
    $exePath = Join-Path $llvmBin "$name.exe"
    if (Test-Path $exePath) { return @{ Path = $exePath; Interpreter = $null } }

    # script without .exe (e.g. run-clang-tidy) -- found it
    $scriptPath = Join-Path $llvmBin $name
    if (Test-Path $scriptPath) {
        return @{ Path = $scriptPath }
    }

    return $null
}

$clangFormat = Find-Tool "clang-format"
$clangTidy   = Find-Tool "clang-tidy"

if (-not $clangFormat -and -not $SkipFormat) {
    Write-Host "clang-format not found." -ForegroundColor Yellow
    $SkipFormat = $true
}
if (-not $clangTidy -and -not $SkipTidy) {
    Write-Host "clang-tidy not found." -ForegroundColor Yellow
    $SkipTidy = $true
}

if ($SkipFormat -and $SkipTidy) {
    Write-Host "Nothing to do -- both clang-format and clang-tidy unavailable." -ForegroundColor Red
    exit 1
}

if ($clangFormat) { Write-Host "clang-format: $($clangFormat.Path)" -ForegroundColor DarkGray }
if (-not $SkipTidy -and $clangTidy) {
    Write-Host "clang-tidy:   $($clangTidy.Path) ($Jobs parallel job(s))" -ForegroundColor DarkGray
}

# Gather source files
$allCpp = @(Get-ChildItem -Recurse -Filter "*.cpp" -Path (Join-Path $root "src") | ForEach-Object { $_.FullName })
$allHpp = @(Get-ChildItem -Recurse -Filter "*.hpp" -Path (Join-Path $root "include") | ForEach-Object { $_.FullName })

if ($Incremental) {
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        Write-Host "git not found; falling back to full lint." -ForegroundColor Yellow
        $Incremental = $false
    }
}

if ($Incremental) {
    $changed = @(git -C $root diff --name-only --diff-filter=d HEAD -- src/ include/ 2>&1 |
        Where-Object { $_ -match '\.(cpp|hpp)$' } |
        ForEach-Object { Join-Path $root $_ })
    if ($changed.Count -eq 0) {
        Write-Host "No changed C++ files since HEAD. Nothing to lint." -ForegroundColor Green
        exit 0
    }
    $cppFiles = @($changed | Where-Object { $_ -match '\.cpp$' })
    $hppFiles = @($changed | Where-Object { $_ -match '\.hpp$' })
    Write-Host "Incremental mode: $($changed.Count) changed file(s)" -ForegroundColor DarkGray
} else {
    $cppFiles = $allCpp
    $hppFiles = $allHpp
}

# ── clang-format ──────────────────────────────────────────────────────────

$formatErrors = 0
if (-not $SkipFormat) {
    Write-Host "`n-> clang-format" -ForegroundColor Cyan
    $allSources = $cppFiles + $hppFiles

    if ($Fix) {
        & $($clangFormat.Path) -i -style=file $allSources 2>&1 | Out-Null
        Write-Host "  Formatted $($allSources.Count) files." -ForegroundColor Green
    } else {
        # --dry-run --Werror: exits non-zero if any file differs
        $output = & $($clangFormat.Path) -n -style=file --Werror $allSources 2>&1
        if ($LASTEXITCODE -ne 0) {
            $dirty = $output | Where-Object { $_ -match '^(.+?): warning:' } | ForEach-Object { $matches[1] } | Sort-Object -Unique
            foreach ($f in $dirty) {
                $rel = if ($f.Length -gt $root.Length) { $f.Substring($root.Length + 1) } else { $f }
                Write-Host "  !! $rel needs formatting" -ForegroundColor Yellow
                $formatErrors++
            }
        }

        if ($formatErrors -eq 0) {
            Write-Host "  All $($allSources.Count) files formatted correctly." -ForegroundColor Green
        } else {
            Write-Host "  $formatErrors file(s) need formatting (run with -Fix to auto-fix)." -ForegroundColor Yellow
        }
    }
}

# ── clang-tidy ────────────────────────────────────────────────────────────

$tidyErrors = 0
if (-not $SkipTidy) {
    Write-Host "`n-> clang-tidy" -ForegroundColor Cyan

    $cc = Join-Path $root "build\compile_commands.json"
    if (-not (Test-Path $cc)) {
        Write-Host "  build\compile_commands.json not found -- run build.ps1 first." -ForegroundColor Yellow
    } else {
        # Prefer run-clang-tidy (native LLVM parallel runner) over manual parallelism
        $runTidy = Find-Tool "run-clang-tidy"
        $python  = Get-Command python -ErrorAction SilentlyContinue
        if (-not $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
        $bp = Join-Path $root "build"

        if ($runTidy -and $python) {
            Write-Host "  Using run-clang-tidy ($Jobs parallel jobs)..." -ForegroundColor DarkGray

            $tidyArgs = @($runTidy.Path, "-p", $bp, "-j", $Jobs, "-quiet", "-warnings-as-errors=*")
            if ($Fix) { $tidyArgs += "-fix"; $tidyArgs += "-format" }
            # Escape backslashes for regex (run-clang-tidy matches via re.search)
            $tidyArgs += ($cppFiles | ForEach-Object { $_.Replace('\', '\\') })

            $out = & $python.Source @tidyArgs 2>&1
            $tidyExit = $LASTEXITCODE

            # run-clang-tidy prints warnings/errors to stderr; count non-zero exits
            if ($tidyExit -ne 0) {
                $out | ForEach-Object { Write-Host $_ -ForegroundColor Red }
                $tidyErrors = @($cppFiles).Count  # pessimistic; run-clang-tidy doesn't report per-file
            }
        } else {
            # Fallback: manual parallel via ForEach-Object -Parallel (PowerShell 7+)
            if (-not $runTidy) { Write-Host "  run-clang-tidy not found; falling back to ForEach-Object -Parallel" -ForegroundColor Yellow }
            elseif (-not $python) { Write-Host "  Python not found; falling back to ForEach-Object -Parallel (install: winget install Python.Python.3)" -ForegroundColor Yellow }
            Write-Host "  Running on $($cppFiles.Count) translation units ($Jobs parallel jobs)..." -ForegroundColor DarkGray

            $tidyPath = $clangTidy.Path

            $results = $cppFiles | ForEach-Object -Parallel {
                $file  = $_
                $rel   = $file.Substring($using:root.Length + 1)
                Write-Host "  tidy $rel" -ForegroundColor DarkGray

                $args = @("-p", $using:bp, "--warnings-as-errors=*", "--quiet")
                if ($using:Fix) { $args += "--fix"; $args += "--format" }

                $out = & $using:tidyPath @args $file 2>&1

                [PSCustomObject]@{ File = $rel; ExitCode = $LASTEXITCODE; Output = "$out" }
            } -ThrottleLimit $Jobs

            foreach ($r in $results) {
                if ($r.ExitCode -ne 0) {
                    Write-Host $r.Output -ForegroundColor Red
                    $tidyErrors++
                }
            }
        }

        if ($tidyErrors -eq 0) {
            Write-Host "  All $($cppFiles.Count) translation units clean." -ForegroundColor Green
        } else {
            Write-Host "  $tidyErrors file(s) have clang-tidy warnings." -ForegroundColor Yellow
        }
    }
}

# ── Summary ───────────────────────────────────────────────────────────────

$sw.Stop()
$exitCode = $formatErrors + $tidyErrors

if ($exitCode -eq 0) {
    Write-Host ("`nLint passed. ({0:f1}s)" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
} else {
    Write-Host ("`nLint finished with {0} issue(s) ({1:f1}s)" -f $exitCode, $sw.Elapsed.TotalSeconds) -ForegroundColor Yellow
}

exit $exitCode
