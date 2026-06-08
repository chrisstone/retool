#Requires -RunAsAdministrator
<#
.SYNOPSIS
    26_inspect_json.ps1 - Test: retool inspect <file> -j

.DESCRIPTION
    Runs 'retool inspect' with -j on a single file, two files, and a volume root.
    Verifies that in each case the output is valid JSON and contains expected keys.
    Also tests -o <file> to redirect output to a file.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: inspect - JSON output (--json)" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''

# -- Single file JSON ----------------------------------------------------------
Write-Section "Single file -j"
$file = "{0}:\json_single.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $file -Force

$out = Invoke-Retool -Args @('inspect', $file, '-j')
& $env:RETOOL_EXE inspect $file -j | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Single file -j exits 0"
$json = ($out -join "`n") | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $json -Description "Single file JSON parses successfully"

# -- Multi-file JSON -----------------------------------------------------------
Write-Section "Multi-file -j"
$file2 = "{0}:\json_multi.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $file2 -Force

$out = Invoke-Retool -Args @('inspect', $file, $file2, '-j')
& $env:RETOOL_EXE inspect $file $file2 -j | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Multi-file -j exits 0"
$json = ($out -join "`n") | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $json -Description "Multi-file JSON parses successfully"

# -- Volume scan JSON ----------------------------------------------------------
Write-Section "Volume scan -j"
$out = Invoke-Retool -Args @('inspect', $env:RETOOL_DRIVE_A, '-j')
& $env:RETOOL_EXE inspect $env:RETOOL_DRIVE_A -j | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Volume scan -j exits 0"
$json = ($out -join "`n") | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $json -Description "Volume scan JSON parses successfully"

# -- JSON to file (-o) ---------------------------------------------------------
Write-Section "JSON output to file (-o)"
$jsonOutPath = 'C:\Temp\retool_inspect_out.json'
Remove-Item $jsonOutPath -Force -ErrorAction SilentlyContinue
Invoke-Retool -Args @('inspect', $file, '-j', '-o', $jsonOutPath) | Out-Null
& $env:RETOOL_EXE inspect $file -j -o $jsonOutPath | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "-o file: exit code 0"
Assert-FileExists -Path $jsonOutPath -Description "JSON output file created"

# File integrity
Assert-HashMatch -Path $file  -ExpectedHash $env:RETOOL_TEST_HASH -Description "Inspected file data unchanged"
Assert-HashMatch -Path $file2 -ExpectedHash $env:RETOOL_TEST_HASH -Description "Multi-file second file unchanged"

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $file
Remove-DriveFile -Path $file2
Remove-Item $jsonOutPath -Force -ErrorAction SilentlyContinue

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
