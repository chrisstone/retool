#Requires -RunAsAdministrator
<#
.SYNOPSIS
    39_copy_edge_sizes.ps1 - Regression test: retool copy of a non-cluster-aligned
    file and a genuinely sparse (real holes) file.

.DESCRIPTION
    RETOOL_TEST_FILE (50 MB) divides evenly into both 64K and 4K clusters, so no
    other copy test in this suite ever exercises a file whose last cluster is
    partial, or a file with an actual unallocated (sparse) hole between two
    allocated regions - both are classic places for off-by-one / "read past
    logical EOF" style bugs, similar in spirit to the interval-offset bug found
    in dedup.cpp.

    Sections:
    1. Non-cluster-aligned file size (3.5 clusters at 64K = 229376 bytes):
       same-volume and cross-volume copy.
    2. Genuinely sparse file: 2 clusters of real data, a 4-cluster unallocated
       hole (created via `fsutil sparse setflag` + a seek-ahead write, not just
       a zero-filled write), then 2 more clusters of real data. Same-volume and
       cross-volume copy. Expected content is computed analytically (real data
       + zero bytes for the hole - matching what ReadFile transparently returns
       for a sparse gap) rather than by reading the source back.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy - non-aligned size and genuinely sparse files" -ForegroundColor White

$drvA = $env:RETOOL_DRIVE_A -replace ':',''
$drvB = $env:RETOOL_DRIVE_B -replace ':',''
$cs   = 65536  # Drive A/B cluster size

# ===============================================================================
# Section 1: Non-cluster-aligned file size (partial trailing cluster)
# ===============================================================================
Write-Section "Section 1: Non-cluster-aligned file size (3.5 clusters)"

$src1 = "{0}:\unaligned_src.bin" -f $drvA
Remove-DriveFile -Path $src1
$size1 = [int]($cs * 3.5)  # 229376 bytes - not a multiple of 65536
$rng1 = [System.Random]::new(601)
$buf1 = [byte[]]::new($size1)
$rng1.NextBytes($buf1)
[System.IO.File]::WriteAllBytes($src1, $buf1)
$hash1 = (Get-FileHash $src1 -Algorithm SHA256).Hash
Assert-FileExists -Path $src1 -Description "Non-aligned source file created (229376 bytes)"

# -- Same-volume ---
$dest1a = "{0}:\unaligned_same_dest.bin" -f $drvA
Remove-DriveFile -Path $dest1a
$out = Invoke-Retool -Args @('copy', $src1, $dest1a)
& $env:RETOOL_EXE copy $src1 $dest1a | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Same-volume copy of non-aligned file exits 0"
Assert-HashMatch -Path $dest1a -ExpectedHash $hash1 -Description "Same-volume destination data correct (trailing partial cluster intact)"
Remove-DriveFile -Path $dest1a

# -- Cross-volume ---
$dest1b = "{0}:\unaligned_xvol_dest.bin" -f $drvB
Remove-DriveFile -Path $dest1b
$out = Invoke-Retool -Args @('copy', $src1, $dest1b)
& $env:RETOOL_EXE copy $src1 $dest1b | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Cross-volume copy of non-aligned file exits 0"
Assert-HashMatch -Path $dest1b -ExpectedHash $hash1 -Description "Cross-volume destination data correct (trailing partial cluster intact)"
Assert-HashMatch -Path $src1 -ExpectedHash $hash1 -Description "Source unchanged"

Remove-DriveFile -Path $src1
Remove-DriveFile -Path $dest1b

# ===============================================================================
# Section 2: Genuinely sparse file (real allocation gap, not just zero-filled)
# ===============================================================================
Write-Section "Section 2: Genuinely sparse file (2 clusters data, 4 clusters hole, 2 clusters data)"

$src2 = "{0}:\sparse_src.bin" -f $drvA
Remove-DriveFile -Path $src2
New-Item -ItemType File -Path $src2 -Force | Out-Null

$fsutilOut = & fsutil sparse setflag $src2 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host ("    [FAIL] fsutil sparse setflag failed: {0}" -f $fsutilOut) -ForegroundColor Red
    $script:TestsFailed++
} else {
    $script:TestsPassed++
}

$rngHead = [System.Random]::new(602)
$rngTail = [System.Random]::new(603)
$bufHead = [byte[]]::new(2 * $cs)
$bufTail = [byte[]]::new(2 * $cs)
$rngHead.NextBytes($bufHead)
$rngTail.NextBytes($bufTail)

$fs = [System.IO.File]::Open($src2, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
try {
    $fs.Write($bufHead, 0, $bufHead.Length)          # clusters 0-1: real data
    $fs.Seek(6 * $cs, [System.IO.SeekOrigin]::Begin) | Out-Null
    $fs.Write($bufTail, 0, $bufTail.Length)          # clusters 6-7: real data (clusters 2-5 stay a hole)
} finally {
    $fs.Close()
}

$expectedSize = 8 * $cs
Assert-FileExists -Path $src2 -Description "Sparse source file created"
if ((Get-Item $src2).Length -eq $expectedSize) {
    Write-Host "    [PASS] Sparse file has expected size (524288 bytes)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host ("    [FAIL] Sparse file size wrong: expected {0}, got {1}" -f $expectedSize, (Get-Item $src2).Length) -ForegroundColor Red
    $script:TestsFailed++
}

# Expected content computed analytically: real head + zero hole + real tail.
$expectedBuf = [byte[]]::new($expectedSize)
[System.Array]::Copy($bufHead, 0, $expectedBuf, 0, $bufHead.Length)
[System.Array]::Copy($bufTail, 0, $expectedBuf, 6 * $cs, $bufTail.Length)
$sha256 = [System.Security.Cryptography.SHA256]::Create()
$hash2 = [System.BitConverter]::ToString($sha256.ComputeHash($expectedBuf)) -replace '-',''

Assert-HashMatch -Path $src2 -ExpectedHash $hash2 -Description "Source reads back as real-data/zero-hole/real-data (ReadFile fills the hole with zeros)"

# -- Same-volume ---
$dest2a = "{0}:\sparse_same_dest.bin" -f $drvA
Remove-DriveFile -Path $dest2a
$out = Invoke-Retool -Args @('copy', $src2, $dest2a)
& $env:RETOOL_EXE copy $src2 $dest2a | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Same-volume copy of sparse file exits 0"
Assert-HashMatch -Path $dest2a -ExpectedHash $hash2 -Description "Same-volume destination data correct (hole preserved as zeros)"
Remove-DriveFile -Path $dest2a

# -- Cross-volume ---
$dest2b = "{0}:\sparse_xvol_dest.bin" -f $drvB
Remove-DriveFile -Path $dest2b
$out = Invoke-Retool -Args @('copy', $src2, $dest2b)
& $env:RETOOL_EXE copy $src2 $dest2b | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Cross-volume copy of sparse file exits 0"
Assert-HashMatch -Path $dest2b -ExpectedHash $hash2 -Description "Cross-volume destination data correct (hole preserved as zeros)"
Assert-HashMatch -Path $src2 -ExpectedHash $hash2 -Description "Source unchanged"

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $src2
Remove-DriveFile -Path $dest2b

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
