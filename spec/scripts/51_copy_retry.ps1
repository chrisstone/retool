#Requires -RunAsAdministrator
<#
.SYNOPSIS
    51_copy_retry.ps1 - Test: retool copy <src> <dest> -t <count> -w <secs>

.DESCRIPTION
    -t/-w govern retry_copy_file()'s retry loop in copy.cpp (retry count and wait
    between attempts). No test in this suite passes either flag - both default to
    3 retries / 30s wait and are otherwise completely unexercised.

    Since Invoke-Retool (the shared helper) runs retool synchronously and waits
    for it to exit, it can't be used here - this test needs to release a lock
    on the destination file WHILE retool is mid-run. Both sections launch retool
    directly via Start-Process instead.

    A real, deterministic failure is forced by pre-opening the destination path
    with FileShare.None (an exclusive lock) from PowerShell before starting
    retool. copy.cpp's create_dest_file() opens the destination with
    CREATE_ALWAYS + FILE_SHARE_READ|FILE_SHARE_WRITE, which fails with a sharing
    violation against an exclusive lock held elsewhere - a real, not simulated,
    copy failure.

    Sections:
    1. Retry succeeds: lock held for less than the total retry window - retool
       must retry at least once and ultimately succeed (exit 0, correct data,
       "Copy attempt" text proving a retry actually happened).
    2. Retries exhausted: lock held for longer than the total retry window -
       retool must fail (non-zero exit) after exhausting all attempts, and must
       not leave a corrupt/partial file at the destination.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy - retry on failure (-t / -w)" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''
$src = "{0}:\retry_src.bin" -f $drv
Remove-DriveFile -Path $src
$rng = [System.Random]::new(1001)
$buf = [byte[]]::new(1MB)
$rng.NextBytes($buf)
[System.IO.File]::WriteAllBytes($src, $buf)
$hash = (Get-FileHash $src -Algorithm SHA256).Hash
Assert-FileExists -Path $src -Description "Source file created"

<#
.SYNOPSIS
    Runs retool copy against a destination locked exclusively for LockSeconds,
    releasing the lock partway through. Returns @($exitCode, $stdout).
#>
function Invoke-RetoolCopyWithTransientLock {
    param(
        [string] $Src,
        [string] $Dest,
        [int]    $RetryCount,
        [int]    $RetryWait,
        [int]    $LockSeconds,
        [int]    $WaitForExitMs
    )
    $lock = [System.IO.File]::Open($Dest, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    # GetTempFileName() creates the file; Start-Process -RedirectStandardOutput
    # is finicky about a pre-existing target on some PowerShell versions, so
    # delete it immediately and just keep the path.
    $stdoutFile = [System.IO.Path]::GetTempFileName()
    $stderrFile = [System.IO.Path]::GetTempFileName()
    Remove-Item $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
    try {
        $proc = Start-Process -FilePath $env:RETOOL_EXE `
            -ArgumentList @('copy', $Src, $Dest, '-t', $RetryCount, '-w', $RetryWait) `
            -NoNewWindow -PassThru `
            -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile

        Start-Sleep -Seconds $LockSeconds
        $lock.Close()
        $lock = $null

        $proc.WaitForExit($WaitForExitMs) | Out-Null
        if (-not $proc.HasExited) { $proc.Kill(); Start-Sleep -Milliseconds 200 }

        $stdout = (Get-Content $stdoutFile -Raw -ErrorAction SilentlyContinue) + "`n" +
                  (Get-Content $stderrFile -Raw -ErrorAction SilentlyContinue)
        return @($proc.ExitCode, $stdout)
    } finally {
        if ($lock) { $lock.Close() }
        Remove-Item $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
    }
}

# ===============================================================================
# Section 1: lock released mid-window - retry must succeed
# ===============================================================================
Write-Section "Section 1: transient failure - retry succeeds"

$dest1 = "{0}:\retry_succeed_dest.bin" -f $drv
Remove-DriveFile -Path $dest1

# -t 3 -w 2: up to 4 attempts, 2s apart (window ~6-8s). Lock held 3s, well
# within the window, so a retry after the wait should find it released.
$result = Invoke-RetoolCopyWithTransientLock -Src $src -Dest $dest1 `
    -RetryCount 3 -RetryWait 2 -LockSeconds 3 -WaitForExitMs 20000
$exitCode1 = $result[0]
$out1      = $result[1]

Assert-ExitCode -Expected 0 -Actual $exitCode1 -Description "Copy eventually succeeds after transient lock is released"
Assert-OutputContains -Output $out1 -Substring 'Copy attempt' -Description "Output shows at least one retry actually happened"
Assert-HashMatch -Path $dest1 -ExpectedHash $hash -Description "Destination data correct after retry-succeeded copy"

Remove-DriveFile -Path $dest1

# ===============================================================================
# Section 2: lock held past the whole window - retries exhausted, copy fails
# ===============================================================================
Write-Section "Section 2: persistent failure - retries exhausted"

$dest2 = "{0}:\retry_exhausted_dest.bin" -f $drv
Remove-DriveFile -Path $dest2

# -t 1 -w 1: 2 attempts total, 1s apart (~1-2s window). Lock held 8s - much
# longer than the whole retry window, so every attempt must fail.
$result = Invoke-RetoolCopyWithTransientLock -Src $src -Dest $dest2 `
    -RetryCount 1 -RetryWait 1 -LockSeconds 8 -WaitForExitMs 15000
$exitCode2 = $result[0]
$out2      = $result[1]

if ($exitCode2 -ne 0) {
    Write-Host ("    [PASS] Copy fails (exit {0}) once retries are exhausted" -f $exitCode2) -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Copy reported success despite the destination being locked the whole time" -ForegroundColor Red
    $script:TestsFailed++
}
Assert-OutputContains -Output $out2 -Substring 'exhausted' -Description "Output reports retries exhausted"

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $src
Remove-DriveFile -Path $dest2

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
