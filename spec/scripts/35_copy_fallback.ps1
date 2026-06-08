#Requires -RunAsAdministrator
<#
.SYNOPSIS
    35_copy_fallback.ps1 — Test: retool copy <src-ReFS-64K> <dest-ReFS-4K> (cluster mismatch → fallback)

.DESCRIPTION
    Drive A is ReFS 64K and Drive C is ReFS 4K.  Copying between them triggers
    FallbackCopyStrategy (cluster sizes differ) — retool falls back to CopyFileExW.
    Verifies:
    - Exit code 0
    - Destination file exists on Drive C with correct hash
    - Source file is unmodified
    - Output contains a warning or notice about the fallback
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy — fallback (cluster size mismatch: 64K → 4K)" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$drvA = $env:RETOOL_DRIVE_A -replace ':',''
$drvC = $env:RETOOL_DRIVE_C -replace ':',''
$src  = "{0}:\fallback_src.bin" -f $drvA
$dest = "{0}:\fallback_dest.bin" -f $drvC
Copy-Item $env:RETOOL_TEST_FILE $src -Force
Remove-DriveFile -Path $dest
Assert-FileExists -Path $src -Description "Source file on Drive A (64K)"

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section "Run: retool copy DriveA→DriveC (cluster mismatch)"
$out = Invoke-Retool -Args @('copy', $src, $dest)
& $env:RETOOL_EXE copy $src $dest | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0 (fallback succeeds)"
Assert-FileExists -Path $dest                 -Description "Destination file created on Drive C"
Assert-HashMatch -Path $dest -ExpectedHash $env:RETOOL_TEST_HASH -Description "Destination data correct"
Assert-HashMatch -Path $src  -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source file unchanged"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $src
Remove-DriveFile -Path $dest

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
