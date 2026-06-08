#Requires -RunAsAdministrator
<#
.SYNOPSIS
    99_teardown.ps1 — Dismount and delete all retool test VHDXs.

.DESCRIPTION
    Cleans up the test environment created by 00_setup.ps1:
    - Dismounts all three retool VHDXs (if attached)
    - Deletes all three VHDX files from C:\Temp
    - Deletes the test file and environment file
    - Does NOT delete C:\Temp itself (it may have been pre-existing)
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'  # Don't abort on individual cleanup errors

$scriptDir = $PSScriptRoot
$envFile   = Join-Path $scriptDir '.retool_env.ps1'

Write-Host "==> Loading environment..." -ForegroundColor Cyan
if (Test-Path $envFile) {
    . $envFile
} else {
    Write-Host "    Environment file not found — using defaults" -ForegroundColor Yellow
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

# Also clean up any stray JSON or filelist files left by tests
$tempFiles = @(
    'C:\Temp\retool_filelist.txt',
    'C:\Temp\retool_inspect_out.json'
)
foreach ($f in $tempFiles) {
    if (Test-Path $f) { Remove-Item $f -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
Write-Host "==> Teardown complete." -ForegroundColor Green
