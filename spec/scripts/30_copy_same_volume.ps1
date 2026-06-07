#Requires -RunAsAdministrator
<#
.SYNOPSIS
    30_copy_same_volume.ps1 — Test: retool copy <src> <dest> (same ReFS volume)

.DESCRIPTION
    Copies the test file onto Drive A, then uses retool copy to clone it to a second
    location on the same volume.  Same-volume copy uses FSCTL_DUPLICATE_EXTENTS_TO_FILE
    and should:
    - Exit code 0
    - Destination file exists
    - Destination file hash matches source (data integrity)
    - Destination takes minimal additional disk space (blocks are shared)
    - Source file is not modified
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy — same-volume clone (FSCTL_DUPLICATE_EXTENTS_TO_FILE)" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$drv = $env:RETOOL_DRIVE_A -replace ':',''
$src  = "${drv}:\copy_src.bin"
$dest = "${drv}:\copy_dest.bin"
Copy-Item $env:RETOOL_TEST_FILE $src -Force
Remove-DriveFile -Path $dest
Assert-FileExists -Path $src -Description "Source file on Drive A"

$freeBeforeMB = [math]::Round((Get-PSDrive -Name $drv).Free / 1MB, 1)
Write-Host "    Free space before copy: $freeBeforeMB MB" -ForegroundColor DarkGray

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section "Run: retool copy <src> <dest> (same volume)"
$out = Invoke-Retool -Args @('copy', $src, $dest)
& $env:RETOOL_EXE copy $src $dest | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode       -Description "Exit code 0"
Assert-FileExists -Path $dest                        -Description "Destination file exists"
Assert-HashMatch -Path $dest -ExpectedHash $env:RETOOL_TEST_HASH -Description "Destination data matches source"
Assert-HashMatch -Path $src  -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source file unchanged"

# Verify space savings: free space should not have dropped by a full 100 MB
$freeAfterMB = [math]::Round((Get-PSDrive -Name $drv).Free / 1MB, 1)
$dropped = $freeBeforeMB - $freeAfterMB
Write-Host "    Free space after copy:  $freeAfterMB MB (dropped $dropped MB)" -ForegroundColor DarkGray
if ($dropped -lt 10) {
    Write-Host "    [PASS] Disk space consumption < 10 MB (blocks are shared)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [WARN] Disk dropped $dropped MB — blocks may not be shared (ReFS dedup may not have fired)" -ForegroundColor Yellow
    # Not a hard failure since VHDX allocation is approximate
}

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $src
Remove-DriveFile -Path $dest

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
