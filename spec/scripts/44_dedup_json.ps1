#Requires -RunAsAdministrator
<#
.SYNOPSIS
    44_dedup_json.ps1 — Test: retool dedup <volume> -j and retool dedup <f1> <f2> -j

.DESCRIPTION
    Tests JSON output mode for both volume-wide and pair-wise dedup.
    Verifies:
    - Exit code 0
    - Output is valid JSON for both modes
    - Data integrity preserved
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: dedup — JSON output (--json)" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''

# ── Volume-wide dedup JSON (dry-run to avoid side effects) ────────────────────
Write-Section "Volume-wide dedup -j -n"
$fileA = "${drv}:\dedup_json_a.bin"
$fileB = "${drv}:\dedup_json_b.bin"
Copy-Item $env:RETOOL_TEST_FILE $fileA -Force
Copy-Item $env:RETOOL_TEST_FILE $fileB -Force

$out = Invoke-Retool -Args @('dedup', $env:RETOOL_DRIVE_A, '-n', '-j')
& $env:RETOOL_EXE dedup $env:RETOOL_DRIVE_A -n -j | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Volume-wide -j exits 0"
$json = ($out -join "`n") | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $json -Description "Volume-wide JSON parses successfully"
Assert-HashMatch -Path $fileA -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileA intact"
Assert-HashMatch -Path $fileB -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileB intact"

# ── Pair-wise dedup JSON (dry-run) ─────────────────────────────────────────────
Write-Section "Pair-wise dedup -j -n"
$out = Invoke-Retool -Args @('dedup', $fileA, $fileB, '-n', '-j')
& $env:RETOOL_EXE dedup $fileA $fileB -n -j | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Pair-wise -j exits 0"
$json = ($out -join "`n") | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $json -Description "Pair-wise JSON parses successfully"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $fileA
Remove-DriveFile -Path $fileB

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
