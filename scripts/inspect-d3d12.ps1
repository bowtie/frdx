$env:PSStyle.OutputRendering = 'PlainText'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um'
$d3d12   = Join-Path $sdkRoot 'd3d12.h'
if (-not (Test-Path $d3d12)) {
    $fallback = Get-ChildItem (Join-Path $sdkRoot 'd3d1*.h') -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($fallback) { $d3d12 = $fallback.FullName }
    else { Write-Host 'd3d12.h not found'; exit 1 }
}
Write-Host ("Reading: {0}" -f $d3d12)
$lines = Get-Content $d3d12

# Helper: print a C++ style interface block starting at $startIndex.
function Print-Block($lines, $startIndex, $label, $maxLines) {
    Write-Host ("")
    Write-Host ("=== {0} (starts at line {1}) ===" -f $label, $startIndex)
    for ($j = $startIndex; $j -lt ([Math]::Min($startIndex + $maxLines, $lines.Count)); $j++) {
        $trunc = $lines[$j]
        if ($trunc.Length -gt 110) { $trunc = $trunc.Substring(0, 110) + '...' }
        Write-Host ("{0,6}  {1}" -f $j, $trunc)
    }
}

# Find ID3D12Object C++ definition (interior, non-forward decl).
$objStart = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'MIDL_INTERFACE') {
        for ($k = $i + 1; $k -lt [Math]::Min($i + 6, $lines.Count); $k++) {
            if ($lines[$k] -match '^\s*ID3D12Object\s*:') { $objStart = $k; break }
        }
        if ($objStart -ge 0) { break }
    }
}
if ($objStart -ge 0) { Print-Block $lines $objStart 'ID3D12Object C++ block' 35 }

# Find ID3D12DeviceChild C++ definition.
$dcStart = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'MIDL_INTERFACE') {
        for ($k = $i + 1; $k -lt [Math]::Min($i + 6, $lines.Count); $k++) {
            if ($lines[$k] -match '^\s*ID3D12DeviceChild\s*:') { $dcStart = $k; break }
        }
        if ($dcStart -ge 0) { break }
    }
}
if ($dcStart -ge 0) { Print-Block $lines $dcStart 'ID3D12DeviceChild C++ block' 25 }

# Find ID3D12Pageable C++ definition.
$pgStart = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'MIDL_INTERFACE') {
        for ($k = $i + 1; $k -lt [Math]::Min($i + 6, $lines.Count); $k++) {
            if ($lines[$k] -match '^\s*ID3D12Pageable\s*:') { $pgStart = $k; break }
        }
        if ($pgStart -ge 0) { break }
    }
}
if ($pgStart -ge 0) { Print-Block $lines $pgStart 'ID3D12Pageable C++ block' 15 }

# Find ID3D12CommandQueue C++ definition.
$qStart = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'MIDL_INTERFACE') {
        for ($k = $i + 1; $k -lt [Math]::Min($i + 6, $lines.Count); $k++) {
            if ($lines[$k] -match '^\s*ID3D12CommandQueue\s*:') { $qStart = $k; break }
        }
        if ($qStart -ge 0) { break }
    }
}
if ($qStart -ge 0) { Print-Block $lines $qStart 'ID3D12CommandQueue C++ block' 80 }
