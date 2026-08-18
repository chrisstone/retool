#Requires -RunAsAdministrator
<#
.SYNOPSIS
    63_copy_skip_reparse_system.ps1 - Regression test: recursive copy skips
    reparse points and system-attributed files/directories.

.DESCRIPTION
    util::enumerate_files_recursive explicitly skips FILE_ATTRIBUTE_SYSTEM and
    FILE_ATTRIBUTE_REPARSE_POINT (util.cpp:288-290), and checks reparse-point
    BEFORE deciding whether to recurse into something as a directory - meaning
    a directory junction should never be followed (avoiding both infinite
    recursion and copying content that lives elsewhere). No test in this suite
    creates either kind of item, so this path has zero coverage.

    Source directory contains: a normal file, a file with FILE_ATTRIBUTE_SYSTEM
    set, and a junction pointing at C:\Temp (an unrelated, always-present
    directory). Verifies a recursive copy:
    - Succeeds (exit 0) without erroring on the skipped items.
    - Copies the normal file correctly.
    - Does NOT copy the system-attributed file.
    - Does NOT follow the junction (nothing from C:\Temp leaks into the
      destination, and no infinite recursion / hang occurs).
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy - recursive copy skips reparse points and system files" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''
$srcDir = "{0}:\skip_test_src" -f $drv
$dstDir = "{0}:\skip_test_dst" -f $drv
Remove-Item $srcDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dstDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $srcDir | Out-Null

# -- Normal file -----------------------------------------------------------
$normalFile = "{0}\normal.bin" -f $srcDir
Copy-Item $env:RETOOL_TEST_FILE $normalFile -Force
Assert-FileExists -Path $normalFile -Description "Normal file created"

# -- System-attributed file -------------------------------------------------
$systemFile = "{0}\system.bin" -f $srcDir
[System.IO.File]::WriteAllBytes($systemFile, [byte[]]::new(65536))
$item = Get-Item $systemFile -Force
$item.Attributes = $item.Attributes -bor [System.IO.FileAttributes]::System
Assert-FileExists -Path $systemFile -Description "System-attributed file created"

# -- Junction (directory reparse point) pointing at an unrelated directory --
$junctionPath = "{0}\link_to_temp" -f $srcDir
New-Item -ItemType Junction -Path $junctionPath -Target 'C:\Temp' -ErrorAction SilentlyContinue | Out-Null
$junctionOk = (Get-Item $junctionPath -Force -ErrorAction SilentlyContinue).Attributes -band [System.IO.FileAttributes]::ReparsePoint
if ($junctionOk) {
    Write-Host "    [PASS] Junction created" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Failed to create junction - cannot test reparse-point skipping" -ForegroundColor Red
    $script:TestsFailed++
}

# ===============================================================================
# Act
# ===============================================================================
Write-Section "Run: retool copy <srcDir> <dstDir> -r"

$out = Invoke-Retool -Args @('copy', $srcDir, $dstDir, '-r')
& $env:RETOOL_EXE copy $srcDir $dstDir -r | Out-Null
$exitCode = $LASTEXITCODE
$outStr = $out -join "`n"

# ===============================================================================
# Assert
# ===============================================================================
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Recursive copy exits 0 (no error from skipped items)"

Assert-FileExists -Path ("{0}\normal.bin" -f $dstDir) -Description "Normal file was copied"
Assert-HashMatch -Path ("{0}\normal.bin" -f $dstDir) -ExpectedHash $env:RETOOL_TEST_HASH -Description "Normal file data correct"

if (-not (Test-Path ("{0}\system.bin" -f $dstDir))) {
    Write-Host "    [PASS] System-attributed file was NOT copied" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] System-attributed file was copied despite the SYSTEM attribute skip" -ForegroundColor Red
    $script:TestsFailed++
}

if (-not (Test-Path ("{0}\link_to_temp" -f $dstDir))) {
    Write-Host "    [PASS] Junction was NOT followed/copied" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Junction was copied/followed despite the REPARSE_POINT skip" -ForegroundColor Red
    $script:TestsFailed++
}

# Whatever the destination directory ended up with, it must not contain
# anything sourced from C:\Temp (proof the junction was never followed).
$destItemCount = (Get-ChildItem $dstDir -Recurse -Force -ErrorAction SilentlyContinue | Measure-Object).Count
if ($destItemCount -le 1) {
    Write-Host ("    [PASS] Destination contains only the expected normal.bin ({0} item(s))" -f $destItemCount) -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host ("    [FAIL] Destination contains {0} items - expected only normal.bin" -f $destItemCount) -ForegroundColor Red
    $script:TestsFailed++
}

# -- Cleanup -------------------------------------------------------------------
# Remove the junction itself first (not its target's contents) to avoid
# accidentally recursing into C:\Temp during cleanup.
if (Test-Path $junctionPath) { (Get-Item $junctionPath -Force).Delete() }
Remove-Item $srcDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dstDir -Recurse -Force -ErrorAction SilentlyContinue

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
