#Requires -RunAsAdministrator
<#
.SYNOPSIS
    28_inspect_strict_and_errors.ps1 - Test: retool inspect -s (strict mode) and
    best-effort handling of missing files.

.DESCRIPTION
    No test in this suite passes -s to inspect. Verified in inspect.cpp:
    - Best-effort multi-file (default): a missing/failing file is recorded as an
      error and the rest still get processed; execute() unconditionally
      `return 0` for multi-file mode (inspect.cpp:1255) regardless of any
      accumulated errors - only -s makes a failure fatal.
    - -s, path resolution failure (file doesn't exist / can't determine its
      volume): "Strict Mode: Failed to resolve target '...'" (inspect.cpp:1186),
      exit code 2, aborts before inspecting anything else.
    - -s, single target: same abort-with-exit-2 behavior applies even with only
      one file specified.

    Sections:
    1. Best-effort multi-file: one missing file among two - the existing file is
       still inspected and reported, exit code 0.
    2. Strict multi-file: aborts immediately, the existing file is never reached.
    3. Strict single file: aborts with exit code 2.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: inspect - strict mode (-s) and best-effort error handling" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''
$existing = "{0}:\strict_existing.bin" -f $drv
$missing  = "{0}:\strict_missing_{1}.bin" -f $drv, (Get-Random)
Remove-DriveFile -Path $existing
Copy-Item $env:RETOOL_TEST_FILE $existing -Force

# ===============================================================================
# Section 1: best-effort multi-file (default)
# ===============================================================================
Write-Section "Section 1: best-effort multi-file (default, no -s)"

$out = Invoke-Retool -Args @('inspect', $missing, $existing)
& $env:RETOOL_EXE inspect $missing $existing | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Best-effort run with one missing file still exits 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Files Analyzed' -Description "Existing file was still analyzed"

# ===============================================================================
# Section 2: strict multi-file
# ===============================================================================
Write-Section "Section 2: strict multi-file (-s)"

$out = Invoke-Retool -Args @('inspect', $missing, $existing, '-s') -AllowFailure
Assert-ExitCode -Expected 2 -Actual $LASTEXITCODE -Description "Strict mode aborts with exit code 2"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Strict Mode' -Description "Reports Strict Mode abort"
if ($outStr -notmatch 'Files Analyzed') {
    Write-Host "    [PASS] Aborted before ever analyzing the existing file" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Strict mode still produced a Files Analyzed summary (should have aborted first)" -ForegroundColor Red
    $script:TestsFailed++
}

# ===============================================================================
# Section 3: strict single file
# ===============================================================================
Write-Section "Section 3: strict single file (-s)"

$out = Invoke-Retool -Args @('inspect', $missing, '-s') -AllowFailure
Assert-ExitCode -Expected 2 -Actual $LASTEXITCODE -Description "Strict mode on a single missing target exits 2"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Strict Mode' -Description "Reports Strict Mode abort"

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $existing

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
