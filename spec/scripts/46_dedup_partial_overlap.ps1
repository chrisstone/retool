#Requires -RunAsAdministrator
<#
.SYNOPSIS
    46_dedup_partial_overlap.ps1 - Regression test: retool dedup of multiple small
    files with PARTIAL cluster overlap (not full-file duplicates).

.DESCRIPTION
    Every other dedup test in this suite (40-44) uses fully-duplicate whole-file
    pairs (fileA and fileB are byte-identical copies of the same source). None of
    them test the more realistic "some clusters shared, some clusters unique per
    file" case, where dedup must merge at cluster granularity rather than treating
    each file as all-shared-or-all-unique.

    Uses Drive C (4K cluster, smallest of the three test volumes) with distinct
    4 KB content blocks c1..c6, each a deterministically-seeded random buffer.

    Section 1 (the requested scenario): fileA = [c1, c2], fileB = [c1, c3].
    c1 is shared; c2 and c3 are each unique to their file and different from each
    other. Verifies dedup finds the one shared cluster without cross-contaminating
    the two unique ones - i.e. fileA must not pick up any of c3, and fileB must
    not pick up any of c2.

    Section 2 (offset-position stress): fileC = [c4, c5], fileD = [c5, c6].
    c5 is shared, but sits at fileC's SECOND cluster (offset = cluster_size) while
    it sits at fileD's FIRST cluster (offset 0). This is the exact shape that
    would trigger the interval-offset bug fixed in inspect.cpp/dedup.cpp: when
    rebuilding fileD, dedup looks up the matching cluster's offset within fileC's
    interval - fileC's interval's stored BlockEntry::file_offset is only valid at
    the interval's start_lcn (c4's position). The bug would have used that stale
    offset unconditionally, wrongly cloning fileC's UNIQUE c4 into fileD instead
    of the actually-matching c5 - a silent, deterministic corruption this section
    would directly catch.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: dedup - partial cluster overlap across multiple small files" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_C -replace ':',''
$cs  = 4096

function New-ContentBlock {
    param([int] $Seed)
    $rng = [System.Random]::new($Seed)
    $buf = [byte[]]::new($cs)
    $rng.NextBytes($buf)
    return $buf
}

function Write-ClusterFile {
    param([string] $Path, [byte[][]] $Blocks)
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create)
    try {
        foreach ($b in $Blocks) { $fs.Write($b, 0, $b.Length) }
    } finally {
        $fs.Close()
    }
}

function Get-ExpectedHash {
    param([byte[][]] $Blocks)
    $total = [byte[]]::new($cs * $Blocks.Count)
    for ($i = 0; $i -lt $Blocks.Count; $i++) {
        [System.Array]::Copy($Blocks[$i], 0, $total, $i * $cs, $cs)
    }
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    return [System.BitConverter]::ToString($sha256.ComputeHash($total)) -replace '-',''
}

# ===============================================================================
# Arrange
# ===============================================================================
Write-Section "Arrange: distinct 4K content blocks and small multi-cluster files"

$c1 = New-ContentBlock -Seed 801
$c2 = New-ContentBlock -Seed 802
$c3 = New-ContentBlock -Seed 803
$c4 = New-ContentBlock -Seed 804
$c5 = New-ContentBlock -Seed 805
$c6 = New-ContentBlock -Seed 806

$fileA = "{0}:\overlap_a.bin" -f $drv   # [c1, c2]
$fileB = "{0}:\overlap_b.bin" -f $drv   # [c1, c3]
$fileC = "{0}:\overlap_c.bin" -f $drv   # [c4, c5]
$fileD = "{0}:\overlap_d.bin" -f $drv   # [c5, c6]

foreach ($f in @($fileA, $fileB, $fileC, $fileD)) { Remove-DriveFile -Path $f }

Write-ClusterFile -Path $fileA -Blocks @($c1, $c2)
Write-ClusterFile -Path $fileB -Blocks @($c1, $c3)
Write-ClusterFile -Path $fileC -Blocks @($c4, $c5)
Write-ClusterFile -Path $fileD -Blocks @($c5, $c6)

$hashA = Get-ExpectedHash -Blocks @($c1, $c2)
$hashB = Get-ExpectedHash -Blocks @($c1, $c3)
$hashC = Get-ExpectedHash -Blocks @($c4, $c5)
$hashD = Get-ExpectedHash -Blocks @($c5, $c6)

Assert-FileExists -Path $fileA -Description "fileA [c1,c2] created"
Assert-FileExists -Path $fileB -Description "fileB [c1,c3] created"
Assert-FileExists -Path $fileC -Description "fileC [c4,c5] created"
Assert-FileExists -Path $fileD -Description "fileD [c5,c6] created"
Assert-HashMatch -Path $fileA -ExpectedHash $hashA -Description "fileA content correct before dedup"
Assert-HashMatch -Path $fileB -ExpectedHash $hashB -Description "fileB content correct before dedup"
Assert-HashMatch -Path $fileC -ExpectedHash $hashC -Description "fileC content correct before dedup"
Assert-HashMatch -Path $fileD -ExpectedHash $hashD -Description "fileD content correct before dedup"

# ===============================================================================
# Act: volume-wide dedup
# ===============================================================================
Write-Section ("Run: retool dedup {0}" -f $env:RETOOL_DRIVE_C)

$out = Invoke-Retool -Args @('dedup', $env:RETOOL_DRIVE_C)
& $env:RETOOL_EXE dedup $env:RETOOL_DRIVE_C | Out-Null
$exitCode = $LASTEXITCODE
$outStr = $out -join "`n"

# ===============================================================================
# Assert
# ===============================================================================
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
Assert-OutputContains -Output $outStr -Substring 'Clusters Deduped' -Description "Clusters Deduped field present"

Write-Section "Section 1: partial overlap ([c1,c2] / [c1,c3]) - no cross-contamination"
Assert-HashMatch -Path $fileA -ExpectedHash $hashA -Description "fileA still reads as [c1,c2] (did not pick up c3)"
Assert-HashMatch -Path $fileB -ExpectedHash $hashB -Description "fileB still reads as [c1,c3] (did not pick up c2)"

Write-Section "Section 2: shared cluster at differing offsets ([c4,c5] / [c5,c6])"
Assert-HashMatch -Path $fileC -ExpectedHash $hashC -Description "fileC still reads as [c4,c5]"
Assert-HashMatch -Path $fileD -ExpectedHash $hashD -Description "fileD still reads as [c5,c6] (not [c4,c6] - would indicate the interval-offset bug)"

# -- Cleanup -------------------------------------------------------------------
foreach ($f in @($fileA, $fileB, $fileC, $fileD)) { Remove-DriveFile -Path $f }

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
