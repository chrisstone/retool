#Requires -RunAsAdministrator
<#
.SYNOPSIS
    62_dedup_pairwise_roles.ps1 - Regression test: pairwise dedup's fileRef role
    is truly untouched at the physical (extent) level, not just content-equal.

.DESCRIPTION
    42_dedup_pairwise.ps1 and 47_dedup_pairwise_edge_cases.ps1 both verify
    fileRef's SHA-256 hash is unchanged after a pairwise dedup - but a hash match
    only proves the content is byte-identical, not that fileRef was literally
    never touched. A hypothetical bug that renamed/rebuilt fileRef too (instead
    of only fileOp) could still pass a pure hash check as long as the rebuild
    happened to reproduce identical content.

    This test captures fileRef's full extent table (VCN/LCN/Clusters/Bytes) via
    retool inspect -e BEFORE and AFTER a pairwise dedup and asserts the text is
    IDENTICAL - proving fileRef's physical layout (not just its content) never
    changed, i.e. it was genuinely never renamed or rewritten.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: dedup - pairwise fileRef role is physically untouched" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''
$fileRef = "{0}:\roles_ref.bin" -f $drv
$fileOp  = "{0}:\roles_op.bin" -f $drv
Remove-DriveFile -Path $fileRef
Remove-DriveFile -Path $fileOp
Copy-Item $env:RETOOL_TEST_FILE $fileRef -Force
Copy-Item $env:RETOOL_TEST_FILE $fileOp -Force

Write-Section "Capture fileRef's extent table before dedup"
$before = Invoke-Retool -Args @('inspect', $fileRef, '-e')
$beforeStr = ($before -join "`n").Trim()
Assert-OutputContains -Output $beforeStr -Substring 'VCN' -Description "Captured a real extent table before dedup"

Write-Section "Run: retool dedup fileRef fileOp"
$out = Invoke-Retool -Args @('dedup', $fileRef, $fileOp)
& $env:RETOOL_EXE dedup $fileRef $fileOp | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Pairwise dedup exits 0"

Write-Section "Capture fileRef's extent table after dedup"
$after = Invoke-Retool -Args @('inspect', $fileRef, '-e')
$afterStr = ($after -join "`n").Trim()

Write-Section "Assertions"
Assert-HashMatch -Path $fileRef -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileRef content unchanged"
Assert-HashMatch -Path $fileOp -ExpectedHash $env:RETOOL_TEST_HASH -Description "fileOp content correct after rebuild"

if ($beforeStr -eq $afterStr) {
    Write-Host "    [PASS] fileRef's extent table (VCN/LCN/Clusters) is byte-for-byte identical before/after - never physically touched" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] fileRef's extent table CHANGED - it was rebuilt/relocated despite being the read-only reference file" -ForegroundColor Red
    $script:TestsFailed++
}

# -- Cleanup -------------------------------------------------------------------
Remove-DriveFile -Path $fileRef
Remove-DriveFile -Path $fileOp

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
