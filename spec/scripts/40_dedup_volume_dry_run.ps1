#Requires -RunAsAdministrator
<#
.SYNOPSIS
    40_dedup_volume_dry_run.ps1 - Test: retool dedup <volume> -n

.DESCRIPTION
    Copies the test file twice to Drive A (creating two physically distinct files
    with identical content - they will have different LCNs until dedup runs).
    Then runs 'retool dedup <volume> -n'.

    KEY SCENARIO: Because Drive A is only 1 GB and ReFS overhead is significant,
    copying the file twice nearly fills the disk.  The dry-run reports potential
    savings; the actual dedup (script 41) then frees space.

    Verifies:
    - Exit code 0
    - No data is written (file hashes unchanged)
    - Output contains dedup summary with Space Reclaimed and candidate count
    - Drive A free space has not changed
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: dedup - volume-wide dry run (--dry-run)" -ForegroundColor White

# -- Arrange -------------------------------------------------------------------
Write-Section "Arrange - copy test file twice to Drive A"
$drv   = $env:RETOOL_DRIVE_A -replace ':',''
$fileA = "{0}:\dedup_dry_a.bin" -f $drv
$fileB = "{0}:\dedup_dry_b.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $fileA -Force
Copy-Item $env:RETOOL_TEST_FILE $fileB -Force
Assert-FileExists -Path $fileA -Description "fileA copied"
Assert-FileExists -Path $fileB -Description "fileB copied"

$freeBefore = (Get-PSDrive -Name $drv).Free
Write-Host ("    Free before dry-run: {0} MB" -f [math]::Round($freeBefore/1MB,1)) -ForegroundColor DarkGray

# -- Act -----------------------------------------------------------------------
Write-Section ("Run: retool dedup {0} -n" -f $env:RETOOL_DRIVE_A)
$out = Invoke-Retool -Args @('dedup', $env:RETOOL_DRIVE_A, '-n')
& $env:RETOOL_EXE dedup $env:RETOOL_DRIVE_A -n | Out-Null
$exitCode = $LASTEXITCODE

# -- Assert --------------------------------------------------------------------
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Clusters Deduped' -Description "Clusters Deduped field present"
Assert-OutputContains -Output $outStr -Substring 'Space Reclaimed'  -Description "Space Reclaimed field present"
Assert-OutputContains -Output $outStr -Substring 'dry'              -Description "Output mentions dry-run"

# Data must be unchanged
Assert-HashMatch -Path $fileA -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileA data unchanged after dry-run"
Assert-HashMatch -Path $fileB -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileB data unchanged after dry-run"

# Disk free space must be unchanged
$freeAfter = (Get-PSDrive -Name $drv).Free
$delta = [math]::Abs($freeAfter - $freeBefore)
if ($delta -lt 1MB) {
    Write-Host "    [PASS] Free space unchanged (delta < 1 MB)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host ("    [FAIL] Free space changed by {0} MB during dry-run" -f [math]::Round($delta/1MB,1)) -ForegroundColor Red
    $script:TestsFailed++
}

# NOTE: Do NOT clean up - script 41 needs these files to test actual dedup.
Write-Host "    (files left for 41_dedup_volume.ps1)" -ForegroundColor DarkGray

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
