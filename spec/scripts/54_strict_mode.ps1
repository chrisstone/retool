#Requires -RunAsAdministrator
<#
.SYNOPSIS
    54_strict_mode.ps1 - Test: retool -s (strict mode) for copy and dedup.

.DESCRIPTION
    -s makes copy/dedup return the first error immediately instead of recording
    it and continuing (copy.cpp:1936, dedup.cpp:706/621). No test in this suite
    passes -s at all.

    Both copy::execute() and dedup::execute() share the same shape: on a strict
    abort, they `return std::unexpected(...)` from inside the processing loop,
    which means finalize_and_report() - the function that prints the closing
    summary section ("retool copy summary" / "retool dedup summary", with
    "Total Files" / "Clusters Deduped" etc.) - never runs. Without -s, the loop
    always finishes and finalize_and_report() always runs, regardless of any
    errors recorded along the way. That's a robust, order-independent signal to
    distinguish the two modes (unlike trying to infer it from which specific
    file in a multi-file batch succeeded, which would depend on filesystem
    enumeration order).

    A real failure is forced deterministically by pre-locking a file from
    PowerShell before retool runs:
      - Copy: locks the destination path exclusively (FileShare.None), so
        create_dest_file()'s CreateFileW (which requests
        FILE_SHARE_READ|FILE_SHARE_WRITE) hits a sharing violation immediately.
      - Dedup: locks the fileOp path itself with FileShare.Read (NOT None -
        dedup::prepare() volume-scans first and needs read access to even see
        fileOp; None would fail the scan itself, which happens before the
        strict-controlled loop and would short-circuit finalize_and_report()
        unconditionally regardless of -s). Read sharing lets the scan succeed
        while still blocking rebuild_file()'s later MoveFileW rename (step 1,
        before any mutation), which needs delete-sharing this doesn't grant.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: strict mode (-s) for copy and dedup" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''

# ===============================================================================
# Section 1: copy -s
# ===============================================================================
Write-Section "Section 1: copy -s"

$src1 = "{0}:\strict_copy_src.bin" -f $drv
Remove-DriveFile -Path $src1
$rng = [System.Random]::new(1201)
$buf = [byte[]]::new(65536)
$rng.NextBytes($buf)
[System.IO.File]::WriteAllBytes($src1, $buf)

$dest1a = "{0}:\strict_copy_dest_a.bin" -f $drv  # non-strict run
$dest1b = "{0}:\strict_copy_dest_b.bin" -f $drv  # strict run
Remove-DriveFile -Path $dest1a
Remove-DriveFile -Path $dest1b

$lockA = [System.IO.File]::Open($dest1a, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
$lockB = [System.IO.File]::Open($dest1b, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
try {
    # Non-strict: the failure is recorded, but finalize_and_report still runs.
    $out = Invoke-Retool -Args @('copy', $src1, $dest1a, '-t', '0') -AllowFailure
    & $env:RETOOL_EXE copy $src1 $dest1a -t 0 | Out-Null
    $outStr = $out -join "`n"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    [PASS] Non-strict copy against a locked destination fails (exit != 0)" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Non-strict copy unexpectedly succeeded against a locked destination" -ForegroundColor Red
        $script:TestsFailed++
    }
    Assert-OutputContains -Output $outStr -Substring 'Total Files' -Description "Non-strict: summary section IS printed (loop ran to completion)"

    # Strict: aborts immediately, summary section never printed.
    $out = Invoke-Retool -Args @('copy', $src1, $dest1b, '-t', '0', '-s') -AllowFailure
    & $env:RETOOL_EXE copy $src1 $dest1b -t 0 -s | Out-Null
    $outStr = $out -join "`n"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    [PASS] Strict copy against a locked destination fails (exit != 0)" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Strict copy unexpectedly succeeded against a locked destination" -ForegroundColor Red
        $script:TestsFailed++
    }
    if ($outStr -notmatch 'Total Files') {
        Write-Host "    [PASS] Strict: summary section NOT printed (aborted before finalize_and_report)" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Strict: summary section was printed despite -s" -ForegroundColor Red
        $script:TestsFailed++
    }
} finally {
    $lockA.Close()
    $lockB.Close()
}

Remove-DriveFile -Path $src1
Remove-DriveFile -Path $dest1a
Remove-DriveFile -Path $dest1b

# ===============================================================================
# Section 2: dedup -s
# ===============================================================================
Write-Section "Section 2: dedup -s"

$fileRef2a = "{0}:\strict_dedup_ref_a.bin" -f $drv
$fileOp2a  = "{0}:\strict_dedup_op_a.bin" -f $drv    # non-strict run
$fileRef2b = "{0}:\strict_dedup_ref_b.bin" -f $drv
$fileOp2b  = "{0}:\strict_dedup_op_b.bin" -f $drv    # strict run
foreach ($f in @($fileRef2a, $fileOp2a, $fileRef2b, $fileOp2b)) { Remove-DriveFile -Path $f }

# fileOp must genuinely duplicate fileRef, or rebuild_file()'s pre-check finds
# zero dedup candidates and never attempts the rename this test depends on -
# but the duplicate content doesn't need to be large. Drive A only has room
# for roughly one 50 MB file at a time (00_setup.ps1's rationale) and this
# section needs FOUR files live at once, so use small (128 KB / 2-cluster)
# fixtures instead - a prior version of this test used full 50 MB copies of
# RETOOL_TEST_FILE and reliably exhausted Drive A's free space.
$rngDedup = [System.Random]::new(1210)
$dedupContent = [byte[]]::new(2 * 65536)
$rngDedup.NextBytes($dedupContent)
[System.IO.File]::WriteAllBytes($fileRef2a, $dedupContent)
[System.IO.File]::WriteAllBytes($fileOp2a, $dedupContent)
[System.IO.File]::WriteAllBytes($fileRef2b, $dedupContent)
[System.IO.File]::WriteAllBytes($fileOp2b, $dedupContent)
$dedupHash = (Get-FileHash $fileRef2a -Algorithm SHA256).Hash

# Lock fileOp itself (not its .old target) with read-only sharing: this must
# still allow dedup's own volume-scan phase to open the file for reading
# (FileShare.None would make the SCAN itself fail to find fileOp at all -
# PairwiseDedupStrategy::build_plans() would then error with "Origin file not
# found in scan", which happens in execute() before the strict-controlled
# per-file loop is ever reached, short-circuiting finalize_and_report()
# unconditionally - defeating the strict-vs-non-strict distinction this test
# is for). FileShare.Read lets the scan succeed while still blocking
# rebuild_file()'s later MoveFileW rename (step 1, before any mutation),
# which needs delete-sharing this doesn't grant.
$lockOpA = [System.IO.File]::Open($fileOp2a, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read)
$lockOpB = [System.IO.File]::Open($fileOp2b, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read)
try {
    # Non-strict
    $out = Invoke-Retool -Args @('dedup', $fileRef2a, $fileOp2a) -AllowFailure
    & $env:RETOOL_EXE dedup $fileRef2a $fileOp2a | Out-Null
    $outStr = $out -join "`n"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    [PASS] Non-strict dedup against a locked fileOp fails (exit != 0)" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Non-strict dedup unexpectedly succeeded against a locked fileOp" -ForegroundColor Red
        $script:TestsFailed++
    }
    Assert-OutputContains -Output $outStr -Substring 'Clusters Deduped' -Description "Non-strict: summary section IS printed"

    # Strict
    $out = Invoke-Retool -Args @('dedup', $fileRef2b, $fileOp2b, '-s') -AllowFailure
    & $env:RETOOL_EXE dedup $fileRef2b $fileOp2b -s | Out-Null
    $outStr = $out -join "`n"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    [PASS] Strict dedup against a locked fileOp fails (exit != 0)" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Strict dedup unexpectedly succeeded against a locked fileOp" -ForegroundColor Red
        $script:TestsFailed++
    }
    if ($outStr -notmatch 'Clusters Deduped') {
        Write-Host "    [PASS] Strict: summary section NOT printed (aborted before finalize_and_report)" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Strict: summary section was printed despite -s" -ForegroundColor Red
        $script:TestsFailed++
    }
} finally {
    $lockOpA.Close()
    $lockOpB.Close()
}

# fileRef must be untouched throughout (it's read-only by role in pair-wise dedup).
Assert-HashMatch -Path $fileRef2a -ExpectedHash $dedupHash -Description "fileRef (non-strict run) untouched"
Assert-HashMatch -Path $fileRef2b -ExpectedHash $dedupHash -Description "fileRef (strict run) untouched"
# fileOp's rename never happened (locked before any mutation), so it must
# still be present at its original path with original content.
Assert-HashMatch -Path $fileOp2a -ExpectedHash $dedupHash -Description "fileOp (non-strict run) untouched (rename failed before any mutation)"
Assert-HashMatch -Path $fileOp2b -ExpectedHash $dedupHash -Description "fileOp (strict run) untouched (rename failed before any mutation)"

# -- Cleanup -------------------------------------------------------------------
foreach ($f in @($fileRef2a, $fileOp2a, $fileRef2b, $fileOp2b)) { Remove-DriveFile -Path $f }

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
