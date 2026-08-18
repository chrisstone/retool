#Requires -RunAsAdministrator
<#
.SYNOPSIS
    99_teardown.ps1 - Dismount and delete all retool test VHDXs.

.DESCRIPTION
    Cleans up the test environment created by 00_setup.ps1:
    - Dismounts all three retool VHDXs (if attached)
    - Deletes all three VHDX files from C:\Temp
    - Deletes the test file and environment file
    - Deletes stray files/dirs that individual test scripts write directly to
      C:\Temp instead of onto a VHDX-mounted drive (VHDX-hosted artifacts are
      cleaned automatically when the VHDX itself is deleted, above): the file
      list and JSON output from 25/26_inspect_*.ps1, and the resident-file
      fixtures from 37_copy_small_files.ps1
    - Does NOT delete C:\Temp itself (it may have been pre-existing)

    KNOWN LIMITATION: if a VHDX's backing file was deleted (e.g. by hand)
    while it was still attached, the resulting orphaned mount isn't detected
    or dismounted here (Remove-Vhdx only acts on VHDX paths that still exist
    on disk). 00_setup.ps1's leftover check will flag such an orphan volume,
    but clearing it requires manually detaching it (e.g. via Disk Management
    or `mountvol /d`) before re-running setup.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'  # Don't abort on individual cleanup errors

$scriptDir = $PSScriptRoot
$envFile   = Join-Path $scriptDir '.retool_env.ps1'

Write-Host "==> Loading environment..." -ForegroundColor Cyan
if (Test-Path $envFile) {
    . $envFile
} else {
    Write-Host "    Environment file not found - using defaults" -ForegroundColor Yellow
    $env:RETOOL_VHDX_A    = 'C:\Temp\retool-a.vhdx'
    $env:RETOOL_VHDX_B    = 'C:\Temp\retool-b.vhdx'
    $env:RETOOL_VHDX_C    = 'C:\Temp\retool-c.vhdx'
    $env:RETOOL_TEST_FILE = 'C:\Temp\testfile.bin'
}

function Remove-Vhdx {
    param([string] $Path)
    if (-not (Test-Path $Path)) {
        Write-Host ("    Skipped (not found): {0}" -f $Path) -ForegroundColor DarkGray
        return
    }
    try {
        Write-Host ("    Dismounting: {0}" -f $Path) -ForegroundColor Cyan
        $tempScript = [System.IO.Path]::GetTempFileName()
        $commands = @(
            ('select vdisk file="{0}"' -f $Path),
            "detach vdisk"
        )
        $commands | Set-Content $tempScript -Encoding ASCII
        diskpart /s $tempScript | Out-Null
        Remove-Item $tempScript -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    } catch {
        Write-Host ("    Warning: could not dismount {0}" -f $Path) -ForegroundColor Yellow
    }
    Write-Host ("    Deleting: {0}" -f $Path) -ForegroundColor Cyan
    Remove-Item $Path -Force -ErrorAction SilentlyContinue
    if (Test-Path $Path) {
        Write-Host ("    WARNING: Could not delete {0}" -f $Path) -ForegroundColor Yellow
    } else {
        Write-Host ("    Deleted: {0}" -f $Path) -ForegroundColor Green
    }
}

Write-Host "==> Dismounting and deleting VHDXs..." -ForegroundColor Cyan
Remove-Vhdx -Path $env:RETOOL_VHDX_A
Remove-Vhdx -Path $env:RETOOL_VHDX_B
Remove-Vhdx -Path $env:RETOOL_VHDX_C

Write-Host "==> Removing test file..." -ForegroundColor Cyan
if (Test-Path $env:RETOOL_TEST_FILE) {
    Remove-Item $env:RETOOL_TEST_FILE -Force -ErrorAction SilentlyContinue
    Write-Host ("    Deleted: {0}" -f $env:RETOOL_TEST_FILE) -ForegroundColor Green
} else {
    Write-Host ("    Not found (already gone): {0}" -f $env:RETOOL_TEST_FILE) -ForegroundColor DarkGray
}

Write-Host "==> Removing environment file..." -ForegroundColor Cyan
if (Test-Path $envFile) {
    Remove-Item $envFile -Force -ErrorAction SilentlyContinue
    Write-Host ("    Deleted: {0}" -f $envFile) -ForegroundColor Green
}

# Also clean up any stray files left directly on C:\Temp by tests that don't
# write onto a VHDX-mounted drive - 25/26_inspect_*.ps1's file list and JSON
# output, and 37_copy_small_files.ps1's resident-file fixtures (only linger
# if that script was interrupted before its own end-of-section cleanup ran).
$strayFiles = @(
    'C:\Temp\retool_filelist.txt',
    'C:\Temp\retool_inspect_out.json',
    'C:\Temp\small_same_src.txt',
    'C:\Temp\small_same_dest.txt',
    'C:\Temp\small_xvol_src.txt'
)
foreach ($f in $strayFiles) {
    if (Test-Path $f) { Remove-Item $f -Force -ErrorAction SilentlyContinue }
}

$strayDirs = @(
    'C:\Temp\small_same_dir_src',
    'C:\Temp\small_same_dir_dst'
)
foreach ($d in $strayDirs) {
    if (Test-Path $d) { Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
Write-Host "==> Teardown complete." -ForegroundColor Green
