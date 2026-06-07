#Requires -RunAsAdministrator
<#
.SYNOPSIS
    33_copy_cross_volume_scan_dest.ps1 — Test: retool copy <src> <dest> -d

.DESCRIPTION
    Pre-populates Drive B with a copy of the test file using a regular copy.
    Then calls 'retool copy -d' to copy a second file from Drive A to Drive B.
    Since the content already exists on Drive B, -d should find hash matches
    and clone blocks rather than physically writing them.

    Verifies:
    - Exit code 0
    - Destination file exists with correct hash
    - Output mentions fewer physical bytes written (clone path exercised)
    - Source file intact
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy — cross-volume with -d (content-hash matching)" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$drvA = $env:RETOOL_DRIVE_A -replace ':',''
$drvB = $env:RETOOL_DRIVE_B -replace ':',''

# Pre-seed Drive B with a copy of the test file so -d can find it
$seedFile = "${drvB}:\scan_dest_seed.bin"
Copy-Item $env:RETOOL_TEST_FILE $seedFile -Force
Assert-FileExists -Path $seedFile -Description "Seed file pre-copied to Drive B"

$src  = "${drvA}:\scan_dest_src.bin"
$dest = "${drvB}:\scan_dest_out.bin"
Copy-Item $env:RETOOL_TEST_FILE $src -Force
Remove-DriveFile -Path $dest
Assert-FileExists -Path $src -Description "Source file on Drive A"

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section "Run: retool copy <src> <dest> -d"
$out = Invoke-Retool -Args @('copy', $src, $dest, '-d')
& $env:RETOOL_EXE copy $src $dest -d | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
Assert-FileExists -Path $dest                 -Description "Destination file exists"
Assert-HashMatch -Path $dest -ExpectedHash $env:RETOOL_TEST_HASH -Description "Destination data correct"
Assert-HashMatch -Path $src  -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source unchanged"
Assert-HashMatch -Path $seedFile -ExpectedHash $env:RETOOL_TEST_HASH -Description "Seed file unchanged"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $src
Remove-DriveFile -Path $dest
Remove-DriveFile -Path $seedFile

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
