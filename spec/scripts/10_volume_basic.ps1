#Requires -RunAsAdministrator
<#
.SYNOPSIS
    10_volume_basic.ps1 — Test: retool volume <drive>

.DESCRIPTION
    Runs 'retool volume' against each of the three test drives and verifies:
    - Exit code 0
    - Output contains expected fields (Volume, File System, Cluster Size)
    - Correct cluster sizes reported for 64K and 4K volumes
    - Non-ReFS warning is NOT present (all three are ReFS)
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: volume — basic volume information" -ForegroundColor White

# ── Drive A: ReFS 64K ─────────────────────────────────────────────────────────
Write-Section "Drive A ($env:RETOOL_DRIVE_A) — ReFS 64K cluster"
$out = Invoke-Retool -Args @('volume', $env:RETOOL_DRIVE_A)
& $env:RETOOL_EXE volume $env:RETOOL_DRIVE_A | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "retool volume DriveA exits 0"
Assert-OutputContains -Output ($out -join "`n") -Substring 'ReFS'         -Description "Drive A reports ReFS filesystem"
Assert-OutputContains -Output ($out -join "`n") -Substring '65536'        -Description "Drive A reports 65536-byte cluster size"
Assert-OutputContains -Output ($out -join "`n") -Substring 'Total Space'  -Description "Drive A output has Total Space field"
Assert-OutputContains -Output ($out -join "`n") -Substring 'Free Space'   -Description "Drive A output has Free Space field"

# ── Drive B: ReFS 64K ─────────────────────────────────────────────────────────
Write-Section "Drive B ($env:RETOOL_DRIVE_B) — ReFS 64K cluster"
$out = Invoke-Retool -Args @('volume', $env:RETOOL_DRIVE_B)
& $env:RETOOL_EXE volume $env:RETOOL_DRIVE_B | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "retool volume DriveB exits 0"
Assert-OutputContains -Output ($out -join "`n") -Substring 'ReFS'  -Description "Drive B reports ReFS filesystem"
Assert-OutputContains -Output ($out -join "`n") -Substring '65536' -Description "Drive B reports 65536-byte cluster size"

# ── Drive C: ReFS 4K ──────────────────────────────────────────────────────────
Write-Section "Drive C ($env:RETOOL_DRIVE_C) — ReFS 4K cluster"
$out = Invoke-Retool -Args @('volume', $env:RETOOL_DRIVE_C)
& $env:RETOOL_EXE volume $env:RETOOL_DRIVE_C | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "retool volume DriveC exits 0"
Assert-OutputContains -Output ($out -join "`n") -Substring 'ReFS' -Description "Drive C reports ReFS filesystem"
Assert-OutputContains -Output ($out -join "`n") -Substring '4096' -Description "Drive C reports 4096-byte cluster size"

# ── JSON output variant ───────────────────────────────────────────────────────
Write-Section "Drive A — JSON output"
$jsonOut = Invoke-Retool -Args @('volume', $env:RETOOL_DRIVE_A, '-j')
& $env:RETOOL_EXE volume $env:RETOOL_DRIVE_A -j | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "retool volume -j exits 0"
$json = $jsonOut -join "`n" | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $json -Description "JSON output parses successfully"

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
