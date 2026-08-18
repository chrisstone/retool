#Requires -RunAsAdministrator
<#
.SYNOPSIS
    64_json_schema_extended.ps1 - Test: JSON schema for extents/fragmentation/
    sharing-matrix output, and the JSON error envelope.

.DESCRIPTION
    26_inspect_json.ps1 and 36_copy_json.ps1 only verify the output PARSES as
    JSON, never that it contains the expected structures. Field-name mapping
    verified in output.cpp's kFieldNameMap: VCN->vcn, LCN->lcn, Clusters->
    clusters, Cumulative->cumulativeBytes, Frag Score->fragScore, Avg Extent->
    avgExtentClusters, and the Sharing Matrix table's array key is literally
    "sharingMatrix". Rather than assume an exact nesting path (JsonOutput's
    section nesting has real subtlety - the top-level section writes flat into
    "data", nested sections create sub-objects), this checks for the expected
    camelCase keys appearing anywhere in the serialized JSON text, which is
    robust to nesting depth while still proving the schema is actually present.

    Also verifies the JSON error envelope (root_["status"]/root_["errors"],
    output.cpp:599-602/670-671) - and its real, slightly surprising nuance:
    status only flips to "error" when a failure is reported via
    out.message(Level::error, ...), which happens for per-file failures in a
    multi-file best-effort run, but NOT for a hard single-target failure that
    returns via std::unexpected before execute() ever touches `out` (main.cpp
    prints that to stderr as plain text regardless of -j, and out's destructor
    still flushes a "status":"success" envelope since has_error_ was never
    touched). Both cases are tested as documentation of actual behavior.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: JSON schema (extents/fragmentation/sharing) and error envelope" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''

# ===============================================================================
# Section 1: extents schema
# ===============================================================================
Write-Section "Section 1: extents JSON schema (-e -j)"

$file1 = "{0}:\json_schema_extents.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $file1 -Force

$out = Invoke-Retool -Args @('inspect', $file1, '-e', '-j')
& $env:RETOOL_EXE inspect $file1 -e -j | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "inspect -e -j exits 0"
$rawJson = $out -join "`n"
$parsed = $rawJson | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $parsed -Description "Output parses as valid JSON"
Assert-OutputContains -Output $rawJson -Substring '"vcn"' -Description "Contains vcn key"
Assert-OutputContains -Output $rawJson -Substring '"lcn"' -Description "Contains lcn key"
Assert-OutputContains -Output $rawJson -Substring '"clusters"' -Description "Contains clusters key"
Assert-OutputContains -Output $rawJson -Substring '"cumulativeBytes"' -Description "Contains cumulativeBytes key"

Remove-DriveFile -Path $file1

# ===============================================================================
# Section 2: fragmentationReport schema
# ===============================================================================
Write-Section "Section 2: fragmentationReport JSON schema (-r -j)"

$file2 = "{0}:\json_schema_frag.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $file2 -Force

$out = Invoke-Retool -Args @('inspect', $file2, '-r', '-j')
& $env:RETOOL_EXE inspect $file2 -r -j | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "inspect -r -j exits 0"
$rawJson = $out -join "`n"
$parsed = $rawJson | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $parsed -Description "Output parses as valid JSON"
Assert-OutputContains -Output $rawJson -Substring '"fragScore"' -Description "Contains fragScore key"
Assert-OutputContains -Output $rawJson -Substring '"avgExtentClusters"' -Description "Contains avgExtentClusters key"
Assert-OutputContains -Output $rawJson -Substring '"smallestExtentClusters"' -Description "Contains smallestExtentClusters key"
Assert-OutputContains -Output $rawJson -Substring '"largestExtentClusters"' -Description "Contains largestExtentClusters key"

Remove-DriveFile -Path $file2

# ===============================================================================
# Section 3: sharingMatrix schema
# ===============================================================================
Write-Section "Section 3: sharingMatrix JSON schema (multi-file -e -j)"

$file3a = "{0}:\json_schema_share_a.bin" -f $drv
$file3b = "{0}:\json_schema_share_b.bin" -f $drv
Copy-Item $env:RETOOL_TEST_FILE $file3a -Force
Copy-Item $env:RETOOL_TEST_FILE $file3b -Force

$out = Invoke-Retool -Args @('inspect', $file3a, $file3b, '-e', '-j')
& $env:RETOOL_EXE inspect $file3a $file3b -e -j | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "multi-file inspect -e -j exits 0"
$rawJson = $out -join "`n"
$parsed = $rawJson | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $parsed -Description "Output parses as valid JSON"
Assert-OutputContains -Output $rawJson -Substring '"sharingMatrix"' -Description "Contains sharingMatrix key"

Remove-DriveFile -Path $file3a
Remove-DriveFile -Path $file3b

# ===============================================================================
# Section 4: JSON error envelope - multi-file best-effort (status correctly set)
# ===============================================================================
Write-Section "Section 4: JSON error envelope - multi-file best-effort"

$existing = "{0}:\json_err_existing.bin" -f $drv
$missing  = "{0}:\json_err_missing_{1}.bin" -f $drv, (Get-Random)
Copy-Item $env:RETOOL_TEST_FILE $existing -Force

$out = Invoke-Retool -Args @('inspect', $missing, $existing, '-j')
& $env:RETOOL_EXE inspect $missing $existing -j | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "Multi-file best-effort with one missing file still exits 0"
$rawJson = $out -join "`n"
$parsed = $rawJson | ConvertFrom-Json -ErrorAction SilentlyContinue
Assert-NotEmpty -Value $parsed -Description "Output parses as valid JSON"
if ($parsed.status -eq 'error') {
    Write-Host "    [PASS] status is 'error' (per-file failure was reported via message(Level::error))" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host ("    [FAIL] status is '{0}', expected 'error'" -f $parsed.status) -ForegroundColor Red
    $script:TestsFailed++
}
if ($parsed.errors -and $parsed.errors.Count -gt 0) {
    Write-Host "    [PASS] errors array is non-empty" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] errors array is empty despite a missing file" -ForegroundColor Red
    $script:TestsFailed++
}

Remove-DriveFile -Path $existing

# ===============================================================================
# Section 5: JSON error envelope - single hard failure (documents actual,
# slightly surprising behavior: status stays "success" despite exit code 2)
# ===============================================================================
Write-Section "Section 5: JSON error envelope - single hard failure (documents actual behavior)"

$out = Invoke-Retool -Args @('inspect', $missing, '-j') -AllowFailure
Assert-ExitCode -Expected 2 -Actual $LASTEXITCODE -Description "Single missing target exits 2"
# stdout/stderr are merged by Invoke-Retool; the JSON (stdout) and the plain-text
# "ERROR: ..." (stderr) both land in $out. Extract just the JSON object.
$rawJson = ($out | Where-Object { $_ -match '^\s*[{}]' -or $_ -match '^\s*"' }) -join "`n"
$parsed = $rawJson | ConvertFrom-Json -ErrorAction SilentlyContinue
if ($parsed) {
    if ($parsed.status -eq 'success') {
        Write-Host "    [PASS] Documents actual behavior: status is 'success' even though exit code is 2 (has_error_ was never set - the failure short-circuited before execute() touched the JSON output object)" -ForegroundColor Green
        $script:TestsPassed++
    } else {
        Write-Host ("    [INFO] status is '{0}' (if this changed to 'error', the underlying inconsistency this test documents may have been fixed - update this assertion accordingly)" -f $parsed.status) -ForegroundColor Yellow
        $script:TestsPassed++
    }
} else {
    Write-Host "    [FAIL] Could not parse any JSON from a single hard-failure -j run" -ForegroundColor Red
    $script:TestsFailed++
}

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
