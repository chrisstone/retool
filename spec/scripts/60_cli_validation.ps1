#Requires -RunAsAdministrator
<#
.SYNOPSIS
    60_cli_validation.ps1 - Test: CLI argument validation, help/version, and
    deliberate error-rejection paths.

.DESCRIPTION
    None of util.cpp's parse_arguments() rejection branches, main.cpp's
    help/version/unknown-command handling, or the explicit "dedup requires
    ReFS" / "volume is not ReFS" checks are exercised anywhere else in this
    suite. Each check is a quick, independent invocation - no VHDX state is
    mutated - so they're grouped into one script rather than one file each.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: CLI validation, help/version, and error-rejection paths" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''

<#
.SYNOPSIS
    Runs retool with the given args, asserting the exit code and that the
    output contains ExpectedSubstring.
#>
function Assert-RetoolInvocation {
    param(
        [string[]] $ToolArgs,
        [int]      $ExpectedExitCode,
        [string]   $ExpectedSubstring,
        [string]   $Description
    )
    $out = Invoke-Retool -Args $ToolArgs -AllowFailure
    $outStr = $out -join "`n"
    Assert-ExitCode -Expected $ExpectedExitCode -Actual $LASTEXITCODE -Description "$Description (exit code)"
    Assert-OutputContains -Output $outStr -Substring $ExpectedSubstring -Description "$Description (message)"
}

# ===============================================================================
# Section 1: help, version, no command
# ===============================================================================
Write-Section "Section 1: help / version / no command"

Assert-RetoolInvocation -ToolArgs @() -ExpectedExitCode 0 -ExpectedSubstring 'Usage:' -Description "No command prints general help"
Assert-RetoolInvocation -ToolArgs @('help') -ExpectedExitCode 0 -ExpectedSubstring 'Commands:' -Description "'help' prints general help"
Assert-RetoolInvocation -ToolArgs @('help', 'copy') -ExpectedExitCode 0 -ExpectedSubstring 'retool copy <src> <dest>' -Description "'help copy' prints copy-specific help"
Assert-RetoolInvocation -ToolArgs @('help', 'all') -ExpectedExitCode 0 -ExpectedSubstring 'retool dedup <volume>' -Description "'help all' includes every command's help"
Assert-RetoolInvocation -ToolArgs @('version') -ExpectedExitCode 0 -ExpectedSubstring 'retool version' -Description "'version' prints version string"

# ===============================================================================
# Section 2: unknown command / option
# ===============================================================================
Write-Section "Section 2: unknown command / option"

Assert-RetoolInvocation -ToolArgs @('boguscommand') -ExpectedExitCode 1 -ExpectedSubstring "Unknown command" -Description "Unknown command rejected"
Assert-RetoolInvocation -ToolArgs @('volume', $env:RETOOL_DRIVE_A, '--bogus') -ExpectedExitCode 1 -ExpectedSubstring "Unknown option" -Description "Unknown option rejected"

# ===============================================================================
# Section 3: options requiring a missing argument
# ===============================================================================
Write-Section "Section 3: options requiring a missing argument"

Assert-RetoolInvocation -ToolArgs @('copy', 'a', 'b', '-t') -ExpectedExitCode 1 -ExpectedSubstring "-t option requires" -Description "-t with no value rejected"
Assert-RetoolInvocation -ToolArgs @('copy', 'a', 'b', '-w') -ExpectedExitCode 1 -ExpectedSubstring "-w option requires" -Description "-w with no value rejected"
Assert-RetoolInvocation -ToolArgs @('inspect', '-i') -ExpectedExitCode 1 -ExpectedSubstring "-i option requires" -Description "-i with no value rejected"
Assert-RetoolInvocation -ToolArgs @('inspect', 'a', '-o') -ExpectedExitCode 1 -ExpectedSubstring "-o option requires" -Description "-o with no value rejected"

# ===============================================================================
# Section 4: invalid values
# ===============================================================================
Write-Section "Section 4: invalid option values"

Assert-RetoolInvocation -ToolArgs @('copy', 'a', 'b', '-t', '-1') -ExpectedExitCode 1 -ExpectedSubstring "non-negative integer" -Description "-t negative value rejected"
Assert-RetoolInvocation -ToolArgs @('copy', 'a', 'b', '-t', 'abc') -ExpectedExitCode 1 -ExpectedSubstring "non-negative integer" -Description "-t non-numeric value rejected"
Assert-RetoolInvocation -ToolArgs @('copy', 'a', 'b', '-c:X') -ExpectedExitCode 1 -ExpectedSubstring "accepts only D, A, T, S, O" -Description "-c: invalid letter rejected"
Assert-RetoolInvocation -ToolArgs @('copy', 'a', 'b', '-c:') -ExpectedExitCode 1 -ExpectedSubstring "requires at least one component letter" -Description "-c: with no letters rejected"
Assert-RetoolInvocation -ToolArgs @('copy', 'a', 'b', '-ca:X') -ExpectedExitCode 1 -ExpectedSubstring "accepts only R, A, S, H" -Description "-ca: invalid letter rejected"
Assert-RetoolInvocation -ToolArgs @('copy', 'a', 'b', '-ca:') -ExpectedExitCode 1 -ExpectedSubstring "requires at least one attribute letter" -Description "-ca: with no letters rejected"

# ===============================================================================
# Section 5: mutually exclusive flags
# ===============================================================================
Write-Section "Section 5: mutually exclusive flags"

Assert-RetoolInvocation -ToolArgs @('volume', $env:RETOOL_DRIVE_A, '-j', '-q') -ExpectedExitCode 1 -ExpectedSubstring "cannot be used with" -Description "-j and -q together rejected"
Assert-RetoolInvocation -ToolArgs @('volume', $env:RETOOL_DRIVE_A, '-q', '-o', 'C:\Temp\x.txt') -ExpectedExitCode 1 -ExpectedSubstring "cannot be used with" -Description "-q and -o together rejected"

# ===============================================================================
# Section 6: positional argument rules and missing files
# ===============================================================================
Write-Section "Section 6: positional argument rules and missing files"

Assert-RetoolInvocation -ToolArgs @('inspect', 'somefile.bin', $env:RETOOL_DRIVE_A) `
    -ExpectedExitCode 1 -ExpectedSubstring "must be the first argument" -Description "Volume specifier after another positional arg rejected"

# The -f expression must be parenthesized here: unparenthesized, inline
# inside an @() array literal that's itself a command's parameter value,
# PowerShell's operator precedence does not reliably bind -f before the
# array/argument boundary - confirmed empirically (bisected against a
# working pre-computed-variable version) to corrupt $LASTEXITCODE as
# observed by the caller, even though the array's own contents come out
# correct. Every other script in this suite avoids the issue by always
# pre-computing -f expressions into a variable before use; this is the one
# place it was written inline, so it's parenthesized instead.
$missing = "{0}:\this_file_does_not_exist_{1}.bin" -f $drv, (Get-Random)
Assert-RetoolInvocation -ToolArgs @('copy', $missing, ("{0}:\dest.bin" -f $drv)) `
    -ExpectedExitCode 2 -ExpectedSubstring "does not exist" -Description "Copying a nonexistent source file rejected"

# ===============================================================================
# Section 7: ReFS requirement checks
# ===============================================================================
Write-Section "Section 7: ReFS requirement checks"

# dedup explicitly requires ReFS - running it against the NTFS system drive
# should be rejected with a specific error rather than silently doing nothing.
Assert-RetoolInvocation -ToolArgs @('dedup', 'C:') `
    -ExpectedExitCode 2 -ExpectedSubstring "Dedup requires ReFS" -Description "dedup on non-ReFS volume rejected"

# volume's purpose is reporting filesystem info, so it doesn't reject a
# non-ReFS volume outright - it reports the info and warns instead.
$out = Invoke-Retool -Args @('volume', 'C:')
& $env:RETOOL_EXE volume C: | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "volume on non-ReFS volume still succeeds"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'not a ReFS filesystem' -Description "volume on non-ReFS volume warns instead of rejecting"

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
