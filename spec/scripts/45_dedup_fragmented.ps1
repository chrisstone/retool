#Requires -RunAsAdministrator
<#
.SYNOPSIS
    45_dedup_fragmented.ps1 - Regression test: retool dedup of genuinely fragmented
    (multi-extent) files with identical content.

.DESCRIPTION
    Every dedup test in this suite (40-44) dedups the fixed 50 MB test file, which
    is written in one sequential pass and lands as a single contiguous extent -
    meaning every LCN interval dedup.cpp looks up during source selection has
    exactly one cluster in it, at the interval's start_lcn. That's precisely the
    shape that hid the interval-offset bug fixed in inspect.cpp/dedup.cpp
    (BlockEntry::file_offset is only valid AT an interval's start_lcn; reading it
    for any other LCN in a multi-cluster interval silently returns the wrong
    offset). This test forces genuine multi-extent files into dedup specifically
    to close that gap, rather than relying on 41/42's contiguous fixtures.

    Two files (fragA, fragB) are grown with byte-identical content from the same
    seeded RNG sequence, but their writes are interleaved with a third "spacer"
    file (deleted afterward) so the volume allocator can't place either one
    contiguously - forcing each into multiple extents.

    Verifies:
    - The fixture is genuinely fragmented (sanity check, hard assertion).
    - fragA and fragB start byte-identical.
    - Volume-wide dedup exits 0, reports non-zero clusters deduped.
    - Both files read back byte-identical to the original content after dedup
      (this is the check that would have caught the interval-offset bug).
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: dedup - fragmented (multi-extent) files" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''
$cs  = 65536

<#
.SYNOPSIS
    Grows fragA and fragB with byte-identical content (same seed sequence, written
    to two different files) while interleaving writes to a third spacer file, so
    the allocator can't place any of the three contiguously. Returns the SHA-256
    hash the two files should share.
#>
function New-FragmentedDuplicatePair {
    param(
        [string] $PathA,
        [string] $PathB,
        [string] $SpacerPath,
        [int]    $ChunkSizeBytes = 131072,
        [int]    $ChunkCount     = 24
    )
    $rngShared = [System.Random]::new(701)   # Same seed feeds both A and B identically.
    $rngSpacer = [System.Random]::new(702)

    $fsA = [System.IO.File]::Open($PathA, [System.IO.FileMode]::Create)
    $fsB = [System.IO.File]::Open($PathB, [System.IO.FileMode]::Create)
    $fsS = [System.IO.File]::Open($SpacerPath, [System.IO.FileMode]::Create)
    try {
        $bufShared = [byte[]]::new($ChunkSizeBytes)
        $bufSpacer = [byte[]]::new($ChunkSizeBytes)
        for ($i = 0; $i -lt $ChunkCount; $i++) {
            $rngShared.NextBytes($bufShared)
            $fsA.Write($bufShared, 0, $bufShared.Length); $fsA.Flush()

            $rngSpacer.NextBytes($bufSpacer)
            $fsS.Write($bufSpacer, 0, $bufSpacer.Length); $fsS.Flush()

            # Re-derive the exact same chunk bytes for B via a fresh RNG seeded
            # identically to rngShared's state at this point isn't possible with
            # System.Random directly, so instead write the SAME buffer just used
            # for A - guarantees byte-identical content deterministically.
            $fsB.Write($bufShared, 0, $bufShared.Length); $fsB.Flush()

            $rngSpacer.NextBytes($bufSpacer)
            $fsS.Write($bufSpacer, 0, $bufSpacer.Length); $fsS.Flush()
        }
    } finally {
        $fsA.Close(); $fsB.Close(); $fsS.Close()
    }
}

# ===============================================================================
# Arrange: fragmented, byte-identical fixture
# ===============================================================================
Write-Section "Arrange: fragmented duplicate fixture"

$fragA  = "{0}:\dedup_frag_a.bin" -f $drv
$fragB  = "{0}:\dedup_frag_b.bin" -f $drv
$spacer = "{0}:\dedup_frag_spacer.bin" -f $drv
Remove-DriveFile -Path $fragA
Remove-DriveFile -Path $fragB
Remove-DriveFile -Path $spacer

New-FragmentedDuplicatePair -PathA $fragA -PathB $fragB -SpacerPath $spacer
Remove-DriveFile -Path $spacer   # Free the spacer's space; leaves fragA/fragB fragmented.

Assert-FileExists -Path $fragA -Description "fragA created"
Assert-FileExists -Path $fragB -Description "fragB created"

$hashA = (Get-FileHash $fragA -Algorithm SHA256).Hash
$hashB = (Get-FileHash $fragB -Algorithm SHA256).Hash
if ($hashA -eq $hashB) {
    Write-Host "    [PASS] fragA and fragB start byte-identical" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] fragA and fragB differ before dedup even ran" -ForegroundColor Red
    $script:TestsFailed++
}

$out = Invoke-Retool -Args @('inspect', $fragA, '-r')
$outStr = $out -join "`n"
if ($outStr -match 'Fragments\s*(\d+)') {
    $fragCount = [int]$matches[1]
    if ($fragCount -gt 1) {
        Write-Host ("    [PASS] fragA is genuinely fragmented ({0} extents)" -f $fragCount) -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host ("    [FAIL] fragA has only {0} extent - fixture did not fragment on this volume" -f $fragCount) -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Fragments count from inspect output" -ForegroundColor Red
    $script:TestsFailed++
}

# ===============================================================================
# Act: volume-wide dedup
# ===============================================================================
Write-Section ("Run: retool dedup {0}" -f $env:RETOOL_DRIVE_A)

$out = Invoke-Retool -Args @('dedup', $env:RETOOL_DRIVE_A)
& $env:RETOOL_EXE dedup $env:RETOOL_DRIVE_A | Out-Null
$exitCode = $LASTEXITCODE
$outStr = $out -join "`n"

# ===============================================================================
# Assert
# ===============================================================================
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
Assert-OutputContains -Output $outStr -Substring 'Clusters Deduped' -Description "Clusters Deduped field present"

if ($outStr -match 'Clusters Deduped\s*(\d+)') {
    $clustersDeduped = [int]$matches[1]
    if ($clustersDeduped -gt 0) {
        Write-Host ("    [PASS] Non-zero clusters deduped ({0})" -f $clustersDeduped) -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host "    [FAIL] Zero clusters deduped for fully-duplicate fragmented files" -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Clusters Deduped count" -ForegroundColor Red
    $script:TestsFailed++
}

# The check that would have caught the interval-offset bug: both files must
# still read back as the ORIGINAL content, not a scrambled/zero-filled mix.
Assert-HashMatch -Path $fragA -ExpectedHash $hashA -Description "fragA data correct after dedup (not corrupted by interval-offset bug)"
Assert-HashMatch -Path $fragB -ExpectedHash $hashA -Description "fragB data correct after dedup (not corrupted by interval-offset bug)"

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $fragA
Remove-DriveFile -Path $fragB

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
