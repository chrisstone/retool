#Requires -RunAsAdministrator
<#
.SYNOPSIS
    31_copy_same_volume_recursive.ps1 — Test: retool copy <src-dir> <dest-dir> -r

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

Write-Host "TEST: copy — recursive same-volume directory copy (-r)" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$drv    = $env:RETOOL_DRIVE_A -replace ':',''
$srcDir = "${drv}:\copy_src_dir"
$dstDir = "${drv}:\copy_dst_dir"

# Clean up any prior runs
Remove-Item $srcDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dstDir -Recurse -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Path $srcDir | Out-Null
New-Item -ItemType Directory -Path "$srcDir\sub" | Out-Null
Copy-Item $env:RETOOL_TEST_FILE "$srcDir\file1.bin" -Force
Copy-Item $env:RETOOL_TEST_FILE "$srcDir\sub\file2.bin" -Force

Assert-FileExists -Path "$srcDir\file1.bin"     -Description "srcDir\file1.bin present"
Assert-FileExists -Path "$srcDir\sub\file2.bin" -Description "srcDir\sub\file2.bin present"

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section "Run: retool copy <srcDir> <dstDir> -r"
$out = Invoke-Retool -Args @('copy', $srcDir, $dstDir, '-r')
& $env:RETOOL_EXE copy $srcDir $dstDir -r | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
Assert-FileExists -Path "$dstDir\file1.bin"     -Description "dstDir\file1.bin created"
Assert-FileExists -Path "$dstDir\sub\file2.bin" -Description "dstDir\sub\file2.bin created"
Assert-HashMatch -Path "$dstDir\file1.bin"     -ExpectedHash $env:RETOOL_TEST_HASH -Description "file1.bin data correct"
Assert-HashMatch -Path "$dstDir\sub\file2.bin" -ExpectedHash $env:RETOOL_TEST_HASH -Description "file2.bin data correct"
Assert-HashMatch -Path "$srcDir\file1.bin"     -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source file1.bin unchanged"
Assert-HashMatch -Path "$srcDir\sub\file2.bin" -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source file2.bin unchanged"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-Item $srcDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dstDir -Recurse -Force -ErrorAction SilentlyContinue

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
