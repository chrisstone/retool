#Requires -RunAsAdministrator
<#
.SYNOPSIS
    56_copy_dest_nonrefs.ps1 - Regression test: retool copy to a non-ReFS
    destination (FallbackCopyStrategy's "Destination volume is non-ReFS" branch).

.DESCRIPTION
    FallbackCopyStrategy::copy_file has two distinct warning branches
    (copy.cpp:679-687):
      - "Destination volume cluster size mismatch" - covered by
        35_copy_fallback.ps1 (Drive A 64K -> Drive C 4K, both ReFS).
      - "Destination volume is non-ReFS" - covered by NOTHING in this suite.
        37_copy_small_files.ps1 uses C:\Temp as a SOURCE (same-volume and
        cross-volume-with-ReFS-dest), but nothing ever copies TO C:\Temp from
        a ReFS drive, so this specific branch and its exact warning text have
        never been exercised.

    Copies from Drive A (ReFS) to C:\Temp (the NTFS system drive) and verifies:
    - Exit code 0 (fallback succeeds)
    - Destination file exists with correct hash
    - Output specifically contains the non-ReFS warning text
    - Source file unmodified
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy - fallback to non-ReFS destination (Drive A -> C:\Temp)" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''
$src  = "{0}:\nonrefs_dest_src.bin" -f $drv
$dest = 'C:\Temp\nonrefs_dest_out.bin'
Copy-Item $env:RETOOL_TEST_FILE $src -Force
Remove-Item $dest -Force -ErrorAction SilentlyContinue
Assert-FileExists -Path $src -Description "Source file on Drive A (ReFS)"

$out = Invoke-Retool -Args @('copy', $src, $dest)
& $env:RETOOL_EXE copy $src $dest | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Exit code 0 (fallback succeeds)"
$outStr = $out -join "`n"

Assert-FileExists -Path $dest -Description "Destination file created on C:\Temp"
Assert-HashMatch -Path $dest -ExpectedHash $env:RETOOL_TEST_HASH -Description "Destination data correct"
Assert-HashMatch -Path $src  -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source file unchanged"
Assert-OutputContains -Output $outStr -Substring 'Destination volume is non-ReFS' `
    -Description "Output specifically identifies non-ReFS destination (not the cluster-mismatch branch)"

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $src
Remove-Item $dest -Force -ErrorAction SilentlyContinue

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
