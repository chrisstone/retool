#Requires -RunAsAdministrator
<#
.SYNOPSIS
    20_inspect_single.ps1 — Test: retool inspect <single-file>

.DESCRIPTION
    Copies the test file to Drive A, runs 'retool inspect <file>', and verifies:
    - Exit code 0
    - Output contains extent table headers (VCN, LCN, Clusters)
    - Output contains the correct file size
    - Cluster size reported matches the volume (65536)
    - Source file is not modified (hash unchanged)
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: inspect — single file" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$dest = Copy-TestFileToDrive -DriveLetter ($env:RETOOL_DRIVE_A -replace ':','') -DestName 'inspect_test.bin'
Assert-FileExists -Path $dest -Description "Test file copied to Drive A"

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section "Run: retool inspect <file>"
$out = Invoke-Retool -Args @('inspect', $dest, '-e')
& $env:RETOOL_EXE inspect $dest -e | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'VCN'          -Description "Output contains VCN column"
Assert-OutputContains -Output $outStr -Substring 'LCN'          -Description "Output contains LCN column"
Assert-OutputContains -Output $outStr -Substring 'Clusters'     -Description "Output contains Clusters column"
Assert-OutputContains -Output $outStr -Substring '65536'        -Description "Output contains cluster size 65536"
Assert-OutputContains -Output $outStr -Substring '104857600'    -Description "Output contains file size (100 MB = 104857600 bytes)"

# Source file integrity
Assert-HashMatch -Path $env:RETOOL_TEST_FILE -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source test file unchanged"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $dest

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
