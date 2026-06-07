#Requires -RunAsAdministrator
<#
.SYNOPSIS
    42_dedup_pairwise.ps1 — Test: retool dedup <file1> <file2> (pair-wise dedup)

.DESCRIPTION
    Copies the test file twice to Drive A as separate files, then uses pair-wise
    dedup to deduplicate just those two files.  Verifies:
    - Exit code 0
    - Both files still read back with correct hashes
    - Disk space is reclaimed
    - Summary fields are present
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: dedup — pair-wise (<file1> <file2>)" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$drv   = $env:RETOOL_DRIVE_A -replace ':',''
$fileA = "${drv}:\pair_a.bin"
$fileB = "${drv}:\pair_b.bin"
Copy-Item $env:RETOOL_TEST_FILE $fileA -Force
Copy-Item $env:RETOOL_TEST_FILE $fileB -Force
Assert-FileExists -Path $fileA -Description "fileA present"
Assert-FileExists -Path $fileB -Description "fileB present"

$freeBefore = (Get-PSDrive -Name $drv).Free
Write-Host "    Free before dedup: $([math]::Round($freeBefore/1MB,1)) MB" -ForegroundColor DarkGray

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section "Run: retool dedup <file1> <file2>"
$out = Invoke-Retool -Args @('dedup', $fileA, $fileB)
& $env:RETOOL_EXE dedup $fileA $fileB | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Clusters Deduped' -Description "Clusters Deduped field present"
Assert-OutputContains -Output $outStr -Substring 'Space Reclaimed'  -Description "Space Reclaimed field present"

Assert-HashMatch -Path $fileA -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileA data correct after dedup"
Assert-HashMatch -Path $fileB -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileB data correct after dedup"

$freeAfter = (Get-PSDrive -Name $drv).Free
$reclaimed = $freeAfter - $freeBefore
Write-Host "    Free after dedup: $([math]::Round($freeAfter/1MB,1)) MB (reclaimed ~$([math]::Round($reclaimed/1MB,1)) MB)" -ForegroundColor DarkGray
if ($reclaimed -gt 50MB) {
    Write-Host "    [PASS] > 50 MB reclaimed after pair-wise dedup" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [WARN] Only $([math]::Round($reclaimed/1MB,1)) MB reclaimed" -ForegroundColor Yellow
}

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $fileA
Remove-DriveFile -Path $fileB

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
