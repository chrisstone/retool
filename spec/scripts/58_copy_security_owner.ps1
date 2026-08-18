#Requires -RunAsAdministrator
<#
.SYNOPSIS
    58_copy_security_owner.ps1 - Test: retool copy -c:S (ACL preservation) and
    copy's own volume-specifier / multi-source rejections.

.DESCRIPTION
    52_copy_components.ps1 verifies D, A, and T (via -ca:R). S (security ACL,
    via GetNamedSecurityInfoW/SetNamedSecurityInfoW at copy.cpp:1545/1560) is
    untested. Owner (O) copying is deliberately NOT covered here - see
    doc/notes.md for why (SID/ownership manipulation is meaningfully more
    fragile to verify than an ACL entry for comparatively little extra coverage).

    Also covers copy's OWN explicit rejections (copy.cpp:1278-1298), distinct
    from the generic "volume specifier must be first argument" parser rule
    already covered by 60_cli_validation.ps1:
    - copy rejects ANY volume specifier as source or destination, even as the
      sole/first argument (where the generic parser rule alone would allow it).
    - copy rejects more than 2 path specifiers ("multi-source not yet supported").

    Sections:
    1. -c:DATS (S included): a distinctive, non-inherited ACL entry on the
       source carries over to the destination.
    2. -c:DAT (S excluded): that same ACL entry does NOT carry over.
    3. copy does not accept a volume specifier, even as the sole source arg.
    4. copy rejects more than 2 path specifiers.
#>

. (Join-Path $PSScriptRoot '.retool_common.ps1')

Write-Host "TEST: copy - security ACL (-c:S) and copy-specific rejections" -ForegroundColor White

$drv = $env:RETOOL_DRIVE_A -replace ':',''
$markerIdentity = 'Everyone'
$markerRight    = [System.Security.AccessControl.FileSystemRights]::TakeOwnership

function Test-HasMarkerAce {
    param([string] $Path)
    $acl = Get-Acl -Path $Path
    foreach ($ace in $acl.Access) {
        if (-not $ace.IsInherited -and
            $ace.IdentityReference.Value -match [regex]::Escape($markerIdentity) -and
            ($ace.FileSystemRights -band $markerRight) -eq $markerRight -and
            $ace.AccessControlType -eq [System.Security.AccessControl.AccessControlType]::Allow) {
            return $true
        }
    }
    return $false
}

function New-SourceFile {
    param([string] $Path, [int] $Seed)
    $rng = [System.Random]::new($Seed)
    $buf = [byte[]]::new(65536)
    $rng.NextBytes($buf)
    [System.IO.File]::WriteAllBytes($Path, $buf)
    return (Get-FileHash $Path -Algorithm SHA256).Hash
}

# ===============================================================================
# Section 1: -c:DATS (S included) - ACL carries over
# ===============================================================================
Write-Section "Section 1: -c:DATS (S included)"

$src1  = "{0}:\sec_s_src.bin" -f $drv
$dest1 = "{0}:\sec_s_dest.bin" -f $drv
Remove-DriveFile -Path $src1
Remove-DriveFile -Path $dest1
$hash1 = New-SourceFile -Path $src1 -Seed 1501

$acl1 = Get-Acl -Path $src1
$rule1 = New-Object System.Security.AccessControl.FileSystemAccessRule(
    $markerIdentity, $markerRight, 'Allow')
$acl1.AddAccessRule($rule1)
Set-Acl -Path $src1 -AclObject $acl1

if (Test-HasMarkerAce -Path $src1) {
    Write-Host "    [PASS] Marker ACE applied to source" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Failed to apply marker ACE to source - -c:S precondition not met" -ForegroundColor Red
    $script:TestsFailed++
}

$out = Invoke-Retool -Args @('copy', $src1, $dest1, '-c:DATS')
& $env:RETOOL_EXE copy $src1 $dest1 '-c:DATS' | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "-c:DATS copy exits 0"
Assert-HashMatch -Path $dest1 -ExpectedHash $hash1 -Description "-c:DATS data correct"

if (Test-HasMarkerAce -Path $dest1) {
    Write-Host "    [PASS] Marker ACE carried over to destination (S included)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Marker ACE did NOT carry over despite S being included" -ForegroundColor Red
    $script:TestsFailed++
}

Remove-DriveFile -Path $src1
Remove-DriveFile -Path $dest1

# ===============================================================================
# Section 2: -c:DAT (S excluded) - ACL does not carry over
# ===============================================================================
Write-Section "Section 2: -c:DAT (S excluded)"

$src2  = "{0}:\sec_nos_src.bin" -f $drv
$dest2 = "{0}:\sec_nos_dest.bin" -f $drv
Remove-DriveFile -Path $src2
Remove-DriveFile -Path $dest2
$hash2 = New-SourceFile -Path $src2 -Seed 1502

$acl2 = Get-Acl -Path $src2
$rule2 = New-Object System.Security.AccessControl.FileSystemAccessRule(
    $markerIdentity, $markerRight, 'Allow')
$acl2.AddAccessRule($rule2)
Set-Acl -Path $src2 -AclObject $acl2

$out = Invoke-Retool -Args @('copy', $src2, $dest2, '-c:DAT')
& $env:RETOOL_EXE copy $src2 $dest2 '-c:DAT' | Out-Null
Assert-ExitCode -Expected 0 -Actual $LASTEXITCODE -Description "-c:DAT copy exits 0"
Assert-HashMatch -Path $dest2 -ExpectedHash $hash2 -Description "-c:DAT data correct"

if (-not (Test-HasMarkerAce -Path $dest2)) {
    Write-Host "    [PASS] Marker ACE did NOT carry over (S excluded)" -ForegroundColor Green
    $script:TestsPassed++
} else {
    Write-Host "    [FAIL] Marker ACE carried over despite S being excluded from -c:DAT" -ForegroundColor Red
    $script:TestsFailed++
}

Remove-DriveFile -Path $src2
Remove-DriveFile -Path $dest2

# ===============================================================================
# Section 3: copy rejects a volume specifier, even as the sole source arg
# ===============================================================================
Write-Section "Section 3: copy rejects volume specifiers (its own check, not the generic parser rule)"

$dest3 = "{0}:\vol_reject_dest.bin" -f $drv
$out = Invoke-Retool -Args @('copy', $env:RETOOL_DRIVE_A, $dest3) -AllowFailure
Assert-ExitCode -Expected 2 -Actual $LASTEXITCODE -Description "copy <volume> <dest> fails (exit 2)"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'copy does not accept volume specifiers' -Description "Reports copy's own volume-specifier rejection"

# ===============================================================================
# Section 4: copy rejects more than 2 path specifiers
# ===============================================================================
Write-Section "Section 4: copy rejects multi-source (>2 specifiers)"

$out = Invoke-Retool -Args @('copy', 'a.bin', 'b.bin', 'c.bin') -AllowFailure
Assert-ExitCode -Expected 2 -Actual $LASTEXITCODE -Description "copy with 3 path specifiers fails (exit 2)"
$outStr = $out -join "`n"
Assert-OutputContains -Output $outStr -Substring 'Multiple source files are not yet supported' -Description "Reports the multi-source rejection"

Exit-TestSummary -ScriptName $MyInvocation.MyCommand.Name
