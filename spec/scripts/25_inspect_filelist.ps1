#Requires -RunAsAdministrator
<#
.SYNOPSIS
    25_inspect_filelist.ps1 — Test: retool inspect -i <filelist.txt>

.DESCRIPTION
    Writes a newline-delimited file list to C:\Temp, copies two test files to Drive A,
    then runs 'retool inspect -i <filelist.txt>'.
    Verifies multi-file output is produced from a file list input.
    Also verifies:
    - Lines starting with '#' and blank lines are skipped
    - UTF-8 BOM is handled (file list written with BOM)
    - Both files' data is intact after inspection
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: inspect — file list input (-i)" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$drv = $env:RETOOL_DRIVE_A -replace ':',''
$file1 = "${drv}:\filelist_a.bin"
$file2 = "${drv}:\filelist_b.bin"
Copy-Item $env:RETOOL_TEST_FILE $file1 -Force
Copy-Item $env:RETOOL_TEST_FILE $file2 -Force
Assert-FileExists -Path $file1 -Description "file1 copied to Drive A"
Assert-FileExists -Path $file2 -Description "file2 copied to Drive A"

# Write file list with UTF-8 BOM, a comment line, and a blank line
$listPath = 'C:\Temp\retool_filelist.txt'
$bom = [System.Text.Encoding]::UTF8.GetPreamble()
$listContent = "# This is a comment line`r`n`r`n$file1`r`n$file2`r`n"
$bytes = $bom + [System.Text.Encoding]::UTF8.GetBytes($listContent)
[System.IO.File]::WriteAllBytes($listPath, $bytes)
Assert-FileExists -Path $listPath -Description "File list created at C:\Temp\retool_filelist.txt"

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section "Run: retool inspect -i <filelist>"
$out = Invoke-Retool -Args @('inspect', '-i', $listPath)
& $env:RETOOL_EXE inspect -i $listPath | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Files Analyzed' -Description "Multi-file sharing summary present"
Assert-OutputContains -Output $outStr -Substring 'F0'             -Description "Sharing matrix present"

# File integrity
Assert-HashMatch -Path $file1 -ExpectedHash $env:RETOOL_TEST_HASH -Description "file1 intact"
Assert-HashMatch -Path $file2 -ExpectedHash $env:RETOOL_TEST_HASH -Description "file2 intact"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $file1
Remove-DriveFile -Path $file2
Remove-Item $listPath -Force -ErrorAction SilentlyContinue

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
