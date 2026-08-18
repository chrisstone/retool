#Requires -RunAsAdministrator
<#
.SYNOPSIS
    11_volume_edge_cases.ps1 - Test: retool volume path resolution and error handling.

.DESCRIPTION
    10_volume_basic.ps1 only ever passes a bare drive letter ("E:") to retool
    volume. volume.cpp's resolve_volume_info(path) accepts any path on a volume
    (via GetVolumePathNameW), and prepare() has its own missing-argument
    rejection - neither is exercised anywhere in this suite.

    Sections:
    1. Subdirectory path input (C:\Temp) resolves to the C:\ volume root.
    2. File path input (a file on Drive A) resolves to Drive A's volume root.
    3. Missing volume argument is rejected.
    4. Non-existent drive letter is rejected with a Win32-derived error
       (skipped, not failed, if Z: unexpectedly exists on this machine).
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: volume - path resolution and error handling" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''

# ===============================================================================
# Section 1: subdirectory path input
# ===============================================================================
Write-Section "Section 1: subdirectory path input (C:\Temp)"

$out = Invoke-Retool -Args @('volume', 'C:\Temp')
& $env:RETOOL_EXE volume 'C:\Temp' | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "volume C:\Temp exits 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'C:\' -Description "Resolves to the C:\ volume root"
Assert-OutputContains -Output $outStr -Substring 'NTFS' -Description "Reports NTFS filesystem"

# ===============================================================================
# Section 2: file path input
# ===============================================================================
Write-Section "Section 2: file path input (a file on Drive A)"

$file = "{0}:\vol_edge_test.bin" -f $drv
Remove-DriveFile -Path $file
New-Item -ItemType File -Path $file -Force | Out-Null
Assert-FileExists -Path $file -Description "Test file created on Drive A"

$out = Invoke-Retool -Args @('volume', $file)
& $env:RETOOL_EXE volume $file | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "volume <file path> exits 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring ("{0}:\" -f $drv) -Description "Resolves to Drive A's volume root"
Assert-OutputContains -Output $outStr -Substring 'ReFS' -Description "Reports ReFS filesystem"

Remove-DriveFile -Path $file

# ===============================================================================
# Section 3: missing volume argument
# ===============================================================================
Write-Section "Section 3: missing volume argument"

$out = Invoke-Retool -Args @('volume') -AllowFailure
# volume::prepare() rejects this (not util::parse_arguments()), so it's a
# command-level failure (exit 2) like any other prepare()/execute() error -
# not a CLI-syntax failure (which would be exit 1).
Assert-ExitCode -Expected 2 -Actual $LASTEXITCODE -Description "volume with no argument exits 2"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Missing volume path argument' -Description "Reports missing argument error"

# ===============================================================================
# Section 4: non-existent drive letter
# ===============================================================================
Write-Section "Section 4: non-existent drive letter"

if (Test-Path 'Z:\') {
    Write-Host "    [SKIP] Z: unexpectedly exists on this machine - cannot test as a non-existent drive" -ForegroundColor Yellow
} else {
    $out = Invoke-Retool -Args @('volume', 'Z:') -AllowFailure
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("    [PASS] volume Z: (non-existent) fails (exit {0})" -f $LASTEXITCODE) -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] volume Z: (non-existent) unexpectedly succeeded" -ForegroundColor Red
        $script:TestsFailed++
    }
}

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
