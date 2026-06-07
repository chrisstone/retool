#Requires -RunAsAdministrator
<#
.SYNOPSIS
    23_inspect_multi_frag.ps1 — Test: retool inspect <file1> <file2> -r

.DESCRIPTION
    Same as 22 but adds -r to include per-file fragmentation reports.
    Verifies that the Fragmentation Report section appears for both files.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: inspect — multi-file with fragmentation report (-r)" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$drv = $env:RETOOL_DRIVE_A -replace ':',''
$file1 = "${drv}:\multifrag_a.bin"
$file2 = "${drv}:\multifrag_b.bin"
Copy-Item $env:RETOOL_TEST_FILE $file1 -Force
Copy-Item $env:RETOOL_TEST_FILE $file2 -Force
Assert-FileExists -Path $file1 -Description "file1 copied to Drive A"
Assert-FileExists -Path $file2 -Description "file2 copied to Drive A"

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section "Run: retool inspect <file1> <file2> -r"
$out = Invoke-Retool -Args @('inspect', $file1, $file2, '-r')
& $env:RETOOL_EXE inspect $file1 $file2 -r | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Files Analyzed'        -Description "Sharing summary present"
Assert-OutputContains -Output $outStr -Substring 'Fragmentation Report'  -Description "Fragmentation Report section present"
Assert-OutputContains -Output $outStr -Substring 'Frag Score'            -Description "Frag Score field present"

# Integrity
Assert-HashMatch -Path $file1 -ExpectedHash $env:RETOOL_TEST_HASH -Description "file1 data unchanged"
Assert-HashMatch -Path $file2 -ExpectedHash $env:RETOOL_TEST_HASH -Description "file2 data unchanged"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $file1
Remove-DriveFile -Path $file2

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
