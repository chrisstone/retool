#Requires -RunAsAdministrator
<#
.SYNOPSIS
    36_copy_json.ps1 — Test: retool copy <src> <dest> --json

.DESCRIPTION
    Runs 'retool copy --json' for same-volume and cross-volume cases.
    Verifies:
    - Exit code 0
    - Output is valid JSON
    - Destination files have correct hashes
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy — JSON output (--json)" -ForegroundColor White

$drvA = $env:RETOOL_DRIVE_A -replace ':',''
$drvB = $env:RETOOL_DRIVE_B -replace ':',''

# ── Same-volume JSON ──────────────────────────────────────────────────────────
Write-Section "Same-volume copy --json"
$src  = "${drvA}:\json_copy_src.bin"
$dest = "${drvA}:\json_copy_dest.bin"
Copy-Item $env:RETOOL_TEST_FILE $src -Force
Remove-DriveFile -Path $dest

$out = Invoke-Retool -Args @('copy', $src, $dest, '--json')
& $env:RETOOL_EXE copy $src $dest --json | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Same-volume --json exits 0"
$json = ($out -join "`n") | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $json -Description "Same-volume JSON output parses"
Assert-FileExists -Path $dest                     -Description "Destination exists"
Assert-HashMatch -Path $dest -ExpectedHash $env:RETOOL_TEST_HASH -Description "Destination data correct"

# ── Cross-volume JSON ─────────────────────────────────────────────────────────
Write-Section "Cross-volume copy --json"
$destB = "${drvB}:\json_copy_dest_b.bin"
Remove-DriveFile -Path $destB

$out = Invoke-Retool -Args @('copy', $src, $destB, '--json')
& $env:RETOOL_EXE copy $src $destB --json | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Cross-volume --json exits 0"
$json = ($out -join "`n") | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $json -Description "Cross-volume JSON output parses"
Assert-FileExists -Path $destB                     -Description "Cross-volume destination exists"
Assert-HashMatch -Path $destB -ExpectedHash $env:RETOOL_TEST_HASH -Description "Cross-volume data correct"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $src
Remove-DriveFile -Path $dest
Remove-DriveFile -Path $destB

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
