#Requires -RunAsAdministrator
<#
.SYNOPSIS
    24_inspect_volume.ps1 — Test: retool inspect <volume-root>

.DESCRIPTION
    Copies the test file to Drive A, then runs 'retool inspect E:\' (volume scan mode).
    Verifies:
    - Exit code 0
    - Volume scan report section present
    - Files Scanned, Clusters Indexed fields present
    - Shared Blocks and Dedup Savings fields present
    - Source file on the volume is not corrupted
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: inspect — volume scan mode" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$drv = $env:RETOOL_DRIVE_A -replace ':',''
$file = "{0}:\vol_scan_test.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $file -Force
Assert-FileExists -Path $file -Description "Test file copied to Drive A"

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section ("Run: retool inspect {0}" -f $env:RETOOL_DRIVE_A)
$out = Invoke-Retool -Args @('inspect', $env:RETOOL_DRIVE_A)
& $env:RETOOL_EXE inspect $env:RETOOL_DRIVE_A | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Files Scanned'    -Description "Files Scanned field present"
Assert-OutputContains -Output $outStr -Substring 'Clusters Indexed' -Description "Clusters Indexed field present"
Assert-OutputContains -Output $outStr -Substring 'Shared Blocks'    -Description "Shared Blocks field present"
Assert-OutputContains -Output $outStr -Substring 'Dedup Savings'    -Description "Dedup Savings field present"

# File on volume should be intact
Assert-HashMatch -Path $file -ExpectedHash $env:RETOOL_TEST_HASH -Description "File on volume intact after scan"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $file

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
