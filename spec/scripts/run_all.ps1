#Requires -RunAsAdministrator
<#
.SYNOPSIS
    run_all.ps1 - Runs all retool spec scripts in numeric order.

.DESCRIPTION
    Discovers every NN_*.ps1 script in this directory (00_setup.ps1 through
    99_teardown.ps1), sorts them numerically, and runs them in order:
      1. 00_setup.ps1        - aborts the run immediately if it fails.
      2. 10_.. through 44_.. - the actual test scripts.
      3. 99_teardown.ps1     - always runs unless -SkipTeardown is passed,
                                even if a test script failed.

    Each script calls `exit` on completion (via Exit-TestSummary), so each
    one is launched in its own child PowerShell process rather than
    dot-sourced/called in-process - otherwise its exit would terminate this
    runner too. Dot files (.retool_common.ps1, .retool_env.ps1) are shared
    helpers, not scripts, and are skipped automatically.

.PARAMETER SkipTeardown
    Skip 99_teardown.ps1 at the end, leaving the VHDXs/test file/env file in
    place for post-mortem inspection.

.PARAMETER StopOnFailure
    Stop running further test scripts as soon as one fails (teardown still
    runs afterward unless -SkipTeardown is also passed).

.EXAMPLE
    .\run_all.ps1

.EXAMPLE
    .\run_all.ps1 -StopOnFailure -SkipTeardown
#>

param(
    [switch] $SkipTeardown,
    [switch] $StopOnFailure
)

Set-StrictMode -Version Latest

$scriptDir  = $PSScriptRoot
$allScripts = Get-ChildItem -Path $scriptDir -Filter '*.ps1' |
    Where-Object { $_.Name -match '^\d+_.*\.ps1$' } |
    Sort-Object { [int](($_.Name -split '_')[0]) }

$setupScript    = $allScripts | Where-Object { $_.Name -eq '00_setup.ps1' }
$teardownScript = $allScripts | Where-Object { $_.Name -eq '99_teardown.ps1' }
$testScripts    = $allScripts | Where-Object { $_.Name -ne '00_setup.ps1' -and $_.Name -ne '99_teardown.ps1' }

$results = @()

function Invoke-SpecScript {
    param([System.IO.FileInfo] $Script)
    Write-Host ""
    Write-Host "=======================================================" -ForegroundColor White
    Write-Host ("  RUNNING: {0}" -f $Script.Name) -ForegroundColor White
    Write-Host "=======================================================" -ForegroundColor White
    # Piping to Out-Host (rather than letting it stream "bare") keeps the
    # child script's console output visible while stopping it from being
    # captured into this function's own return value - without this, `$code =
    # Invoke-SpecScript ...` at the call site captures EVERY line the child
    # printed as part of $code (turning it into a string array), silently
    # breaking both `$code -ne 0` (always true for a non-empty array,
    # regardless of the real exit code) and any `-f` formatting of $code.
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Script.FullName | Out-Host
    return $LASTEXITCODE
}

# -- 00_setup.ps1 (always first, aborts run on failure) ------------------------
if ($setupScript) {
    $code = Invoke-SpecScript -Script $setupScript
    $results += [pscustomobject]@{ Script = $setupScript.Name; ExitCode = $code }
    if ($code -ne 0) {
        Write-Host ""
        Write-Error ("00_setup.ps1 failed (exit {0}) - aborting run." -f $code)
        exit 1
    }
}

# -- Test scripts (10_.. through 44_..) -----------------------------------------
foreach ($script in $testScripts) {
    $code = Invoke-SpecScript -Script $script
    $results += [pscustomobject]@{ Script = $script.Name; ExitCode = $code }
    if ($code -ne 0 -and $StopOnFailure) {
        Write-Host ""
        Write-Warning ("{0} failed (exit {1}) - stopping (StopOnFailure)." -f $script.Name, $code)
        break
    }
}

# -- 99_teardown.ps1 (always last, unless skipped) ------------------------------
if ($teardownScript -and -not $SkipTeardown) {
    $code = Invoke-SpecScript -Script $teardownScript
    $results += [pscustomobject]@{ Script = $teardownScript.Name; ExitCode = $code }
}

# -- Summary ---------------------------------------------------------------------
Write-Host ""
Write-Host "=======================================================" -ForegroundColor White
Write-Host "  SUMMARY" -ForegroundColor White
Write-Host "=======================================================" -ForegroundColor White
$failed = 0
foreach ($r in $results) {
    if ($r.ExitCode -eq 0) {
        Write-Host ("  [PASS] {0}" -f $r.Script) -ForegroundColor Green
    } else {
        Write-Host ("  [FAIL] {0} (exit {1})" -f $r.Script, $r.ExitCode) -ForegroundColor Red
        $failed++
    }
}
Write-Host "=======================================================" -ForegroundColor White
if ($failed -eq 0) {
    Write-Host ("  ALL SCRIPTS PASSED ({0} total)" -f $results.Count) -ForegroundColor Green
} else {
    Write-Host ("  {0} SCRIPT(S) FAILED / {1} TOTAL" -f $failed, $results.Count) -ForegroundColor Red
}
Write-Host "=======================================================" -ForegroundColor White

exit $(if ($failed -eq 0) { 0 } else { 1 })
