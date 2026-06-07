#Requires -RunAsAdministrator
<#
.SYNOPSIS
    00_setup.ps1 — Test environment setup for retool spec scripts.

.DESCRIPTION
    1. Verifies that build/debug/Debug/retool.exe exists (relative to repo root).
    2. Creates C:\Temp if it does not exist.
    3. Creates a 100 MB binary test file at C:\Temp\testfile.bin.
    4. Creates, attaches, initialises, and mounts three 1 GB VHDX files:
         - retool-a.vhdx  (ReFS, 64 K cluster) → expected drive letter stored in $env:RETOOL_DRIVE_A
         - retool-b.vhdx  (ReFS, 64 K cluster) → $env:RETOOL_DRIVE_B
         - retool-c.vhdx  (ReFS,  4 K cluster) → $env:RETOOL_DRIVE_C
    5. Writes a shared environment file (spec\scripts\.retool_env.ps1) that all
       subsequent test scripts dot-source to pick up drive-letter variables.

    RATIONALE FOR 1 GB SIZE:
    ReFS metadata overhead on a 1 GB volume consumes the majority of available
    clusters, leaving just enough room for one 100 MB test file.  All subsequent
    "copies" of that file (without dedup) fill the disk, forcing retool's
    deduplication code paths to be exercised before additional files can coexist.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ── Paths ────────────────────────────────────────────────────────────────────
$scriptDir  = $PSScriptRoot
$repoRoot   = Resolve-Path (Join-Path $scriptDir '..\..')
$retoolExe  = Join-Path $repoRoot 'build\debug\Debug\retool.exe'
$vhdxDir    = 'C:\Temp'
$envFile    = Join-Path $scriptDir '.retool_env.ps1'

# ── 1. Verify retool.exe ─────────────────────────────────────────────────────
Write-Host "==> Verifying retool.exe..." -ForegroundColor Cyan
if (-not (Test-Path $retoolExe)) {
    Write-Error "retool.exe not found at: $retoolExe`nPlease build the project first: cmake --build build/debug --config Debug"
    exit 1
}
Write-Host "    Found: $retoolExe" -ForegroundColor Green

# ── 2. Create C:\Temp ────────────────────────────────────────────────────────
Write-Host "==> Creating C:\Temp..." -ForegroundColor Cyan
if (-not (Test-Path $vhdxDir)) {
    New-Item -ItemType Directory -Path $vhdxDir | Out-Null
    Write-Host "    Created C:\Temp" -ForegroundColor Green
} else {
    Write-Host "    C:\Temp already exists" -ForegroundColor Yellow
}

# ── 3. Create 100 MB test file ───────────────────────────────────────────────
$testFile = Join-Path $vhdxDir 'testfile.bin'
Write-Host "==> Creating 100 MB test file at $testFile..." -ForegroundColor Cyan
if (Test-Path $testFile) {
    Write-Host "    Removing existing test file" -ForegroundColor Yellow
    Remove-Item $testFile -Force
}

# Use a fixed random seed so the file is reproducible for hash verification
$rng = [System.Random]::new(42)
$buf = [byte[]]::new(1MB)
$fs  = [System.IO.File]::OpenWrite($testFile)
try {
    for ($i = 0; $i -lt 100; $i++) {
        $rng.NextBytes($buf)
        $fs.Write($buf, 0, $buf.Length)
    }
} finally {
    $fs.Close()
}
$testHash = (Get-FileHash $testFile -Algorithm SHA256).Hash
Write-Host "    Created $testFile  SHA-256: $testHash" -ForegroundColor Green

# ── Helper: create, attach, format, and mount a VHDX ────────────────────────
function New-RetoolVhdx {
    param(
        [string] $VhdxPath,
        [int]    $SizeMB,
        [int]    $ClusterSizeBytes,
        [string] $VolumeLabel
    )

    if (Test-Path $VhdxPath) {
        Write-Host "    Removing existing VHDX: $VhdxPath" -ForegroundColor Yellow
        # Detach if mounted
        $disk = Get-VHD $VhdxPath -ErrorAction SilentlyContinue
        if ($disk -and $disk.Attached) {
            Dismount-VHD -Path $VhdxPath
        }
        Remove-Item $VhdxPath -Force
    }

    Write-Host "    Creating VHDX: $VhdxPath ($SizeMB MB, cluster=$ClusterSizeBytes bytes)" -ForegroundColor Cyan

    # Create dynamically expanding VHDX
    New-VHD -Path $VhdxPath -SizeBytes ($SizeMB * 1MB) -Dynamic -BlockSizeBytes 2MB | Out-Null

    # Attach
    $vhd = Mount-VHD -Path $VhdxPath -PassThru
    $diskNum = $vhd.DiskNumber

    # Initialise (GPT)
    Initialize-Disk -Number $diskNum -PartitionStyle GPT -Confirm:$false | Out-Null

    # Create a single data partition (leave 1 MB for GPT overhead)
    $partition = New-Partition -DiskNumber $diskNum -UseMaximumSize -AssignDriveLetter
    $driveLetter = $partition.DriveLetter

    # Format with ReFS
    Format-Volume -DriveLetter $driveLetter `
                  -FileSystem ReFS `
                  -AllocationUnitSize $ClusterSizeBytes `
                  -NewFileSystemLabel $VolumeLabel `
                  -Confirm:$false | Out-Null

    Write-Host "    Mounted as ${driveLetter}: (label=$VolumeLabel)" -ForegroundColor Green
    return $driveLetter
}

# ── 4. Create three VHDXs ────────────────────────────────────────────────────
$vhdxA = Join-Path $vhdxDir 'retool-a.vhdx'
$vhdxB = Join-Path $vhdxDir 'retool-b.vhdx'
$vhdxC = Join-Path $vhdxDir 'retool-c.vhdx'

Write-Host "==> Creating VHDX A (ReFS 64K cluster)..." -ForegroundColor Cyan
$driveA = New-RetoolVhdx -VhdxPath $vhdxA -SizeMB 1024 -ClusterSizeBytes 65536 -VolumeLabel 'RetoolA'

Write-Host "==> Creating VHDX B (ReFS 64K cluster)..." -ForegroundColor Cyan
$driveB = New-RetoolVhdx -VhdxPath $vhdxB -SizeMB 1024 -ClusterSizeBytes 65536 -VolumeLabel 'RetoolB'

Write-Host "==> Creating VHDX C (ReFS 4K cluster)..." -ForegroundColor Cyan
$driveC = New-RetoolVhdx -VhdxPath $vhdxC -SizeMB 1024 -ClusterSizeBytes 4096  -VolumeLabel 'RetoolC'

# ── 5. Write environment file ────────────────────────────────────────────────
Write-Host "==> Writing environment file: $envFile" -ForegroundColor Cyan
@"
# Auto-generated by 00_setup.ps1 — do not edit manually.
`$env:RETOOL_EXE    = '$retoolExe'
`$env:RETOOL_DRIVE_A = '${driveA}:'
`$env:RETOOL_DRIVE_B = '${driveB}:'
`$env:RETOOL_DRIVE_C = '${driveC}:'
`$env:RETOOL_TEST_FILE = '$testFile'
`$env:RETOOL_TEST_HASH = '$testHash'
`$env:RETOOL_VHDX_A   = '$vhdxA'
`$env:RETOOL_VHDX_B   = '$vhdxB'
`$env:RETOOL_VHDX_C   = '$vhdxC'
"@ | Set-Content $envFile -Encoding UTF8

Write-Host ""
Write-Host "==> Setup complete." -ForegroundColor Green
Write-Host "    Drive A (ReFS 64K): ${driveA}:"
Write-Host "    Drive B (ReFS 64K): ${driveB}:"
Write-Host "    Drive C (ReFS  4K): ${driveC}:"
Write-Host "    Test file:          $testFile"
Write-Host "    Test file SHA-256:  $testHash"
Write-Host ""
Write-Host "    Run test scripts in numeric order.  Dot-source .retool_env.ps1 if" -ForegroundColor Yellow
Write-Host "    you open a new shell between scripts." -ForegroundColor Yellow
