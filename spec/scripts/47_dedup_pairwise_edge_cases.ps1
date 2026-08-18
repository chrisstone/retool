#Requires -RunAsAdministrator
<#
.SYNOPSIS
    47_dedup_pairwise_edge_cases.ps1 - Test: dedup pairwise disjoint/partial-overlap
    files, cross-volume pairwise, volume-wide mixed files, and invalid arg counts.

.DESCRIPTION
    42_dedup_pairwise.ps1 only tests fully-duplicate pairwise files. This closes
    several distinct gaps:
    1. Pairwise on completely disjoint files (0 shared clusters) - the
       rebuild_file() "no dedup candidates" skip path (dedup.cpp:439-443) is
       otherwise never exercised for pairwise mode.
    2. Pairwise on files with PARTIAL cluster overlap (fileRef and fileOp share
       one cluster, each also has one unique cluster) - 46_dedup_partial_overlap.ps1
       only covers this in volume-wide mode.
    3. Cross-volume pairwise dedup - verified actual behavior in dedup.cpp:
       there's no explicit same-volume check; prepare() resolves the scan volume
       from fileRef only, so a fileOp on a different volume is silently absent
       from that scan and PairwiseDedupStrategy::build_plans fails with
       "Origin file not found in scan: " - not a same-volume-specific message.
    4. Volume-wide dedup on a mix of duplicate and genuinely unique files -
       verifies unique files are left untouched while duplicates are deduped.
    5. Invalid dedup argument counts (0 args, 3 args) are rejected.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: dedup - pairwise edge cases, cross-volume, mixed files, arg counts" -ForegroundColor White

$drvA = $env:RETOOL_DRIVE_A -replace ':',''
$drvB = $env:RETOOL_DRIVE_B -replace ':',''
$drvC = $env:RETOOL_DRIVE_C -replace ':',''
$csA  = 65536
$csC  = 4096

function New-ContentBlock {
    param([int] $Seed, [int] $Size)
    $rng = [System.Random]::new($Seed)
    $buf = [byte[]]::new($Size)
    $rng.NextBytes($buf)
    return $buf
}
function Write-ClusterFile {
    param([string] $Path, [byte[][]] $Blocks)
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create)
    try { foreach ($b in $Blocks) { $fs.Write($b, 0, $b.Length) } } finally { $fs.Close() }
}
function Get-ExpectedHash {
    param([byte[][]] $Blocks)
    $total = 0; foreach ($b in $Blocks) { $total += $b.Length }
    $buf = [byte[]]::new($total)
    $off = 0
    foreach ($b in $Blocks) { [System.Array]::Copy($b, 0, $buf, $off, $b.Length); $off += $b.Length }
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    return [System.BitConverter]::ToString($sha256.ComputeHash($buf)) -replace '-',''
}

# ===============================================================================
# Section 1: pairwise on completely disjoint files
# ===============================================================================
Write-Section "Section 1: pairwise dedup on completely disjoint files"

$refDisjoint = "{0}:\pw_disjoint_ref.bin" -f $drvA
$opDisjoint  = "{0}:\pw_disjoint_op.bin" -f $drvA
Remove-DriveFile -Path $refDisjoint
Remove-DriveFile -Path $opDisjoint
$blockRef = New-ContentBlock -Seed 1401 -Size (2 * $csA)
$blockOp  = New-ContentBlock -Seed 1402 -Size (2 * $csA)
[System.IO.File]::WriteAllBytes($refDisjoint, $blockRef)
[System.IO.File]::WriteAllBytes($opDisjoint, $blockOp)
$hashRefDisjoint = (Get-FileHash $refDisjoint -Algorithm SHA256).Hash
$hashOpDisjoint  = (Get-FileHash $opDisjoint -Algorithm SHA256).Hash

$out = Invoke-Retool -Args @('dedup', $refDisjoint, $opDisjoint)
& $env:RETOOL_EXE dedup $refDisjoint $opDisjoint | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Disjoint pairwise dedup exits 0"
$outStr = $out -join "`n"
if ($outStr -match 'Clusters Deduped\s*(\d+)') {
    if ([int]$matches[1] -eq 0) {
        Write-Host "    [PASS] Clusters Deduped = 0 for disjoint files" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host ("    [FAIL] Clusters Deduped = {0}, expected 0" -f $matches[1]) -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Clusters Deduped" -ForegroundColor Red
    $script:TestsFailed++
}
Assert-HashMatch -Path $refDisjoint -ExpectedHash $hashRefDisjoint -Description "fileRef unchanged"
Assert-HashMatch -Path $opDisjoint -ExpectedHash $hashOpDisjoint -Description "fileOp unchanged (nothing to dedup)"

Remove-DriveFile -Path $refDisjoint
Remove-DriveFile -Path $opDisjoint

# ===============================================================================
# Section 2: pairwise with partial cluster overlap
# ===============================================================================
Write-Section "Section 2: pairwise dedup with partial cluster overlap"

$c1 = New-ContentBlock -Seed 1403 -Size $csA
$c2 = New-ContentBlock -Seed 1404 -Size $csA
$c3 = New-ContentBlock -Seed 1405 -Size $csA

$refPartial = "{0}:\pw_partial_ref.bin" -f $drvA   # [c1, c2]
$opPartial  = "{0}:\pw_partial_op.bin" -f $drvA    # [c1, c3]
Remove-DriveFile -Path $refPartial
Remove-DriveFile -Path $opPartial
Write-ClusterFile -Path $refPartial -Blocks @($c1, $c2)
Write-ClusterFile -Path $opPartial -Blocks @($c1, $c3)
$hashRefPartial = Get-ExpectedHash -Blocks @($c1, $c2)
$hashOpPartial  = Get-ExpectedHash -Blocks @($c1, $c3)

$out = Invoke-Retool -Args @('dedup', $refPartial, $opPartial)
& $env:RETOOL_EXE dedup $refPartial $opPartial | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Partial-overlap pairwise dedup exits 0"
$outStr = $out -join "`n"
if ($outStr -match 'Clusters Deduped\s*(\d+)') {
    if ([int]$matches[1] -eq 1) {
        Write-Host "    [PASS] Clusters Deduped = 1 (only the shared c1 cluster)" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host ("    [FAIL] Clusters Deduped = {0}, expected 1" -f $matches[1]) -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Clusters Deduped" -ForegroundColor Red
    $script:TestsFailed++
}
Assert-HashMatch -Path $refPartial -ExpectedHash $hashRefPartial -Description "fileRef unchanged"
Assert-HashMatch -Path $opPartial -ExpectedHash $hashOpPartial -Description "fileOp still reads as [c1,c3] (not corrupted)"

Remove-DriveFile -Path $refPartial
Remove-DriveFile -Path $opPartial

# ===============================================================================
# Section 3: cross-volume pairwise dedup
# ===============================================================================
Write-Section "Section 3: cross-volume pairwise dedup (rejected)"

$refXvol = "{0}:\pw_xvol_ref.bin" -f $drvA
$opXvol  = "{0}:\pw_xvol_op.bin" -f $drvB
Copy-Item $env:RETOOL_TEST_FILE $refXvol -Force
Copy-Item $env:RETOOL_TEST_FILE $opXvol -Force

$out = Invoke-Retool -Args @('dedup', $refXvol, $opXvol) -AllowFailure
Assert-ExitCode -Expected 2 -Actual $LASTEXITCODE -Description "Cross-volume pairwise dedup fails (exit 2)"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Origin file not found in scan' -Description "Reports the actual (non-same-volume-specific) failure"
Assert-HashMatch -Path $refXvol -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileRef unchanged"
Assert-HashMatch -Path $opXvol -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileOp unchanged"

Remove-DriveFile -Path $refXvol
Remove-DriveFile -Path $opXvol

# ===============================================================================
# Section 4: volume-wide dedup with mixed unique and duplicate files
# ===============================================================================
Write-Section "Section 4: volume-wide dedup with mixed unique/duplicate files (Drive C)"

$dupA   = "{0}:\mixed_dup_a.bin" -f $drvC
$dupB   = "{0}:\mixed_dup_b.bin" -f $drvC
$unique = "{0}:\mixed_unique.bin" -f $drvC
foreach ($f in @($dupA, $dupB, $unique)) { Remove-DriveFile -Path $f }

$dupContent    = New-ContentBlock -Seed 1406 -Size (4 * $csC)
$uniqueContent = New-ContentBlock -Seed 1407 -Size (4 * $csC)
[System.IO.File]::WriteAllBytes($dupA, $dupContent)
[System.IO.File]::WriteAllBytes($dupB, $dupContent)
[System.IO.File]::WriteAllBytes($unique, $uniqueContent)
$hashDup    = (Get-FileHash $dupA -Algorithm SHA256).Hash
$hashUnique = (Get-FileHash $unique -Algorithm SHA256).Hash

$out = Invoke-Retool -Args @('dedup', $env:RETOOL_DRIVE_C)
& $env:RETOOL_EXE dedup $env:RETOOL_DRIVE_C | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Mixed-file volume-wide dedup exits 0"

Assert-HashMatch -Path $dupA -ExpectedHash $hashDup -Description "dupA data correct after dedup"
Assert-HashMatch -Path $dupB -ExpectedHash $hashDup -Description "dupB data correct after dedup"
Assert-HashMatch -Path $unique -ExpectedHash $hashUnique -Description "unique.bin untouched (no matching content anywhere)"

foreach ($f in @($dupA, $dupB, $unique)) { Remove-DriveFile -Path $f }

# ===============================================================================
# Section 5: invalid argument counts
# ===============================================================================
Write-Section "Section 5: invalid dedup argument counts"

# Both are rejected by dedup::prepare() (not util::parse_arguments()), so
# they're command-level failures (exit 2), not CLI-syntax failures (exit 1).
$out = Invoke-Retool -Args @('dedup') -AllowFailure
Assert-ExitCode -Expected 2 -Actual $LASTEXITCODE -Description "dedup with 0 args exits 2"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Usage: retool dedup' -Description "Reports usage error"

$out = Invoke-Retool -Args @('dedup', 'a', 'b', 'c') -AllowFailure
Assert-ExitCode -Expected 2 -Actual $LASTEXITCODE -Description "dedup with 3 args exits 2"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Usage: retool dedup' -Description "Reports usage error"

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
