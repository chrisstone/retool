#Requires -RunAsAdministrator
<#
.SYNOPSIS
    57_copy_cross_volume_recursive_dedup.ps1 - Test: cross-volume recursive
    directory copy, and within-batch cross-volume dedup preservation.

.DESCRIPTION
    31_copy_same_volume_recursive.ps1 only tests -r within the same volume.
    Cross-volume -r (CrossVolumeRefsCopyStrategy handling multiple files, plus
    its within-operation LCN-map dedup preservation across the whole batch) is
    never exercised.

    Sections:
    1. Directory copy without -r is rejected ("Source is a directory but -r
       (recursive) was not specified.", copy.cpp:1310).
    2. Cross-volume recursive copy (Drive A -> Drive B) mirrors a directory tree
       and every file's hash is correct.
    3. Within-batch cross-volume dedup preservation: the source directory
       contains file2.bin cloned from file1.bin (so they share physical blocks
       on Drive A before the copy even starts). After copying the whole
       directory to Drive B in one recursive operation, the destination copies
       must still share physical blocks with each other (verified via
       retool inspect on the destination pair) rather than each being written
       out as a full independent physical copy.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy - cross-volume recursive directory copy and dedup preservation" -ForegroundColor White

$drvA = $env:RETOOL_DRIVE_A -replace ':',''
$drvB = $env:RETOOL_DRIVE_B -replace ':',''

# ===============================================================================
# Section 1: directory copy without -r is rejected
# ===============================================================================
Write-Section "Section 1: directory copy without -r"

$srcDirNoR = "{0}:\xvol_norec_src" -f $drvA
$dstDirNoR = "{0}:\xvol_norec_dst" -f $drvA
Remove-Item $srcDirNoR -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dstDirNoR -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $srcDirNoR | Out-Null
Copy-Item $env:RETOOL_TEST_FILE ("{0}\f.bin" -f $srcDirNoR) -Force

$out = Invoke-Retool -Args @('copy', $srcDirNoR, $dstDirNoR) -AllowFailure
Assert-ExitCode -Expected 2 -Actual $LASTEXITCODE -Description "Directory copy without -r fails (exit 2)"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring '-r (recursive) was not specified' -Description "Reports the missing -r error"

Remove-Item $srcDirNoR -Recurse -Force -ErrorAction SilentlyContinue

# ===============================================================================
# Section 2 + 3: cross-volume recursive copy with within-batch dedup preservation
# ===============================================================================
Write-Section "Section 2+3: cross-volume recursive copy and dedup preservation"

$srcDir = "{0}:\xvol_rec_src" -f $drvA
$dstDir = "{0}:\xvol_rec_dst" -f $drvB
Remove-Item $srcDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dstDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $srcDir | Out-Null
New-Item -ItemType Directory -Path ("{0}\sub" -f $srcDir) | Out-Null

$file1 = "{0}\file1.bin" -f $srcDir
$file2 = "{0}\file2.bin" -f $srcDir
$file3 = "{0}\sub\file3.bin" -f $srcDir
Copy-Item $env:RETOOL_TEST_FILE $file1 -Force

# file2 is a same-volume CLONE of file1 (physically shares blocks on Drive A
# before the cross-volume copy even starts).
Invoke-Retool -Args @('copy', $file1, $file2) | Out-Null
& $env:RETOOL_EXE copy $file1 $file2 | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Arrange: same-volume clone of file1 -> file2 exits 0"
Assert-HashMatch -Path $file2 -ExpectedHash $env:RETOOL_TEST_HASH -Description "Arrange: file2 is a correct clone"

Copy-Item $env:RETOOL_TEST_FILE $file3 -Force

$out = Invoke-Retool -Args @('copy', $srcDir, $dstDir, '-r')
& $env:RETOOL_EXE copy $srcDir $dstDir -r | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Cross-volume recursive copy exits 0"
$outStr = $out -join "`n"

$destFile1 = "{0}\file1.bin" -f $dstDir
$destFile2 = "{0}\file2.bin" -f $dstDir
$destFile3 = "{0}\sub\file3.bin" -f $dstDir
Assert-FileExists -Path $destFile1 -Description "dstDir\file1.bin created"
Assert-FileExists -Path $destFile2 -Description "dstDir\file2.bin created"
Assert-FileExists -Path $destFile3 -Description "dstDir\sub\file3.bin created"
Assert-HashMatch -Path $destFile1 -ExpectedHash $env:RETOOL_TEST_HASH -Description "file1.bin data correct on Drive B"
Assert-HashMatch -Path $destFile2 -ExpectedHash $env:RETOOL_TEST_HASH -Description "file2.bin data correct on Drive B"
Assert-HashMatch -Path $destFile3 -ExpectedHash $env:RETOOL_TEST_HASH -Description "file3.bin data correct on Drive B"

if ($outStr -match 'Cloned Files\s*(\d+)') {
    if ([int]$matches[1] -ge 1) {
        Write-Host ("    [PASS] Summary reports Cloned Files >= 1 ({0})" -f $matches[1]) -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Summary reports 0 Cloned Files despite duplicate content in the batch" -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Cloned Files from summary" -ForegroundColor Red
    $script:TestsFailed++
}

# The real proof: destination file1/file2 must still physically share blocks,
# not just have matching content (which a naive independent copy would also
# produce) - same technique as 27_inspect_inputs_and_sharing.ps1's Section 3.
$out = Invoke-Retool -Args @('inspect', $destFile1, $destFile2, '-e')
& $env:RETOOL_EXE inspect $destFile1 $destFile2 -e | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "inspect of destination pair exits 0"
$outStr = $out -join "`n"
if ($outStr -match 'Shared Blocks\s*[\d.]+ (\w+)?\s*\((\d+) bytes\)') {
    $sharedBytes = [int64]$matches[2]
    if ($sharedBytes -gt 0) {
        Write-Host ("    [PASS] Destination file1/file2 physically share blocks ({0} bytes) - dedup preserved across the cross-volume batch" -f $sharedBytes) -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Destination file1/file2 share zero blocks - within-batch dedup was NOT preserved cross-volume" -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Shared Blocks from destination inspect" -ForegroundColor Red
    $script:TestsFailed++
}

# -- Cleanup -------------------------------------------------------------------
Remove-Item $srcDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dstDir -Recurse -Force -ErrorAction SilentlyContinue

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
