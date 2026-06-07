#Requires -RunAsAdministrator
<#
.SYNOPSIS
    22_inspect_multi.ps1 — Test: retool inspect <file1> <file2> (multi-file sharing)

.DESCRIPTION
    Copies the same test file twice to Drive A (so they are identical and will share
    no blocks until dedup runs), then runs 'retool inspect <f1> <f2>'.
    Before dedup the files have different LCNs so shared blocks = 0; this tests that
    the multi-file mode runs correctly and outputs the sharing matrix and summary.

    Verifies:
    - Exit code 0
    - Output contains sharing summary fields (Files Analyzed, Shared Blocks, Dedup Savings)
    - Output contains a cross-file sharing matrix table
    - Neither source file is modified
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: inspect — multi-file sharing analysis" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$drv = $env:RETOOL_DRIVE_A -replace ':',''
$file1 = "${drv}:\multi_a.bin"
$file2 = "${drv}:\multi_b.bin"
Copy-Item $env:RETOOL_TEST_FILE $file1 -Force
Copy-Item $env:RETOOL_TEST_FILE $file2 -Force
Assert-FileExists -Path $file1 -Description "file1 copied to Drive A"
Assert-FileExists -Path $file2 -Description "file2 copied to Drive A"

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section "Run: retool inspect <file1> <file2>"
$out = Invoke-Retool -Args @('inspect', $file1, $file2)
& $env:RETOOL_EXE inspect $file1 $file2 | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Files Analyzed' -Description "Files Analyzed field present"
Assert-OutputContains -Output $outStr -Substring 'Shared Blocks'  -Description "Shared Blocks field present"
Assert-OutputContains -Output $outStr -Substring 'Saved Space'    -Description "Saved Space field present"
Assert-OutputContains -Output $outStr -Substring 'Dedup Savings'  -Description "Dedup Savings field present"
Assert-OutputContains -Output $outStr -Substring 'F0'             -Description "Sharing matrix F0 column present"
Assert-OutputContains -Output $outStr -Substring 'F1'             -Description "Sharing matrix F1 column present"

# File integrity
Assert-HashMatch -Path $file1 -ExpectedHash $env:RETOOL_TEST_HASH -Description "file1 data unchanged"
Assert-HashMatch -Path $file2 -ExpectedHash $env:RETOOL_TEST_HASH -Description "file2 data unchanged"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $file1
Remove-DriveFile -Path $file2

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
