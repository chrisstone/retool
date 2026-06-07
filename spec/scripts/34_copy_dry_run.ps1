#Requires -RunAsAdministrator
<#
.SYNOPSIS
    34_copy_dry_run.ps1 — Test: retool copy <src> <dest> --dry-run

.DESCRIPTION
    Runs 'retool copy --dry-run' and verifies:
    - Exit code 0
    - Destination file does NOT exist (nothing was written)
    - Output contains dry-run indication
    - Source file is unmodified
    Tests both same-volume and cross-volume --dry-run.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy — dry run (--dry-run)" -ForegroundColor White

$drvA = $env:RETOOL_DRIVE_A -replace ':',''
$drvB = $env:RETOOL_DRIVE_B -replace ':',''

# ── Same-volume dry run ───────────────────────────────────────────────────────
Write-Section "Same-volume --dry-run"
$src  = "${drvA}:\dryrun_src.bin"
$dest = "${drvA}:\dryrun_dest.bin"
Copy-Item $env:RETOOL_TEST_FILE $src -Force
Remove-DriveFile -Path $dest

$out = Invoke-Retool -Args @('copy', $src, $dest, '--dry-run')
& $env:RETOOL_EXE copy $src $dest --dry-run | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Same-volume dry-run exits 0"
if (-not (Test-Path $dest)) {
    Write-Host "    [PASS] Destination not created (dry-run)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Destination file was created (should not be with --dry-run)" -ForegroundColor Red
    $script:TestsFailed++
    Remove-DriveFile -Path $dest
}
Assert-HashMatch -Path $src -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source unchanged after dry-run"
Assert-OutputContains -Output ($out -join "`n") -Substring 'dry' -Description "Output mentions dry-run"

# ── Cross-volume dry run ──────────────────────────────────────────────────────
Write-Section "Cross-volume --dry-run"
$destB = "${drvB}:\dryrun_dest_b.bin"
Remove-DriveFile -Path $destB

$out = Invoke-Retool -Args @('copy', $src, $destB, '--dry-run')
& $env:RETOOL_EXE copy $src $destB --dry-run | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Cross-volume dry-run exits 0"
if (-not (Test-Path $destB)) {
    Write-Host "    [PASS] Cross-volume destination not created (dry-run)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Cross-volume destination was created (should not be)" -ForegroundColor Red
    $script:TestsFailed++
    Remove-DriveFile -Path $destB
}

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $src

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
