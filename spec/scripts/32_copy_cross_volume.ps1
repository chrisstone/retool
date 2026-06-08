#Requires -RunAsAdministrator
<#
.SYNOPSIS
    32_copy_cross_volume.ps1 — Test: retool copy <src> <dest> (cross-volume ReFS→ReFS, 64K→64K)

.DESCRIPTION
    Copies the test file from Drive A to Drive B (both ReFS, both 64K cluster).
    This exercises CrossVolumeRefsCopyStrategy (LCN-mapped dedup-preserving copy).
    Verifies:
    - Exit code 0
    - Destination file exists on Drive B
    - Destination file hash matches source
    - Source file is unmodified
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy — cross-volume ReFS→ReFS (64K cluster)" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$drvA = $env:RETOOL_DRIVE_A -replace ':',''
$drvB = $env:RETOOL_DRIVE_B -replace ':',''
$src  = "{0}:\xvol_src.bin" -f $drvA
$dest = "{0}:\xvol_dest.bin" -f $drvB
Copy-Item $env:RETOOL_TEST_FILE $src -Force
Remove-DriveFile -Path $dest
Assert-FileExists -Path $src -Description "Source file on Drive A"

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section "Run: retool copy <DriveA file> <DriveB file>"
$out = Invoke-Retool -Args @('copy', $src, $dest)
& $env:RETOOL_EXE copy $src $dest | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
Assert-FileExists -Path $dest                 -Description "Destination file exists on Drive B"
Assert-HashMatch -Path $dest -ExpectedHash $env:RETOOL_TEST_HASH -Description "Destination data matches source"
Assert-HashMatch -Path $src  -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source file unchanged"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $src
Remove-DriveFile -Path $dest

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
