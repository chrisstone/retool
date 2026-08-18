#Requires -RunAsAdministrator
<#
.SYNOPSIS
    00_setup.ps1 - Test environment setup for retool spec scripts.

.DESCRIPTION
    1. Verifies that build/debug/Debug/retool.exe exists (relative to repo root).
    2. Creates C:\Temp if it does not exist.
    3. Creates a 50 MB binary test file at C:\Temp\testfile.bin.
    4. Creates, attaches, initialises, and mounts three 1 GB VHDX files:
         - retool-a.vhdx  (ReFS, 64 K cluster) -> expected drive letter stored in $env:RETOOL_DRIVE_A
         - retool-b.vhdx  (ReFS, 64 K cluster) -> $env:RETOOL_DRIVE_B
         - retool-c.vhdx  (ReFS,  4 K cluster) -> $env:RETOOL_DRIVE_C
    5. Writes a shared environment file (spec\scripts\.retool_env.ps1) that all
       subsequent test scripts dot-source to pick up drive-letter variables.

    RATIONALE FOR 1 GB SIZE:
    ReFS metadata overhead on a 1 GB volume consumes the majority of available
    clusters, leaving just enough room for one 50 MB test file.  All subsequent
    "copies" of that file (without dedup) fill the disk, forcing retool's
    deduplication code paths to be exercised before additional files can coexist.

    RE-RUNNING WITHOUT TEARDOWN:
    If leftover state from a previous run (env file, VHDX files, or a mounted
    Retool* volume) is detected, this script refuses to proceed and tells you
    to run 99_teardown.ps1 first - or pass -Force to wipe and recreate the
    environment directly. This prevents accidentally destroying a test
    environment someone is still mid-investigation of.

.PARAMETER Force
    Proceed even if leftover state from a previous, uncleaned run is detected -
    wiping and recreating the environment (env file, VHDXs, mounted volumes).
    Without this switch, leftover state causes the script to abort untouched.
#>

param(
    [switch] $Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# -- Paths --------------------------------------------------------------------
$scriptDir  = $PSScriptRoot
$repoRoot   = Resolve-Path (Join-Path $scriptDir '..\..')
$retoolExe  = Join-Path $repoRoot 'build\debug\Debug\retool.exe'
$vhdxDir    = 'C:\Temp'
$envFile    = Join-Path $scriptDir '.retool_env.ps1'
$testFile   = Join-Path $vhdxDir 'testfile.bin'
$vhdxA      = Join-Path $vhdxDir 'retool-a.vhdx'
$vhdxB      = Join-Path $vhdxDir 'retool-b.vhdx'
$vhdxC      = Join-Path $vhdxDir 'retool-c.vhdx'

# -- 0. Detect leftover state from a previous, uncleaned run -------------------
Write-Host "==> Checking for leftover state from a previous run..." -ForegroundColor Cyan
$leftovers = @()
if (Test-Path $envFile) { $leftovers += ("Environment file: {0}" -f $envFile) }
foreach ($vhdx in @($vhdxA, $vhdxB, $vhdxC)) {
    if (Test-Path $vhdx) { $leftovers += ("VHDX file: {0}" -f $vhdx) }
}
$orphanVolumes = Get-Volume -ErrorAction SilentlyContinue |
    Where-Object { $_.FileSystemLabel -in @('RetoolA', 'RetoolB', 'RetoolC') }
foreach ($vol in $orphanVolumes) {
    $leftovers += ("Mounted volume: {0}: (label={1})" -f $vol.DriveLetter, $vol.FileSystemLabel)
}

if ($leftovers.Count -gt 0 -and -not $Force) {
    Write-Host ""
    Write-Host "==> Found leftover state from a previous run that was not torn down:" -ForegroundColor Red
    foreach ($item in $leftovers) {
        Write-Host ("    - {0}" -f $item) -ForegroundColor Yellow
    }
    Write-Host ""
    Write-Host "    Run 99_teardown.ps1 first, or re-run this script with -Force to" -ForegroundColor Yellow
    Write-Host "    detach/delete/reformat this leftover state and start clean." -ForegroundColor Yellow
    Write-Error "Refusing to overwrite an existing, uncleaned test environment."
    exit 1
} elseif ($leftovers.Count -gt 0) {
    Write-Host "==> -Force specified - wiping leftover state from a previous run:" -ForegroundColor Yellow
    foreach ($item in $leftovers) {
        Write-Host ("    - {0}" -f $item) -ForegroundColor Yellow
    }
} else {
    Write-Host "    No leftover state found." -ForegroundColor Green
}

# -- 1. Verify retool.exe -----------------------------------------------------
Write-Host "==> Verifying retool.exe..." -ForegroundColor Cyan
if (-not (Test-Path $retoolExe)) {
    Write-Error ("retool.exe not found at: {0}`nPlease build the project first: cmake --build build/debug --config Debug" -f $retoolExe)
    exit 1
}
Write-Host ("    Found: {0}" -f $retoolExe) -ForegroundColor Green

# -- 2. Create C:\Temp --------------------------------------------------------
Write-Host "==> Creating C:\Temp..." -ForegroundColor Cyan
if (-not (Test-Path $vhdxDir)) {
    New-Item -ItemType Directory -Path $vhdxDir | Out-Null
    Write-Host "    Created C:\Temp" -ForegroundColor Green
} else {
    Write-Host "    C:\Temp already exists" -ForegroundColor Yellow
}

# -- 3. Create 50 MB test file -----------------------------------------------
Write-Host ("==> Creating 50 MB test file at {0}..." -f $testFile) -ForegroundColor Cyan
if (Test-Path $testFile) {
    Write-Host "    Removing existing test file" -ForegroundColor Yellow
    Remove-Item $testFile -Force
}

# Use a fixed random seed so the file is reproducible for hash verification
$rng = [System.Random]::new(42)
$buf = [byte[]]::new(1MB)
$fs  = [System.IO.File]::OpenWrite($testFile)
try {
    for ($i = 0; $i -lt 50; $i++) {
        $rng.NextBytes($buf)
        $fs.Write($buf, 0, $buf.Length)
    }
} finally {
    $fs.Close()
}
$testHash = (Get-FileHash $testFile -Algorithm SHA256).Hash
Write-Host ("    Created {0}  SHA-256: {1}" -f $testFile, $testHash) -ForegroundColor Green

# -- Helper: create, attach, format, and mount a VHDX ------------------------
function New-RetoolVhdx {
    param(
        [string] $VhdxPath,
        [int]    $SizeMB,
        [int]    $ClusterSizeBytes,
        [string] $VolumeLabel
    )

    if (Test-Path $VhdxPath) {
        Write-Host ("    Removing existing VHDX: {0}" -f $VhdxPath) -ForegroundColor Yellow
        # Detach if mounted using diskpart
        $tempScript = [System.IO.Path]::GetTempFileName()
        $commands = @(
            ('select vdisk file="{0}"' -f $VhdxPath),
            "detach vdisk"
        )
        $commands | Set-Content $tempScript -Encoding ASCII
        diskpart /s $tempScript | Out-Null
        Remove-Item $tempScript -Force -ErrorAction SilentlyContinue
        Remove-Item $VhdxPath -Force
    }

    Write-Host ("    Creating VHDX: {0} ({1} MB, cluster={2} bytes)" -f $VhdxPath, $SizeMB, $ClusterSizeBytes) -ForegroundColor Cyan

    # Find a free drive letter (D..Z)
    $usedLetters = [System.IO.DriveInfo]::GetDrives() | ForEach-Object { $_.Name.Substring(0, 1).ToUpper() }
    $driveLetter = $null
    foreach ($letter in "K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z") {
        if ($letter -notin $usedLetters) {
            $driveLetter = $letter
            break
        }
    }
    if ($null -eq $driveLetter) {
        throw "No free drive letters available!"
    }

    # Create script for diskpart to build, mount, partition, format ReFS, and assign letter
    $tempScript = [System.IO.Path]::GetTempFileName()
    $commands = @(
        ('create vdisk file="{0}" maximum={1} type=expandable' -f $VhdxPath, $SizeMB),
        ('select vdisk file="{0}"' -f $VhdxPath),
        "attach vdisk",
        "convert gpt",
        "create partition primary",
        ('format fs=ReFS unit={0} label="{1}" quick' -f $ClusterSizeBytes, $VolumeLabel),
        ('assign letter={0}' -f $driveLetter)
    )
    $commands | Set-Content $tempScript -Encoding ASCII

    diskpart /s $tempScript | Out-Null
    Remove-Item $tempScript -Force -ErrorAction SilentlyContinue

    if (-not (Test-Path ("{0}:\" -f $driveLetter))) {
        throw ("Failed to create/mount VHDX at drive {0}:" -f $driveLetter)
    }

    Write-Host ("    Mounted as {0}: (label={1})" -f $driveLetter, $VolumeLabel) -ForegroundColor Green
    return $driveLetter
}

# -- 4. Create three VHDXs ----------------------------------------------------
Write-Host "==> Creating VHDX A (ReFS 64K cluster)..." -ForegroundColor Cyan
$driveA = New-RetoolVhdx -VhdxPath $vhdxA -SizeMB 1024 -ClusterSizeBytes 65536 -VolumeLabel 'RetoolA'

Write-Host "==> Creating VHDX B (ReFS 64K cluster)..." -ForegroundColor Cyan
$driveB = New-RetoolVhdx -VhdxPath $vhdxB -SizeMB 1024 -ClusterSizeBytes 65536 -VolumeLabel 'RetoolB'

Write-Host "==> Creating VHDX C (ReFS 4K cluster)..." -ForegroundColor Cyan
$driveC = New-RetoolVhdx -VhdxPath $vhdxC -SizeMB 1024 -ClusterSizeBytes 4096  -VolumeLabel 'RetoolC'

# -- 5. Write environment file ------------------------------------------------
Write-Host ("==> Writing environment file: {0}" -f $envFile) -ForegroundColor Cyan
@'
# Auto-generated by 00_setup.ps1 - do not edit manually.
$env:RETOOL_EXE    = '{0}'
$env:RETOOL_DRIVE_A = '{1}:'
$env:RETOOL_DRIVE_B = '{2}:'
$env:RETOOL_DRIVE_C = '{3}:'
$env:RETOOL_TEST_FILE = '{4}'
$env:RETOOL_TEST_HASH = '{5}'
$env:RETOOL_VHDX_A   = '{6}'
$env:RETOOL_VHDX_B   = '{7}'
$env:RETOOL_VHDX_C   = '{8}'
'@ -f $retoolExe, $driveA, $driveB, $driveC, $testFile, $testHash, $vhdxA, $vhdxB, $vhdxC | Set-Content $envFile -Encoding UTF8

Write-Host ""
Write-Host "==> Setup complete." -ForegroundColor Green
Write-Host ("    Drive A (ReFS 64K): {0}:" -f $driveA)
Write-Host ("    Drive B (ReFS 64K): {0}:" -f $driveB)
Write-Host ("    Drive C (ReFS  4K): {0}:" -f $driveC)
Write-Host ("    Test file:          {0}" -f $testFile)
Write-Host ("    Test file SHA-256:  {0}" -f $testHash)
Write-Host ""
Write-Host "    Run test scripts in numeric order.  Dot-source .retool_env.ps1 if" -ForegroundColor Yellow
Write-Host "    you open a new shell between scripts." -ForegroundColor Yellow
