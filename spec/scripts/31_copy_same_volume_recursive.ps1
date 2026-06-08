#Requires -RunAsAdministrator
<#
.SYNOPSIS
    31_copy_same_volume_recursive.ps1 - Test: retool copy <src-dir> <dest-dir> -r

.DESCRIPTION
    Creates a source directory on Drive A with two copies of the test file,
    then uses 'retool copy -r' to clone the entire directory tree to a second
    location on the same volume.
    Verifies:
    - Exit code 0
    - Destination directory and both files exist
    - Both destination files hash correctly
    - Source files are unmodified
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy - recursive same-volume directory copy (-r)" -ForegroundColor White

# -- Arrange -------------------------------------------------------------------
Write-Section "Arrange"
$drv    = $env:RETOOL_DRIVE_A -replace ':',''
$srcDir = "{0}:\copy_src_dir" -f $drv
$dstDir = "{0}:\copy_dst_dir" -f $drv

# Clean up any prior runs
Remove-Item $srcDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dstDir -Recurse -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Path $srcDir | Out-Null
New-Item -ItemType Directory -Path ("{0}\sub" -f $srcDir) | Out-Null
Copy-Item $env:RETOOL_TEST_FILE ("{0}\file1.bin" -f $srcDir) -Force
Copy-Item $env:RETOOL_TEST_FILE ("{0}\sub\file2.bin" -f $srcDir) -Force

Assert-FileExists -Path ("{0}\file1.bin" -f $srcDir)     -Description "srcDir\file1.bin present"
Assert-FileExists -Path ("{0}\sub\file2.bin" -f $srcDir) -Description "srcDir\sub\file2.bin present"

# -- Act -----------------------------------------------------------------------
Write-Section "Run: retool copy <srcDir> <dstDir> -r"
$out = Invoke-Retool -Args @('copy', $srcDir, $dstDir, '-r')
& $env:RETOOL_EXE copy $srcDir $dstDir -r | Out-Null
$exitCode = $LASTEXITCODE

# -- Assert --------------------------------------------------------------------
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
Assert-FileExists -Path ("{0}\file1.bin" -f $dstDir)     -Description "dstDir\file1.bin created"
Assert-FileExists -Path ("{0}\sub\file2.bin" -f $dstDir) -Description "dstDir\sub\file2.bin created"
Assert-HashMatch -Path ("{0}\file1.bin" -f $dstDir)     -ExpectedHash $env:RETOOL_TEST_HASH -Description "file1.bin data correct"
Assert-HashMatch -Path ("{0}\sub\file2.bin" -f $dstDir) -ExpectedHash $env:RETOOL_TEST_HASH -Description "file2.bin data correct"
Assert-HashMatch -Path ("{0}\file1.bin" -f $srcDir)     -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source file1.bin unchanged"
Assert-HashMatch -Path ("{0}\sub\file2.bin" -f $srcDir) -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source file2.bin unchanged"

# -- Cleanup -------------------------------------------------------------------
Remove-Item $srcDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dstDir -Recurse -Force -ErrorAction SilentlyContinue

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
