#Requires -RunAsAdministrator
<#
.SYNOPSIS
    27_inspect_inputs_and_sharing.ps1 - Test: retool inspect directory/glob inputs
    and genuine cross-file block-sharing analysis.

.DESCRIPTION
    Every existing inspect test passes an explicit file path (or two). None pass
    a directory or a glob pattern, and 22_inspect_multi.ps1 deliberately uses two
    UN-deduplicated copies (0 shared blocks) - meaning the actual sharing-matrix
    calculation (merging LcnIntervalIndex overlaps into Shared Blocks / Saved
    Space / Dedup Savings %) has never been verified against files that
    genuinely share physical blocks.

    Sections:
    1. Directory input: retool inspect <dir> recursively enumerates nested files.
    2. Glob pattern input: matches only the intended files; a zero-match glob is
       rejected with "No files matched the specified path(s)."
    3. Genuine block sharing: clone file1 -> file2 via retool copy (same-volume,
       FSCTL_DUPLICATE_EXTENTS_TO_FILE), then inspect both together and verify
       Shared Blocks / Saved Space / Dedup Savings are actually non-zero.
    4. Default summary (no -e): the 5 summary fields appear, the extent table
       headers do not.
    5. Sparse file: the LCN column renders the literal "SPARSE" for a hole.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: inspect - directory/glob inputs and genuine block sharing" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''
$cs  = 65536

# ===============================================================================
# Section 1: directory input
# ===============================================================================
Write-Section "Section 1: directory input"

$dir = "{0}:\inspect_dir_test" -f $drv
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $dir | Out-Null
New-Item -ItemType Directory -Path ("{0}\sub" -f $dir) | Out-Null
Copy-Item $env:RETOOL_TEST_FILE ("{0}\top.bin" -f $dir) -Force
Copy-Item $env:RETOOL_TEST_FILE ("{0}\sub\nested.bin" -f $dir) -Force

$out = Invoke-Retool -Args @('inspect', $dir)
& $env:RETOOL_EXE inspect $dir | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Directory input exits 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Files Analyzed' -Description "Multi-file sharing summary present (recursed into subdirectory)"
if ($outStr -match 'Files Analyzed\s*(\d+)') {
    if ([int]$matches[1] -eq 2) {
        Write-Host "    [PASS] Files Analyzed = 2 (top-level + nested file both found)" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host ("    [FAIL] Files Analyzed = {0}, expected 2" -f $matches[1]) -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Files Analyzed count" -ForegroundColor Red
    $script:TestsFailed++
}

Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue

# ===============================================================================
# Section 2: glob pattern input
# ===============================================================================
Write-Section "Section 2: glob pattern input"

$globA = "{0}:\glob_a.bin" -f $drv
$globB = "{0}:\glob_b.bin" -f $drv
$other = "{0}:\other_nonmatching.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $globA -Force
Copy-Item $env:RETOOL_TEST_FILE $globB -Force
Copy-Item $env:RETOOL_TEST_FILE $other -Force

$globPattern = "{0}:\glob_*.bin" -f $drv
$out = Invoke-Retool -Args @('inspect', $globPattern)
& $env:RETOOL_EXE inspect $globPattern | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Glob pattern input exits 0"
$outStr = $out -join "`n"
if ($outStr -match 'Files Analyzed\s*(\d+)') {
    if ([int]$matches[1] -eq 2) {
        Write-Host "    [PASS] Files Analyzed = 2 (glob matched only glob_a/glob_b, not other_nonmatching)" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host ("    [FAIL] Files Analyzed = {0}, expected 2" -f $matches[1]) -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Files Analyzed count" -ForegroundColor Red
    $script:TestsFailed++
}

# Zero-match glob
$noMatchPattern = "{0}:\nomatch_*.bin" -f $drv
$out = Invoke-Retool -Args @('inspect', $noMatchPattern) -AllowFailure
Assert-ExitCode -Expected 2 -Actual $LASTEXITCODE -Description "Zero-match glob exits 2"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'No files matched' -Description "Reports no files matched"

Remove-DriveFile -Path $globA
Remove-DriveFile -Path $globB
Remove-DriveFile -Path $other

# ===============================================================================
# Section 3: genuine block sharing
# ===============================================================================
Write-Section "Section 3: genuine block sharing (clone via retool copy, then inspect)"

$file1 = "{0}:\sharing_1.bin" -f $drv
$file2 = "{0}:\sharing_2.bin" -f $drv
Remove-DriveFile -Path $file1
Remove-DriveFile -Path $file2
Copy-Item $env:RETOOL_TEST_FILE $file1 -Force

Invoke-Retool -Args @('copy', $file1, $file2) | Out-Null
& $env:RETOOL_EXE copy $file1 $file2 | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Clone copy (file1 -> file2) exits 0"
Assert-HashMatch -Path $file2 -ExpectedHash $env:RETOOL_TEST_HASH -Description "file2 is a correct clone of file1"

$out = Invoke-Retool -Args @('inspect', $file1, $file2, '-e')
& $env:RETOOL_EXE inspect $file1 $file2 -e | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "inspect of genuinely-shared files exits 0"
$outStr = $out -join "`n"

if ($outStr -match 'Shared Blocks\s*[\d.]+ (\w+)?\s*\((\d+) bytes\)') {
    $sharedBytes = [int64]$matches[2]
    if ($sharedBytes -gt 0) {
        Write-Host ("    [PASS] Shared Blocks is non-zero ({0} bytes)" -f $sharedBytes) -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Shared Blocks is zero despite file2 being a clone of file1" -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Shared Blocks value" -ForegroundColor Red
    $script:TestsFailed++
}

if ($outStr -match 'Saved Space\s*[\d.]+ (\w+)?\s*\((\d+) bytes\)') {
    $savedBytes = [int64]$matches[2]
    if ($savedBytes -gt 0) {
        Write-Host ("    [PASS] Saved Space is non-zero ({0} bytes)" -f $savedBytes) -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Saved Space is zero despite file2 being a clone of file1" -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Saved Space value" -ForegroundColor Red
    $script:TestsFailed++
}

if ($outStr -match 'Dedup Savings\s*([\d.]+)\s*%') {
    $savingsPct = [double]$matches[1]
    if ($savingsPct -gt 0) {
        Write-Host ("    [PASS] Dedup Savings is non-zero ({0}%)" -f $savingsPct) -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Dedup Savings is 0% despite file2 being a clone of file1" -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Dedup Savings percentage" -ForegroundColor Red
    $script:TestsFailed++
}

Assert-OutputContains -Output $outStr -Substring 'F0' -Description "Sharing matrix present"

Remove-DriveFile -Path $file1
Remove-DriveFile -Path $file2

# ===============================================================================
# Section 4: default summary (no -e)
# ===============================================================================
Write-Section "Section 4: default summary output (no -e)"

$file4 = "{0}:\default_summary.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $file4 -Force

$out = Invoke-Retool -Args @('inspect', $file4)
& $env:RETOOL_EXE inspect $file4 | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Default (no -e) inspect exits 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'File Size' -Description "Summary field (File Size) present"
Assert-OutputContains -Output $outStr -Substring 'Fragments' -Description "Summary field (Fragments) present"
if ($outStr -notmatch 'VCN') {
    Write-Host "    [PASS] Extent table (VCN column) absent without -e" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Extent table appeared despite -e not being passed" -ForegroundColor Red
    $script:TestsFailed++
}

Remove-DriveFile -Path $file4

# ===============================================================================
# Section 5: sparse file SPARSE marker
# ===============================================================================
Write-Section "Section 5: sparse file SPARSE marker in extent table"

$sparseFile = "{0}:\inspect_sparse.bin" -f $drv
Remove-DriveFile -Path $sparseFile
New-Item -ItemType File -Path $sparseFile -Force | Out-Null
& fsutil sparse setflag $sparseFile | Out-Null

$rng = [System.Random]::new(1301)
$head = [byte[]]::new(2 * $cs)
$rng.NextBytes($head)
$fs = [System.IO.File]::Open($sparseFile, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
try {
    $fs.Write($head, 0, $head.Length)
    $fs.Seek(6 * $cs, [System.IO.SeekOrigin]::Begin) | Out-Null
    $fs.Write($head, 0, $head.Length)  # reuse same buffer for the tail; content doesn't matter here
} finally {
    $fs.Close()
}

$out = Invoke-Retool -Args @('inspect', $sparseFile, '-e')
& $env:RETOOL_EXE inspect $sparseFile -e | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Sparse file inspect exits 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'SPARSE' -Description "Extent table marks the hole as SPARSE"

Remove-DriveFile -Path $sparseFile

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
