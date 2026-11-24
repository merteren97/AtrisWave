<#
.SYNOPSIS
    Packages AtrisWaveVAD.sys, AtrisWaveVAD.inf, and AtrisWaveVAD.pdb into a CAB archive
    and signs it with an EV Code Signing certificate for Microsoft Partner Center
    Hardware Dev Center Attestation Signing submission.

.PARAMETER Configuration
    Build configuration (default: Release).

.PARAMETER Platform
    Target architecture: x64 or ARM64 (default: x64).

.PARAMETER CertThumbprint
    SHA-1 thumbprint of the EV Code Signing certificate in Cert:\CurrentUser\My or LocalMachine\My.

.PARAMETER TimestampUrl
    RFC 3161 timestamp server URL (default: http://timestamp.digicert.com).
#>
[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",

    [ValidateSet("x64", "ARM64")]
    [string]$Platform = "x64",

    [Parameter(Mandatory = $true)]
    [string]$CertThumbprint,

    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"

$DriverRoot = Split-Path -Parent $PSScriptRoot
$OutDir = Join-Path $DriverRoot "build\$Platform\$Configuration"
$CabDir = Join-Path $DriverRoot "build\attestation\$Platform"
$CabName = "AtrisWaveVAD-$Platform.cab"
$CabPath = Join-Path $CabDir $CabName
$DdfPath = Join-Path $CabDir "AtrisWaveVAD.ddf"

$SysPath = Join-Path $OutDir "AtrisWaveVAD.sys"
$InfPath = Join-Path $OutDir "AtrisWaveVAD.inf"
$PdbPath = Join-Path $OutDir "AtrisWaveVAD.pdb"

foreach ($required in @($SysPath, $InfPath)) {
    if (-not (Test-Path $required)) {
        throw "Missing build artifact: $required. Run build-and-testsign.ps1 first."
    }
}

New-Item -ItemType Directory -Force -Path $CabDir | Out-Null

$ddfContent = @"
.OPTION EXPLICIT
.Set CabinetNameTemplate=$CabName
.Set DiskDirectoryTemplate=$CabDir
.Set CompressionType=MSZIP
.Set Cabinet=ON
.Set Compress=ON
.Set DestinationDir=AtrisWaveVAD
"$SysPath"
"$InfPath"
"@

if (Test-Path $PdbPath) {
    $ddfContent += "`r`n`"$PdbPath`""
}

Set-Content -Path $DdfPath -Value $ddfContent -Encoding ASCII

Write-Host "[1/2] Creating attestation CAB via makecab.exe..." -ForegroundColor Cyan
& makecab.exe /F $DdfPath
if ($LASTEXITCODE -ne 0) { throw "makecab.exe failed with exit code $LASTEXITCODE" }

Write-Host "[2/2] Signing CAB with EV certificate ($CertThumbprint)..." -ForegroundColor Cyan
$kitRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$signtool = Get-ChildItem -Path $kitRoot -Filter "signtool.exe" -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\x64\\" } |
    Sort-Object FullName -Descending |
    Select-Object -First 1

if (-not $signtool) { throw "signtool.exe not found in Windows Kits." }

& $signtool.FullName sign /fd SHA256 /sha1 $CertThumbprint /tr $TimestampUrl /td SHA256 $CabPath
if ($LASTEXITCODE -ne 0) { throw "EV signtool failed with exit code $LASTEXITCODE" }

Write-Host "Attestation CAB ready for Partner Center upload: $CabPath" -ForegroundColor Green
