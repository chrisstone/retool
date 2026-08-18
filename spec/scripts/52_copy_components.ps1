#Requires -RunAsAdministrator
<#
.SYNOPSIS
    52_copy_components.ps1 - Test: retool copy -c:<flags> and -ca:<flags>

.DESCRIPTION
    -c: selects which components are copied per file (D=Data A=Attribs T=Times
    S=Security O=Owner; default DATSO). -ca: further masks which attribute bits
    the A component copies (R/A/S/H; default RASH). Every other copy test in
    this suite uses the default DATSO/RASH, so timestamp/attribute preservation
    (and correct exclusion when a component is left out) has never actually been
    verified - only data hashes have.

    Sections:
    1. -c:D (data only): data must be correct, but the destination's timestamp
       must NOT match a deliberately-backdated source timestamp (T excluded).
    2. -c:DAT (data+attribs+times): destination timestamp must match source,
       and a ReadOnly bit set on the source must carry over (A included, default
       -ca:RASH covers R).
    3. -ca: mask filtering: source has both ReadOnly and Hidden set; copying with
       -c:DA -ca:R must carry over ReadOnly but NOT Hidden.
    4. -c:AT (no D, metadata-only mode): against an existing destination, only
       metadata should update; against a non-existent destination, the file
       should be skipped with a warning rather than erroring.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy - component selection (-c: / -ca:)" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''
$backdate = Get-Date '2019-06-15T08:00:00'

function New-SourceFile {
    param([string] $Path, [int] $Seed)
    $rng = [System.Random]::new($Seed)
    $buf = [byte[]]::new(65536)
    $rng.NextBytes($buf)
    [System.IO.File]::WriteAllBytes($Path, $buf)
    return (Get-FileHash $Path -Algorithm SHA256).Hash
}

function Clear-ReadOnly {
    param([string] $Path)
    if (Test-Path $Path) {
        $item = Get-Item $Path -Force
        $item.Attributes = $item.Attributes -band (-bnot [System.IO.FileAttributes]::ReadOnly)
    }
}

# ===============================================================================
# Section 1: -c:D (data only, times NOT copied)
# ===============================================================================
Write-Section "Section 1: -c:D (data only)"

$src1 = "{0}:\comp_d_src.bin" -f $drv
$dest1 = "{0}:\comp_d_dest.bin" -f $drv
Remove-DriveFile -Path $src1
Remove-DriveFile -Path $dest1
$hash1 = New-SourceFile -Path $src1 -Seed 1101
(Get-Item $src1).LastWriteTime = $backdate

$out = Invoke-Retool -Args @('copy', $src1, $dest1, '-c:D')
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "-c:D copy exits 0"
Assert-HashMatch -Path $dest1 -ExpectedHash $hash1 -Description "-c:D data correct"

if ((Get-Item $dest1).LastWriteTime -ne $backdate) {
    Write-Host "    [PASS] Destination timestamp NOT backdated (T excluded from -c:D)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Destination timestamp was backdated even though T was excluded" -ForegroundColor Red
    $script:TestsFailed++
}

Remove-DriveFile -Path $src1
Remove-DriveFile -Path $dest1

# ===============================================================================
# Section 2: -c:DAT (data + attribs + times)
# ===============================================================================
Write-Section "Section 2: -c:DAT (data, attribs, times)"

$src2 = "{0}:\comp_dat_src.bin" -f $drv
$dest2 = "{0}:\comp_dat_dest.bin" -f $drv
Remove-DriveFile -Path $src2
Remove-DriveFile -Path $dest2
$hash2 = New-SourceFile -Path $src2 -Seed 1102
(Get-Item $src2).LastWriteTime = $backdate
$srcItem2 = Get-Item $src2
$srcItem2.Attributes = $srcItem2.Attributes -bor [System.IO.FileAttributes]::ReadOnly

$out = Invoke-Retool -Args @('copy', $src2, $dest2, '-c:DAT')
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "-c:DAT copy exits 0"
Assert-HashMatch -Path $dest2 -ExpectedHash $hash2 -Description "-c:DAT data correct"

if ((Get-Item $dest2 -Force).LastWriteTime -eq $backdate) {
    Write-Host "    [PASS] Destination timestamp matches backdated source (T included)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Destination timestamp does not match source" -ForegroundColor Red
    $script:TestsFailed++
}

if (((Get-Item $dest2 -Force).Attributes -band [System.IO.FileAttributes]::ReadOnly) -ne 0) {
    Write-Host "    [PASS] ReadOnly attribute carried over (A included, default -ca:RASH covers R)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] ReadOnly attribute did not carry over" -ForegroundColor Red
    $script:TestsFailed++
}

Clear-ReadOnly -Path $src2
Clear-ReadOnly -Path $dest2
Remove-DriveFile -Path $src2
Remove-DriveFile -Path $dest2

# ===============================================================================
# Section 3: -ca: mask filtering (ReadOnly copied, Hidden excluded)
# ===============================================================================
Write-Section "Section 3: -ca:R (attribute mask excludes Hidden)"

$src3 = "{0}:\comp_mask_src.bin" -f $drv
$dest3 = "{0}:\comp_mask_dest.bin" -f $drv
Remove-DriveFile -Path $src3
Remove-DriveFile -Path $dest3
$hash3 = New-SourceFile -Path $src3 -Seed 1103
$srcItem3 = Get-Item $src3
$srcItem3.Attributes = $srcItem3.Attributes -bor [System.IO.FileAttributes]::ReadOnly -bor [System.IO.FileAttributes]::Hidden

$out = Invoke-Retool -Args @('copy', $src3, $dest3, '-c:DA', '-ca:R')
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "-c:DA -ca:R copy exits 0"
Assert-HashMatch -Path $dest3 -ExpectedHash $hash3 -Description "-ca:R data correct"

$destAttrs3 = (Get-Item $dest3 -Force).Attributes
if (($destAttrs3 -band [System.IO.FileAttributes]::ReadOnly) -ne 0) {
    Write-Host "    [PASS] ReadOnly carried over (R is in -ca: mask)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] ReadOnly did not carry over despite being in -ca: mask" -ForegroundColor Red
    $script:TestsFailed++
}
if (($destAttrs3 -band [System.IO.FileAttributes]::Hidden) -eq 0) {
    Write-Host "    [PASS] Hidden did NOT carry over (H excluded from -ca:R mask)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Hidden carried over despite being excluded from -ca: mask" -ForegroundColor Red
    $script:TestsFailed++
}

Clear-ReadOnly -Path $src3
Clear-ReadOnly -Path $dest3
Remove-DriveFile -Path $src3
Remove-DriveFile -Path $dest3

# ===============================================================================
# Section 4: -c:AT (no D, metadata-only mode)
# ===============================================================================
Write-Section "Section 4: -c:AT (metadata-only, no data component)"

$src4 = "{0}:\comp_meta_src.bin" -f $drv
$dest4 = "{0}:\comp_meta_dest.bin" -f $drv
$dest4missing = "{0}:\comp_meta_missing.bin" -f $drv
Remove-DriveFile -Path $src4
Remove-DriveFile -Path $dest4
Remove-DriveFile -Path $dest4missing
$hash4 = New-SourceFile -Path $src4 -Seed 1104

# Pre-seed dest4 with different content than src4, then update its timestamp
# via -c:AT and confirm the DATA was left untouched (D excluded).
[System.IO.File]::WriteAllBytes($dest4, [byte[]]::new(65536))
$preHash4 = (Get-FileHash $dest4 -Algorithm SHA256).Hash
(Get-Item $src4).LastWriteTime = $backdate

$out = Invoke-Retool -Args @('copy', $src4, $dest4, '-c:AT')
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "-c:AT (existing dest) copy exits 0"
Assert-HashMatch -Path $dest4 -ExpectedHash $preHash4 -Description "-c:AT left destination data untouched (D excluded)"
if ((Get-Item $dest4 -Force).LastWriteTime -eq $backdate) {
    Write-Host "    [PASS] Destination timestamp updated to match source (T included)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Destination timestamp was not updated" -ForegroundColor Red
    $script:TestsFailed++
}

# -c:AT against a destination that does not exist yet: should skip with a
# warning, not error, and must not create the destination (no data to copy).
$out = Invoke-Retool -Args @('copy', $src4, $dest4missing, '-c:AT')
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "-c:AT (missing dest) exits 0 (skip, not error)"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Skipping' -Description "Output reports the file was skipped"
if (-not (Test-Path $dest4missing)) {
    Write-Host "    [PASS] Destination was not created (no D component, nothing to copy)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Destination was created despite -c:AT having no D component" -ForegroundColor Red
    $script:TestsFailed++
}

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $src4
Remove-DriveFile -Path $dest4
Remove-DriveFile -Path $dest4missing

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
