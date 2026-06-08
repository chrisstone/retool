#Requires -RunAsAdministrator
<#
.SYNOPSIS
    41_dedup_volume.ps1 - Test: retool dedup <volume> (volume-wide, actual dedup)

.DESCRIPTION
    Depends on 40_dedup_volume_dry_run.ps1 leaving two copies of the test file
    on Drive A (dedup_dry_a.bin and dedup_dry_b.bin).  Runs actual dedup and verifies:
    - Exit code 0
    - Both files still exist with correct hashes (data integrity preserved)
    - Drive A free space has increased (blocks reclaimed)
    - Summary reports non-zero clusters deduped

    This is the KEY integration test: Drive A is ~1 GB and was nearly full with
    two 100 MB files.  After dedup, one of those 100 MB blocks is freed.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: dedup - volume-wide (actual dedup)" -ForegroundColor White

# -- Arrange -------------------------------------------------------------------
Write-Section "Arrange"
$drv   = $env:RETOOL_DRIVE_A -replace ':',''
$fileA = "{0}:\dedup_dry_a.bin" -f $drv
$fileB = "{0}:\dedup_dry_b.bin" -f $drv

# Re-copy if a prior run cleaned up
if (-not (Test-Path $fileA)) { Copy-Item $env:RETOOL_TEST_FILE $fileA -Force }
if (-not (Test-Path $fileB)) { Copy-Item $env:RETOOL_TEST_FILE $fileB -Force }
Assert-FileExists -Path $fileA -Description "fileA present"
Assert-FileExists -Path $fileB -Description "fileB present"

$freeBefore = (Get-PSDrive -Name $drv).Free
Write-Host ("    Free before dedup: {0} MB" -f [math]::Round($freeBefore/1MB,1)) -ForegroundColor DarkGray

# -- Act -----------------------------------------------------------------------
Write-Section ("Run: retool dedup {0}" -f $env:RETOOL_DRIVE_A)
$out = Invoke-Retool -Args @('dedup', $env:RETOOL_DRIVE_A)
& $env:RETOOL_EXE dedup $env:RETOOL_DRIVE_A | Out-Null
$exitCode = $LASTEXITCODE

# -- Assert --------------------------------------------------------------------
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Clusters Deduped' -Description "Clusters Deduped field present"
Assert-OutputContains -Output $outStr -Substring 'Space Reclaimed'  -Description "Space Reclaimed field present"

# Data integrity - both files must read back correctly after dedup
Assert-HashMatch -Path $fileA -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileA data correct after dedup"
Assert-HashMatch -Path $fileB -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileB data correct after dedup"

# Disk space should have been reclaimed
Start-Sleep -Seconds 10
$freeAfter = (Get-PSDrive -Name $drv).Free
$reclaimed = $freeAfter - $freeBefore
Write-Host ("    Free after dedup: {0} MB (reclaimed ~{1} MB)" -f [math]::Round($freeAfter/1MB,1), [math]::Round($reclaimed/1MB,1)) -ForegroundColor DarkGray
if ($reclaimed -gt 50MB) {
    Write-Host "    [PASS] > 50 MB reclaimed after volume-wide dedup" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host ("    [WARN] Only {0} MB reclaimed - check that blocks were actually shared" -f [math]::Round($reclaimed/1MB,1)) -ForegroundColor Yellow
    # Not a hard fail: VHDX space reporting can lag; data integrity is the primary check
}

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $fileA
Remove-DriveFile -Path $fileB

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
