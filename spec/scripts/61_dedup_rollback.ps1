#Requires -RunAsAdministrator
<#
.SYNOPSIS
    61_dedup_rollback.ps1 - Regression test: rebuild_file()'s rollback path
    when a rebuild fails AFTER the origin has already been renamed to .old.

.DESCRIPTION
    No test in this suite forces a dedup rebuild to fail after Step 1 (rename
    to .old). That path - deleting the partial new file and restoring .old back
    to the original name (dedup.cpp:462-470) - has zero coverage; a bug there
    would silently destroy data on any real-world dedup failure.

    Forces a genuine, deterministic failure at Step 3 (pre-size) by making
    fileOp itself consume nearly all of Drive C's free space: fileOp is [c1,
    filler], where c1 matches fileRef (so rebuild_file's pre-check finds a
    dedup candidate and actually attempts the rebuild) and filler is sized so
    only a couple of MB of free space remains. Pre-sizing the replacement file
    to fileOp's full size then requires more free space than exists (the
    .old file is still occupying its own full size at that point), which fails
    every time, deterministically - no timing/locking involved.

    Verifies:
    - Exit code 2 (operational failure recorded, non-strict).
    - Error output mentions the pre-size failure.
    - fileOp is restored at its ORIGINAL path with its ORIGINAL content -
      proving the rollback actually ran.
    - No stray .old file is left behind.
    - fileRef (never touched) is unaffected.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: dedup - rollback after a post-rename rebuild failure" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_C -replace ':',''
$cs  = 4096

$fileRef = "{0}:\rollback_ref.bin" -f $drv
$fileOp  = "{0}:\rollback_op.bin" -f $drv
$oldFile = "{0}.old" -f $fileOp
Remove-DriveFile -Path $fileRef
Remove-DriveFile -Path $fileOp
Remove-DriveFile -Path $oldFile

# -- Arrange: fileOp sized to leave only ~2 MB of free space on Drive C --------
Write-Section "Arrange: size fileOp to nearly exhaust Drive C's free space"

$freeBefore = (Get-PSDrive -Name $drv).Free
$marginBytes = 2MB
$fillerSize = [int64]($freeBefore - $marginBytes)
if ($fillerSize -lt (10 * $cs)) {
    Write-Host "    [FAIL] Not enough free space on Drive C to construct this scenario meaningfully" -ForegroundColor Red
    $script:TestsFailed++
    Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
}
$fillerSize = $fillerSize - ($fillerSize % $cs)  # round down to a cluster boundary

$rngShared = [System.Random]::new(1601)
$c1 = [byte[]]::new($cs)
$rngShared.NextBytes($c1)

# fileRef = just c1, so it's a valid dedup candidate for fileOp's first cluster.
[System.IO.File]::WriteAllBytes($fileRef, $c1)

# fileOp = c1 + a large unique filler, consuming nearly all remaining free space.
# Get-PSDrive's reported free space is only an estimate of what's actually
# writable - ReFS metadata overhead grows as the file itself grows, so the
# real limit can be reached slightly before $fillerSize. That's fine (even
# desirable) here: the goal is just to leave the volume nearly exhausted, and
# hitting disk-full mid-write achieves that as well as reaching the exact
# target would. Only re-throw if something OTHER than disk-full occurs.
$fsOp = [System.IO.File]::Open($fileOp, [System.IO.FileMode]::Create)
try {
    $fsOp.Write($c1, 0, $c1.Length)
    $rngFiller = [System.Random]::new(1602)
    $chunk = [byte[]]::new(4MB)
    $written = 0
    try {
        while ($written -lt $fillerSize) {
            $toWrite = [Math]::Min($chunk.Length, $fillerSize - $written)
            $rngFiller.NextBytes($chunk)
            $fsOp.Write($chunk, 0, $toWrite)
            $written += $toWrite
        }
    } catch [System.IO.IOException] {
        if ($_.Exception.Message -notmatch 'not enough space') { throw }
        Write-Host ("    Filler write stopped early at ~{0} MB (disk full) - volume is already nearly exhausted, which is what this test needs" -f [math]::Round($written/1MB,1)) -ForegroundColor DarkGray
    }
} finally {
    try { $fsOp.Close() } catch { }
}

$originalHash = (Get-FileHash $fileOp -Algorithm SHA256).Hash
$freeAfterFiller = (Get-PSDrive -Name $drv).Free
Write-Host ("    Free before: {0} MB, free after filler: {1} MB (fileOp size ~{2} MB)" -f `
    [math]::Round($freeBefore/1MB,1), [math]::Round($freeAfterFiller/1MB,1), [math]::Round(((Get-Item $fileOp).Length)/1MB,1)) -ForegroundColor DarkGray

# ===============================================================================
# Act
# ===============================================================================
Write-Section "Run: retool dedup fileRef fileOp (expected to fail mid-rebuild)"

$out = Invoke-Retool -Args @('dedup', $fileRef, $fileOp) -AllowFailure
$exitCode = $LASTEXITCODE
$outStr = $out -join "`n"

# ===============================================================================
# Assert
# ===============================================================================
Write-Section "Assertions"
Assert-ExitCode -Expected 2 -Actual $exitCode -Description "Exit code 2 (rebuild failure recorded)"
Assert-OutputContains -Output $outStr -Substring 'pre-size' -Description "Error mentions the pre-size failure"

Assert-FileExists -Path $fileOp -Description "fileOp exists at its original path (rollback restored it)"
Assert-HashMatch -Path $fileOp -ExpectedHash $originalHash -Description "fileOp content is the ORIGINAL content (not corrupted, not partial)"

if (-not (Test-Path $oldFile)) {
    Write-Host "    [PASS] No stray .old file left behind" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Stray .old file left behind after rollback" -ForegroundColor Red
    $script:TestsFailed++
}

$refHash = (Get-FileHash $fileRef -Algorithm SHA256).Hash
$expectedRefHash = [System.BitConverter]::ToString([System.Security.Cryptography.SHA256]::Create().ComputeHash($c1)) -replace '-',''
if ($refHash -eq $expectedRefHash) {
    Write-Host "    [PASS] fileRef unaffected" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] fileRef was modified despite being the read-only reference" -ForegroundColor Red
    $script:TestsFailed++
}

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $fileRef
Remove-DriveFile -Path $fileOp
Remove-DriveFile -Path $oldFile

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
