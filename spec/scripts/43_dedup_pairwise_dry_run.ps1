#Requires -RunAsAdministrator
<#
.SYNOPSIS
    43_dedup_pairwise_dry_run.ps1 - Test: retool dedup <file1> <file2> -n

.DESCRIPTION
    Pair-wise dedup dry run - verifies no data is written and summary is reported.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: dedup - pair-wise dry run (--dry-run)" -ForegroundColor White

# -- Arrange -------------------------------------------------------------------
Write-Section "Arrange"
$drv   = $env:RETOOL_DRIVE_A -replace ':',''
$fileA = "{0}:\pdry_a.bin" -f $drv
$fileB = "{0}:\pdry_b.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $fileA -Force
Copy-Item $env:RETOOL_TEST_FILE $fileB -Force
Assert-FileExists -Path $fileA -Description "fileA copied"
Assert-FileExists -Path $fileB -Description "fileB copied"

$freeBefore = (Get-PSDrive -Name $drv).Free

# -- Act -----------------------------------------------------------------------
Write-Section "Run: retool dedup <file1> <file2> -n"
$out = Invoke-Retool -Args @('dedup', $fileA, $fileB, '-n')
& $env:RETOOL_EXE dedup $fileA $fileB -n | Out-Null
$exitCode = $LASTEXITCODE

# -- Assert --------------------------------------------------------------------
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Clusters Deduped' -Description "Clusters Deduped field present"
Assert-OutputContains -Output $outStr -Substring 'Space Reclaimed'  -Description "Space Reclaimed field present"
Assert-OutputContains -Output $outStr -Substring 'dry'              -Description "dry-run label in output"

# Data must be unchanged
Assert-HashMatch -Path $fileA -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileA unchanged after dry-run"
Assert-HashMatch -Path $fileB -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileB unchanged after dry-run"

# Disk free space must not have changed
$freeAfter = (Get-PSDrive -Name $drv).Free
$delta = [math]::Abs($freeAfter - $freeBefore)
if ($delta -lt 1MB) {
    Write-Host "    [PASS] Free space unchanged during dry-run" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host ("    [FAIL] Free space changed by {0} MB during dry-run" -f [math]::Round($delta/1MB,1)) -ForegroundColor Red
    $script:TestsFailed++
}

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $fileA
Remove-DriveFile -Path $fileB

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
