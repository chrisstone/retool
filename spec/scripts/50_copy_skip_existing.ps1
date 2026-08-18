#Requires -RunAsAdministrator
<#
.SYNOPSIS
    50_copy_skip_existing.ps1 - Test: retool copy <src> <dest> -xs

.DESCRIPTION
    -xs skips a file when the destination already exists with the same size and
    last-write timestamp as the source (should_skip_file() in copy.cpp). No test
    in this suite passes -xs at all.

    Sections:
    1. Positive case: copy once (creates a same-size, same-timestamp destination
       via the default T component), then copy again with -xs - the second run
       should skip and report so via a "Skipping" message, not touch the file.
    2. Negative case: after touching the destination's timestamp so it no longer
       matches the source, re-run with -xs - it must NOT skip this time, and
       must re-copy the (now-different) destination content back to match source.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy - skip existing up-to-date destination (-xs)" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''
$src  = "{0}:\xs_src.bin" -f $drv
$dest = "{0}:\xs_dest.bin" -f $drv
Remove-DriveFile -Path $src
Remove-DriveFile -Path $dest

$rng = [System.Random]::new(901)
$buf = [byte[]]::new(65536)
$rng.NextBytes($buf)
[System.IO.File]::WriteAllBytes($src, $buf)
$hash = (Get-FileHash $src -Algorithm SHA256).Hash
Assert-FileExists -Path $src -Description "Source file created"

# ===============================================================================
# Section 1: destination up-to-date -> should skip
# ===============================================================================
Write-Section "Section 1: destination up-to-date (should skip)"

$out = Invoke-Retool -Args @('copy', $src, $dest)
& $env:RETOOL_EXE copy $src $dest | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Initial copy exits 0"
Assert-HashMatch -Path $dest -ExpectedHash $hash -Description "Initial copy data correct"

$sameTime = (Get-Item $dest).LastWriteTime -eq (Get-Item $src).LastWriteTime
if ($sameTime) {
    Write-Host "    [PASS] Destination timestamp matches source after default copy (T component active)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Destination timestamp does not match source - -xs precondition not met" -ForegroundColor Red
    $script:TestsFailed++
}

$out = Invoke-Retool -Args @('copy', $src, $dest, '-xs')
& $env:RETOOL_EXE copy $src $dest -xs | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "-xs run (up-to-date dest) exits 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Skipping' -Description "Output reports the file was skipped"
Assert-HashMatch -Path $dest -ExpectedHash $hash -Description "Destination still correct after skip"

# ===============================================================================
# Section 2: destination stale (different timestamp) -> should NOT skip
# ===============================================================================
Write-Section "Section 2: destination stale (should re-copy, not skip)"

# Make the destination visibly stale: different content AND a different timestamp.
[System.IO.File]::WriteAllBytes($dest, [byte[]]::new(65536))   # all-zero, wrong content
(Get-Item $dest).LastWriteTime = (Get-Item $dest).LastWriteTime.AddDays(-1)

$out = Invoke-Retool -Args @('copy', $src, $dest, '-xs')
& $env:RETOOL_EXE copy $src $dest -xs | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "-xs run (stale dest) exits 0"
$outStr = $out -join "`n"
if ($outStr -notmatch 'Skipping') {
    Write-Host "    [PASS] Stale destination was NOT skipped" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Stale destination was incorrectly skipped" -ForegroundColor Red
    $script:TestsFailed++
}
Assert-HashMatch -Path $dest -ExpectedHash $hash -Description "Destination re-copied correctly (matches source again)"

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $src
Remove-DriveFile -Path $dest

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
