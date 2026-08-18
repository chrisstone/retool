#Requires -RunAsAdministrator
<#
.SYNOPSIS
    55_output_redirect.ps1 - Test: retool -o <file> for volume, copy, and dedup.

.DESCRIPTION
    26_inspect_json.ps1 already covers -o for inspect. copy.cpp, dedup.cpp, and
    volume.cpp all accept the same global -o flag (CliOutput redirects all
    output() writes to the file instead of stdout - output.cpp:40-43), but none
    of the copy/dedup/volume test scripts exercise it.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: output redirect (-o) for volume, copy, dedup" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''

# -- volume -o --------------------------------------------------------------
Write-Section "volume -o"
$volOut = 'C:\Temp\retool_volume_out.txt'
Remove-Item $volOut -Force -ErrorAction SilentlyContinue
Invoke-Retool -Args @('volume', $env:RETOOL_DRIVE_A, '-o', $volOut) | Out-Null
& $env:RETOOL_EXE volume $env:RETOOL_DRIVE_A -o $volOut | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "volume -o exits 0"
Assert-FileExists -Path $volOut -Description "volume output file created"
$volContent = Get-Content $volOut -Raw
Assert-OutputContains -Output $volContent -Substring 'ReFS' -Description "volume output file contains expected content"

# -- copy -o ------------------------------------------------------------------
Write-Section "copy -o"
$src = "{0}:\out_copy_src.bin" -f $drv
$dest = "{0}:\out_copy_dest.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $src -Force
Remove-DriveFile -Path $dest
$copyOut = 'C:\Temp\retool_copy_out.txt'
Remove-Item $copyOut -Force -ErrorAction SilentlyContinue

Invoke-Retool -Args @('copy', $src, $dest, '-o', $copyOut) | Out-Null
& $env:RETOOL_EXE copy $src $dest -o $copyOut | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "copy -o exits 0"
Assert-FileExists -Path $copyOut -Description "copy output file created"
$copyContent = Get-Content $copyOut -Raw
Assert-OutputContains -Output $copyContent -Substring 'Total Files' -Description "copy output file contains summary"
Assert-HashMatch -Path $dest -ExpectedHash $env:RETOOL_TEST_HASH -Description "copy -o still copied correct data"

# -- dedup -o (dry-run, to avoid side effects) ---------------------------------
Write-Section "dedup -o -n"
$fileB = "{0}:\out_dedup_b.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $fileB -Force
$dedupOut = 'C:\Temp\retool_dedup_out.txt'
Remove-Item $dedupOut -Force -ErrorAction SilentlyContinue

Invoke-Retool -Args @('dedup', $src, $fileB, '-n', '-o', $dedupOut) | Out-Null
& $env:RETOOL_EXE dedup $src $fileB -n -o $dedupOut | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "dedup -o -n exits 0"
Assert-FileExists -Path $dedupOut -Description "dedup output file created"
$dedupContent = Get-Content $dedupOut -Raw
Assert-OutputContains -Output $dedupContent -Substring 'Clusters Deduped' -Description "dedup output file contains summary"

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $src
Remove-DriveFile -Path $dest
Remove-DriveFile -Path $fileB
Remove-Item $volOut, $copyOut, $dedupOut -Force -ErrorAction SilentlyContinue

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
