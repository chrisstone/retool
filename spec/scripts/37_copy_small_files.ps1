#Requires -RunAsAdministrator
<#
.SYNOPSIS
    37_copy_small_files.ps1 - Regression test: retool copy of small/resident files.

.DESCRIPTION
    Regression test for a bug where SameVolumeCopyStrategy and
    CrossVolumeRefsCopyStrategy silently produced a correctly-*sized* but
    all-zero destination file when the source had no clonable extents -
    which NTFS/ReFS resident files (content stored inline in the file record,
    invisible to FSCTL_GET_RETRIEVAL_POINTERS) always hit. Every other copy
    test script in this suite (30-36) copies the same fixed 50 MB test file,
    which is far too large to ever be resident, so none of them exercise this
    code path. This script uses small text files (well under the resident
    threshold) specifically to close that gap.

    The fix routes a non-empty source with zero clonable bytes through the
    same attempt_fallback() path already used for an outright extents-query
    failure, so this script checks both the resulting data (hash match) and,
    where the same-volume/cross-volume clone strategies are in play, that the
    fallback path was actually taken (via its warning message) - confirming
    the fix's code path, not just an accidental correct result.

    IMPORTANT - why sections 1, 2 and 4 use C:\Temp (NTFS) as the source:
    ReFS has no concept of a resident file - every ReFS file, however small,
    is given a real allocated cluster and therefore has a real clonable
    extent. Only NTFS inlines small file content directly into the MFT
    record (invisible to FSCTL_GET_RETRIEVAL_POINTERS), so genuinely
    resident files can only be produced on the NTFS system drive, not on
    the ReFS test VHDXs. SameVolumeCopyStrategy is selected purely on
    "same volume root", independent of filesystem, so sourcing from
    C:\Temp still exercises the exact strategy under test. Section 3 does
    not depend on residency (its guard is src_size == 0) so it stays on
    the ReFS drive for consistency with the rest of the suite.

    Sections:
    1. Same-volume (C:\Temp -> C:\Temp): single small resident file
       (SameVolumeCopyStrategy).
    2. Same-volume (C:\Temp -> C:\Temp): recursive directory with small
       resident files nested two levels deep (mirrors the exact scenario
       that originally surfaced the bug).
    3. Same-volume (Drive A): zero-byte file (edge case - must NOT trigger
       the fallback path; a genuinely empty file has nothing to clone and
       that's correct).
    4. Cross-volume (C:\Temp -> whichever ReFS test drive shares the NTFS
       system drive's cluster size): single small resident file
       (CrossVolumeRefsCopyStrategy - the bug's other affected strategy;
       skipped if no test drive's cluster size matches, since that
       strategy wouldn't be selected on this machine anyway).
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy - small/resident files (zero-fill clone bug regression)" -ForegroundColor White

$drvA = $env:RETOOL_DRIVE_A -replace ':',''

<#
.SYNOPSIS
    Writes deterministic text content to $Path and returns its SHA-256 hash.
#>
function New-SmallTestFile {
    param([string] $Path, [string] $Content)
    [System.IO.File]::WriteAllText($Path, $Content, [System.Text.Encoding]::ASCII)
    return (Get-FileHash $Path -Algorithm SHA256).Hash
}

# ===============================================================================
# Section 1: Same-volume, single small file
# ===============================================================================
Write-Section "Section 1: Same-volume single small file (SameVolumeCopyStrategy)"

$src1  = 'C:\Temp\small_same_src.txt'
$dest1 = 'C:\Temp\small_same_dest.txt'
Remove-DriveFile -Path $dest1
$hash1 = New-SmallTestFile -Path $src1 -Content "A small resident file used to regression-test the clone bug.`n"
Assert-FileExists -Path $src1 -Description "Small source file created"

$out = Invoke-Retool -Args @('copy', $src1, $dest1)
& $env:RETOOL_EXE copy $src1 $dest1 | Out-Null
$exitCode = $LASTEXITCODE
$outStr = $out -join "`n"

Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
Assert-FileExists -Path $dest1 -Description "Destination file exists"
Assert-HashMatch -Path $dest1 -ExpectedHash $hash1 -Description "Destination data matches source (not zero-filled)"
Assert-HashMatch -Path $src1  -ExpectedHash $hash1 -Description "Source file unchanged"
Assert-OutputContains -Output $outStr -Substring 'No clonable extents found' `
    -Description "Fallback path was taken for the resident file (confirms the fix's code path fired)"

Remove-DriveFile -Path $src1
Remove-DriveFile -Path $dest1

# ===============================================================================
# Section 2: Same-volume, recursive directory of small files
# ===============================================================================
Write-Section "Section 2: Same-volume recursive directory of small files (-r)"

$srcDir2 = 'C:\Temp\small_same_dir_src'
$dstDir2 = 'C:\Temp\small_same_dir_dst'
Remove-Item $srcDir2 -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dstDir2 -Recurse -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Path $srcDir2 | Out-Null
New-Item -ItemType Directory -Path ("{0}\sub" -f $srcDir2) | Out-Null
New-Item -ItemType Directory -Path ("{0}\sub\nested" -f $srcDir2) | Out-Null

$hashRoot   = New-SmallTestFile -Path ("{0}\root.txt" -f $srcDir2)             -Content "root file`n"
$hashSub    = New-SmallTestFile -Path ("{0}\sub\sub.txt" -f $srcDir2)          -Content "sub file`n"
$hashNested = New-SmallTestFile -Path ("{0}\sub\nested\nested.txt" -f $srcDir2) -Content "nested file`n"

Assert-FileExists -Path ("{0}\root.txt" -f $srcDir2)             -Description "srcDir\root.txt present"
Assert-FileExists -Path ("{0}\sub\sub.txt" -f $srcDir2)           -Description "srcDir\sub\sub.txt present"
Assert-FileExists -Path ("{0}\sub\nested\nested.txt" -f $srcDir2) -Description "srcDir\sub\nested\nested.txt present"

$out = Invoke-Retool -Args @('copy', $srcDir2, $dstDir2, '-r')
& $env:RETOOL_EXE copy $srcDir2 $dstDir2 -r | Out-Null
$exitCode = $LASTEXITCODE

Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
Assert-FileExists -Path ("{0}\root.txt" -f $dstDir2)             -Description "dstDir\root.txt created"
Assert-FileExists -Path ("{0}\sub\sub.txt" -f $dstDir2)           -Description "dstDir\sub\sub.txt created"
Assert-FileExists -Path ("{0}\sub\nested\nested.txt" -f $dstDir2) -Description "dstDir\sub\nested\nested.txt created"
Assert-HashMatch -Path ("{0}\root.txt" -f $dstDir2)             -ExpectedHash $hashRoot   -Description "root.txt data correct (not zero-filled)"
Assert-HashMatch -Path ("{0}\sub\sub.txt" -f $dstDir2)           -ExpectedHash $hashSub    -Description "sub.txt data correct (not zero-filled)"
Assert-HashMatch -Path ("{0}\sub\nested\nested.txt" -f $dstDir2) -ExpectedHash $hashNested -Description "nested.txt data correct (not zero-filled)"
Assert-HashMatch -Path ("{0}\root.txt" -f $srcDir2)             -ExpectedHash $hashRoot   -Description "Source root.txt unchanged"
Assert-HashMatch -Path ("{0}\sub\sub.txt" -f $srcDir2)           -ExpectedHash $hashSub    -Description "Source sub.txt unchanged"
Assert-HashMatch -Path ("{0}\sub\nested\nested.txt" -f $srcDir2) -ExpectedHash $hashNested -Description "Source nested.txt unchanged"

$outStr2 = $out -join "`n"
$fallbackCount2 = ([regex]::Matches($outStr2, [regex]::Escape('No clonable extents found'))).Count
if ($fallbackCount2 -ge 3) {
    Write-Host ("    [PASS] Fallback path taken for all 3 nested resident files ({0} occurrences)" -f $fallbackCount2) -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host ("    [FAIL] Expected fallback path for all 3 resident files, saw {0} occurrence(s)" -f $fallbackCount2) -ForegroundColor Red
    $script:TestsFailed++
}

Remove-Item $srcDir2 -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dstDir2 -Recurse -Force -ErrorAction SilentlyContinue

# ===============================================================================
# Section 3: Same-volume, zero-byte file (must NOT trigger the fallback path)
# ===============================================================================
Write-Section "Section 3: Same-volume zero-byte file (edge case)"

$src3  = "{0}:\empty_src.txt" -f $drvA
$dest3 = "{0}:\empty_dest.txt" -f $drvA
Remove-DriveFile -Path $dest3
New-Item -ItemType File -Path $src3 -Force | Out-Null
Assert-FileExists -Path $src3 -Description "Zero-byte source file created"

$out = Invoke-Retool -Args @('copy', $src3, $dest3)
& $env:RETOOL_EXE copy $src3 $dest3 | Out-Null
$exitCode = $LASTEXITCODE
$outStr = $out -join "`n"

Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
Assert-FileExists -Path $dest3 -Description "Destination file exists"
if ((Get-Item $dest3).Length -eq 0) {
    Write-Host "    [PASS] Destination is zero-byte (correct - source is genuinely empty)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Destination is not zero-byte (expected 0, got $((Get-Item $dest3).Length))" -ForegroundColor Red
    $script:TestsFailed++
}
if ($outStr -notmatch [regex]::Escape('No clonable extents found')) {
    Write-Host "    [PASS] Fallback path NOT triggered for a genuinely empty file (src_size > 0 guard holds)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Fallback path was triggered for a zero-byte file (should take the fast empty-file path)" -ForegroundColor Red
    $script:TestsFailed++
}

Remove-DriveFile -Path $src3
Remove-DriveFile -Path $dest3

# ===============================================================================
# Section 4: Cross-volume, single small file (CrossVolumeRefsCopyStrategy)
# ===============================================================================
Write-Section "Section 4: Cross-volume single small file (CrossVolumeRefsCopyStrategy)"

# CrossVolumeRefsCopyStrategy additionally requires matching src/dest cluster
# sizes, so pick whichever ReFS test drive shares the NTFS system drive's
# cluster size - otherwise the copy would silently take FallbackCopyStrategy
# instead and this section would test nothing.
$ntfsClusterSize = (Get-Volume -DriveLetter 'C').AllocationUnitSize
$xvolDest = $null
if ($ntfsClusterSize -eq 65536) {
    $xvolDest = $env:RETOOL_DRIVE_B
} elseif ($ntfsClusterSize -eq 4096) {
    $xvolDest = $env:RETOOL_DRIVE_C
}

if (-not $xvolDest) {
    Write-Host ("    [SKIP] NTFS cluster size ({0}) matches neither ReFS test drive (65536 or 4096) - CrossVolumeRefsCopyStrategy would not be selected on this machine" -f $ntfsClusterSize) -ForegroundColor Yellow
} else {
    $src4  = 'C:\Temp\small_xvol_src.txt'
    $dest4 = "{0}\small_xvol_dest.txt" -f $xvolDest
    Remove-DriveFile -Path $dest4
    $hash4 = New-SmallTestFile -Path $src4 -Content "A small resident file for the cross-volume clone strategy.`n"
    Assert-FileExists -Path $src4 -Description "Small source file created on NTFS system drive"

    $out = Invoke-Retool -Args @('copy', $src4, $dest4)
    & $env:RETOOL_EXE copy $src4 $dest4 | Out-Null
    $exitCode = $LASTEXITCODE
    $outStr = $out -join "`n"

    Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
    Assert-FileExists -Path $dest4 -Description ("Destination file exists on {0}" -f $xvolDest)
    Assert-HashMatch -Path $dest4 -ExpectedHash $hash4 -Description "Destination data matches source (not zero-filled)"
    Assert-HashMatch -Path $src4  -ExpectedHash $hash4 -Description "Source file unchanged"
    Assert-OutputContains -Output $outStr -Substring 'No clonable extents found' `
        -Description "Fallback path was taken for the resident file (confirms the fix's code path fired)"

    Remove-DriveFile -Path $src4
    Remove-DriveFile -Path $dest4
}

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
