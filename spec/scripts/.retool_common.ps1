#
# .retool_common.ps1 - Shared helpers for all retool test scripts.
# Dot-source this file at the top of every 1X_ test script.
#

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# -- Load environment ----------------------------------------------------------
$envFile = Join-Path $PSScriptRoot '.retool_env.ps1'
if (-not (Test-Path $envFile)) {
    Write-Error "Environment file not found: $envFile`nPlease run 00_setup.ps1 first."
    exit 1
}
. $envFile

# -- Global test state ---------------------------------------------------------
$script:TestsPassed = 0
$script:TestsFailed = 0

# -- Helpers -------------------------------------------------------------------

<#
.SYNOPSIS
    Run retool with the given arguments and return stdout as a string.
    Throws if retool exits with a non-zero code (unless -AllowFailure is set).
#>
function Invoke-Retool {
    param(
        [Alias('Args')]
        [string[]] $ToolArgs,
        [switch]   $AllowFailure
    )
    Write-Host "    > retool $($ToolArgs -join ' ')" -ForegroundColor DarkGray
    $oldEncoding = [Console]::OutputEncoding
    # retool's top-level fatal errors (bad CLI args, operational failures - see
    # main.cpp) go to stderr; CliOutput itself writes everything else (including
    # WARNING/ERROR-labeled messages) to stdout. Under the script-wide
    # $ErrorActionPreference = 'Stop', merging stderr via 2>&1 wraps each stderr
    # line as a terminating NativeCommandError *at this call site* - before
    # $AllowFailure below ever gets a chance to matter. Relax it just for this
    # call so an expected failure's stderr text becomes ordinary output instead
    # of an uncatchable termination.
    $oldEap = $ErrorActionPreference
    try {
        [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
        $ErrorActionPreference = 'Continue'
        $output = & $env:RETOOL_EXE @ToolArgs 2>&1
    } finally {
        $ErrorActionPreference = $oldEap
        [Console]::OutputEncoding = $oldEncoding
    }
    if ($LASTEXITCODE -ne 0 -and -not $AllowFailure) {
        Write-Error "retool exited with code $LASTEXITCODE`nOutput: $output"
    }
    return $output
}

<#
.SYNOPSIS
    Assert that a file exists at the given path.
#>
function Assert-FileExists {
    param([string] $Path, [string] $Description = '')
    $label = if ($Description) { $Description } else { $Path }
    if (Test-Path $Path) {
        Write-Host "    [PASS] File exists: $label" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] File not found: $label" -ForegroundColor Red
        $script:TestsFailed++
    }
}

<#
.SYNOPSIS
    Assert that two SHA-256 hashes match.
#>
function Assert-HashMatch {
    param([string] $Path, [string] $ExpectedHash, [string] $Description = '')
    $label = if ($Description) { $Description } else { $Path }
    if (-not (Test-Path $Path)) {
        Write-Host "    [FAIL] Cannot hash (file missing): $label" -ForegroundColor Red
        $script:TestsFailed++
        return
    }
    $actual = (Get-FileHash $Path -Algorithm SHA256).Hash
    if ($actual -eq $ExpectedHash) {
        Write-Host "    [PASS] Hash correct: $label" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Hash mismatch: $label" -ForegroundColor Red
        Write-Host "           Expected: $ExpectedHash" -ForegroundColor Red
        Write-Host "           Actual:   $actual" -ForegroundColor Red
        $script:TestsFailed++
    }
}

<#
.SYNOPSIS
    Assert that $Value is not null/empty.
#>
function Assert-NotEmpty {
    param([object] $Value, [string] $Description)
    if ($Value) {
        Write-Host "    [PASS] $Description" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] $Description (was null/empty)" -ForegroundColor Red
        $script:TestsFailed++
    }
}

<#
.SYNOPSIS
    Assert that $Output contains the given substring.
#>
function Assert-OutputContains {
    param([string] $Output, [string] $Substring, [string] $Description = '')
    $label = if ($Description) { $Description } else { "Output contains '$Substring'" }
    if ($Output -match [regex]::Escape($Substring)) {
        Write-Host "    [PASS] $label" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] $label" -ForegroundColor Red
        $script:TestsFailed++
    }
}

<#
.SYNOPSIS
    Assert that retool exits with the given exit code.
#>
function Assert-ExitCode {
    param([int] $Expected, [int] $Actual, [string] $Description = '')
    $label = if ($Description) { $Description } else { "Exit code = $Expected" }
    if ($Actual -eq $Expected) {
        Write-Host "    [PASS] $label" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] $label (got $Actual)" -ForegroundColor Red
        $script:TestsFailed++
    }
}

<#
.SYNOPSIS
    Print a section banner.
#>
function Write-Section {
    param([string] $Title)
    Write-Host ""
    Write-Host "-- $Title --" -ForegroundColor Cyan
}

<#
.SYNOPSIS
    Print the final pass/fail summary and exit with appropriate code.
#>
function Exit-TestSummary {
    param([string] $ScriptName)
    Write-Host ""
    Write-Host "=======================================" -ForegroundColor White
    if ($script:TestsFailed -eq 0) {
        Write-Host "  RESULT: PASSED ($($script:TestsPassed) checks)" -ForegroundColor Green
    } else {
        Write-Host "  RESULT: FAILED ($($script:TestsFailed) failed / $($script:TestsPassed + $script:TestsFailed) total)" -ForegroundColor Red
    }
    Write-Host "  Script: $ScriptName" -ForegroundColor White
    Write-Host "=======================================" -ForegroundColor White
    exit $(if ($script:TestsFailed -eq 0) { 0 } else { 1 })
}

<#
.SYNOPSIS
    Copy the test file to a drive and return the destination path.
#>
function Copy-TestFileToDrive {
    param([string] $DriveLetter, [string] $DestName = 'testfile.bin')
    $dest = "${DriveLetter}:\$DestName"
    Copy-Item $env:RETOOL_TEST_FILE $dest -Force
    return $dest
}

<#
.SYNOPSIS
    Remove a file from a drive (if it exists), silently.
#>
function Remove-DriveFile {
    param([string] $Path)
    if (Test-Path $Path) { Remove-Item $Path -Force }
}
