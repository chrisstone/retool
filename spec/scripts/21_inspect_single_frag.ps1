#Requires -RunAsAdministrator
<#
.SYNOPSIS
    21_inspect_single_frag.ps1 — Test: retool inspect <file> -r (fragmentation report)

.DESCRIPTION
    Copies the test file to Drive A, runs 'retool inspect <file> -r', and verifies:
    - Exit code 0
    - Standard extent table present
    - Fragmentation Report section present
    - Fragment count, Frag Score, Smallest/Largest/Avg Extent fields present
    - Source file hash unchanged
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: inspect — single file with fragmentation report (-r)" -ForegroundColor White

# ── Arrange ───────────────────────────────────────────────────────────────────
Write-Section "Arrange"
$dest = Copy-TestFileToDrive -DriveLetter ($env:RETOOL_DRIVE_A -replace ':','') -DestName 'inspect_frag_test.bin'
Assert-FileExists -Path $dest -Description "Test file copied to Drive A"

# ── Act ───────────────────────────────────────────────────────────────────────
Write-Section "Run: retool inspect <file> -r"
$out = Invoke-Retool -Args @('inspect', $dest, '-r')
& $env:RETOOL_EXE inspect $dest -r | Out-Null
$exitCode = $LASTEXITCODE

# ── Assert ────────────────────────────────────────────────────────────────────
Write-Section "Assertions"
Assert-ExitCode -Expected 0 -Actual $exitCode -Description "Exit code 0"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'VCN'                   -Description "Extent table present"
Assert-OutputContains -Output $outStr -Substring 'Fragmentation Report'  -Description "Fragmentation Report section present"
Assert-OutputContains -Output $outStr -Substring 'Fragments'             -Description "Fragments field present"
Assert-OutputContains -Output $outStr -Substring 'Frag Score'            -Description "Frag Score field present"
Assert-OutputContains -Output $outStr -Substring 'Smallest Extent'       -Description "Smallest Extent field present"
Assert-OutputContains -Output $outStr -Substring 'Largest Extent'        -Description "Largest Extent field present"
Assert-OutputContains -Output $outStr -Substring 'Avg Extent'            -Description "Avg Extent field present"

# Source file integrity
Assert-HashMatch -Path $env:RETOOL_TEST_FILE -ExpectedHash $env:RETOOL_TEST_HASH -Description "Source test file unchanged"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-DriveFile -Path $dest

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
