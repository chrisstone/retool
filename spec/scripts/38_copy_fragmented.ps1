#Requires -RunAsAdministrator
<#
.SYNOPSIS
    38_copy_fragmented.ps1 - Regression test: retool copy of a genuinely fragmented
    (multi-extent) file.

.DESCRIPTION
    Every other copy test in this suite (30-37) copies either the fixed 50 MB test
    file (one contiguous extent - it's written in a single sequential pass) or small
    resident files. Neither exercises a file whose clusters span MULTIPLE extents,
    which is exactly the shape of interval that exposed the dedup offset bug fixed
    in inspect.cpp/dedup.cpp (BlockEntry::file_offset being read as if valid for any
    LCN in an interval, when it's only correct at the interval's start_lcn). Copy's
    same-volume/cross-volume clone strategies walk extents directly (not through the
    interval index), but a fragmented source is still the highest-risk shape for any
    latent offset-math bug in that code path, so it's worth covering explicitly.

    Two files are grown by interleaving writes between them (write a chunk to A,
    then a chunk to B, repeatedly) - a standard technique to force the volume
    allocator to interleave their physical extents, since each file's next chunk is
    allocated while the other file's is still "live" and growing.

    Sections:
    1. Sanity-check the fixture is actually fragmented (retool inspect -r reports
       more than 1 extent) - if this doesn't hold, the rest of the test is testing
       nothing, so it's a hard assertion, not a soft one.
    2. Same-volume copy (Drive A -> Drive A) of the fragmented file.
    3. Cross-volume copy (Drive A -> Drive B, both ReFS 64K) of the fragmented file.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy - fragmented (multi-extent) file" -ForegroundColor White

$drvA = $env:RETOOL_DRIVE_A -replace ':',''
$drvB = $env:RETOOL_DRIVE_B -replace ':',''

<#
.SYNOPSIS
    Writes two files by interleaving chunk writes between them, to force the
    volume allocator to interleave (and thus fragment) their physical extents.
    Returns @($hashA, $hashB).
#>
function New-FragmentedFilePair {
    param(
        [string] $PathA,
        [string] $PathB,
        [int]    $ChunkSizeBytes = 131072,  # 2 clusters at 64K
        [int]    $ChunkCount     = 24        # 3 MB per file
    )
    $rngA = [System.Random]::new(501)
    $rngB = [System.Random]::new(502)
    $fsA = [System.IO.File]::Open($PathA, [System.IO.FileMode]::Create)
    $fsB = [System.IO.File]::Open($PathB, [System.IO.FileMode]::Create)
    try {
        $bufA = [byte[]]::new($ChunkSizeBytes)
        $bufB = [byte[]]::new($ChunkSizeBytes)
        for ($i = 0; $i -lt $ChunkCount; $i++) {
            $rngA.NextBytes($bufA)
            $fsA.Write($bufA, 0, $bufA.Length)
            $fsA.Flush()
            $rngB.NextBytes($bufB)
            $fsB.Write($bufB, 0, $bufB.Length)
            $fsB.Flush()
        }
    } finally {
        $fsA.Close()
        $fsB.Close()
    }
    return @(
        (Get-FileHash $PathA -Algorithm SHA256).Hash,
        (Get-FileHash $PathB -Algorithm SHA256).Hash
    )
}

# ===============================================================================
# Section 1: Fixture sanity check - confirm real fragmentation was achieved
# ===============================================================================
Write-Section "Section 1: Create and verify a genuinely fragmented fixture"

$fragSrc = "{0}:\frag_src.bin" -f $drvA
$fragOther = "{0}:\frag_other.bin" -f $drvA
Remove-DriveFile -Path $fragSrc
Remove-DriveFile -Path $fragOther
$hashes = New-FragmentedFilePair -PathA $fragSrc -PathB $fragOther
$fragHash = $hashes[0]
Assert-FileExists -Path $fragSrc -Description "Fragmented fixture created"

$out = Invoke-Retool -Args @('inspect', $fragSrc, '-r')
$outStr = $out -join "`n"
if ($outStr -match 'Fragments\s*(\d+)') {
    $fragCount = [int]$matches[1]
    if ($fragCount -gt 1) {
        Write-Host ("    [PASS] Fixture is genuinely fragmented ({0} extents)" -f $fragCount) -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host ("    [FAIL] Fixture has only {0} extent - interleaved-write technique did not fragment it on this volume" -f $fragCount) -ForegroundColor Red
        $script:TestsFailed++
    }
} else {
    Write-Host "    [FAIL] Could not parse Fragments count from inspect output" -ForegroundColor Red
    $script:TestsFailed++
}

# ===============================================================================
# Section 2: Same-volume copy of the fragmented file
# ===============================================================================
Write-Section "Section 2: Same-volume copy (Drive A -> Drive A)"

$dest2 = "{0}:\frag_same_dest.bin" -f $drvA
Remove-DriveFile -Path $dest2

$out = Invoke-Retool -Args @('copy', $fragSrc, $dest2)
& $env:RETOOL_EXE copy $fragSrc $dest2 | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Same-volume copy exits 0"
Assert-FileExists -Path $dest2 -Description "Destination file exists"
Assert-HashMatch -Path $dest2 -ExpectedHash $fragHash -Description "Destination data matches fragmented source"
Assert-HashMatch -Path $fragSrc -ExpectedHash $fragHash -Description "Source unchanged"

Remove-DriveFile -Path $dest2

# ===============================================================================
# Section 3: Cross-volume copy of the fragmented file
# ===============================================================================
Write-Section "Section 3: Cross-volume copy (Drive A -> Drive B)"

$dest3 = "{0}:\frag_xvol_dest.bin" -f $drvB
Remove-DriveFile -Path $dest3

$out = Invoke-Retool -Args @('copy', $fragSrc, $dest3)
& $env:RETOOL_EXE copy $fragSrc $dest3 | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Cross-volume copy exits 0"
Assert-FileExists -Path $dest3 -Description "Destination file exists on Drive B"
Assert-HashMatch -Path $dest3 -ExpectedHash $fragHash -Description "Destination data matches fragmented source"
Assert-HashMatch -Path $fragSrc -ExpectedHash $fragHash -Description "Source unchanged"

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $fragSrc
Remove-DriveFile -Path $fragOther
Remove-DriveFile -Path $dest3

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
