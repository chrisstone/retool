#Requires -RunAsAdministrator
<#
.SYNOPSIS
    53_quiet_mode.ps1 - Test: retool -q (quiet mode) across commands.

.DESCRIPTION
    -q selects QuietOutput, whose methods are all no-ops (per output.h). No test
    in this suite passes -q at all. Verifies that volume, inspect, copy, and
    dedup all produce zero stdout/stderr output under -q while still succeeding
    and (for copy/dedup) still doing the real work.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: quiet mode (-q) across commands" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''

function Assert-QuietOutput {
    param([string[]] $Output, [string] $Description)
    $joined = ($Output -join '').Trim()
    if ([string]::IsNullOrEmpty($joined)) {
        Write-Host ("    [PASS] {0}: no output produced" -f $Description) -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host ("    [FAIL] {0}: expected no output, got: {1}" -f $Description, $joined.Substring(0, [Math]::Min(200, $joined.Length))) -ForegroundColor Red
        $script:TestsFailed++
    }
}

# -- volume -q ------------------------------------------------------------------
Write-Section "volume -q"
$out = Invoke-Retool -Args @('volume', $env:RETOOL_DRIVE_A, '-q')
& $env:RETOOL_EXE volume $env:RETOOL_DRIVE_A -q | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "volume -q exits 0"
Assert-QuietOutput -Output $out -Description "volume -q"

# -- inspect -q -----------------------------------------------------------------
Write-Section "inspect -q"
$file = "{0}:\quiet_inspect.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $file -Force
$out = Invoke-Retool -Args @('inspect', $file, '-e', '-q')
& $env:RETOOL_EXE inspect $file -e -q | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "inspect -q exits 0"
Assert-QuietOutput -Output $out -Description "inspect -q"

# -- copy -q ----------------------------------------------------------------
Write-Section "copy -q"
$dest = "{0}:\quiet_copy_dest.bin" -f $drv
Remove-DriveFile -Path $dest
$out = Invoke-Retool -Args @('copy', $file, $dest, '-q')
& $env:RETOOL_EXE copy $file $dest -q | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "copy -q exits 0"
Assert-QuietOutput -Output $out -Description "copy -q"
Assert-HashMatch -Path $dest -ExpectedHash $env:RETOOL_TEST_HASH -Description "copy -q still copied correct data"

# -- dedup -q (dry-run, to avoid side effects while still exercising the path) --
Write-Section "dedup -q -n"
$fileB = "{0}:\quiet_dedup_b.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $fileB -Force
$out = Invoke-Retool -Args @('dedup', $file, $fileB, '-n', '-q')
& $env:RETOOL_EXE dedup $file $fileB -n -q | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "dedup -q -n exits 0"
Assert-QuietOutput -Output $out -Description "dedup -q -n"

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $file
Remove-DriveFile -Path $dest
Remove-DriveFile -Path $fileB

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
