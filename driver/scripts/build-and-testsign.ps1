<#
.SYNOPSIS
    Builds AtrisWaveVAD.sys with WDK MSBuild, generates a local test-signing certificate,
    creates/signs the catalog (.cat) and driver (.sys), and optionally installs onto a Test VM.

.PARAMETER Configuration
    Build configuration: Debug or Release (default: Release).

.PARAMETER Platform
    Target architecture: x64 or ARM64 (default: x64).

.PARAMETER Install
    If specified, installs the test certificate into TrustedRoot/TrustedPublisher and
    installs the driver package via pnputil (requires Administrator + testsigning enabled).
#>
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64", "ARM64")]
    [string]$Platform = "x64",

    [switch]$Install
)

$ErrorActionPreference = "Stop"

$DriverRoot = Split-Path -Parent $PSScriptRoot
$ProjectFile = Join-Path $DriverRoot "AtrisWaveVAD.vcxproj"
$OutDir = Join-Path $DriverRoot "build\$Platform\$Configuration"
$SysPath = Join-Path $OutDir "AtrisWaveVAD.sys"
$InfPath = Join-Path $OutDir "AtrisWaveVAD.inf"
$CatPath = Join-Path $OutDir "AtrisWaveVAD.cat"
$CertSubject = "CN=AtrisWave Test Signing Certificate"
$CertStore = "Cert:\LocalMachine\My"

function Find-MsBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($msbuild -and (Test-Path $msbuild)) { return $msbuild }
    }
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "MSBuild.exe not found. Install Visual Studio 2022+ with Desktop C++ and Windows 11 WDK."
}

function Find-WdkTool([string]$ToolName) {
    $kitRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    if (Test-Path $kitRoot) {
        $found = Get-ChildItem -Path $kitRoot -Filter $ToolName -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\x64\\" } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    $cmd = Get-Command $ToolName -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "$ToolName not found. Ensure Windows 11 WDK is installed."
}

Write-Host "[1/4] Building AtrisWaveVAD ($Platform | $Configuration)..." -ForegroundColor Cyan
$msbuild = Find-MsBuild
& $msbuild $ProjectFile /p:Configuration=$Configuration /p:Platform=$Platform /m /v:minimal
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE" }

if (-not (Test-Path $SysPath)) {
    throw "Expected driver binary not found: $SysPath"
}

Write-Host "[2/4] Ensuring local test-signing certificate ($CertSubject)..." -ForegroundColor Cyan
$cert = Get-ChildItem -Path $CertStore | Where-Object { $_.Subject -eq $CertSubject } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $CertSubject `
        -CertStoreLocation $CertStore `
        -NotAfter (Get-Date).AddYears(3)
    Write-Host "Created new test certificate: $($cert.Thumbprint)" -ForegroundColor Green
} else {
    Write-Host "Reusing existing test certificate: $($cert.Thumbprint)" -ForegroundColor Green
}

Write-Host "[3/4] Generating catalog (inf2cat) & signing .sys / .cat..." -ForegroundColor Cyan
$signtool = Find-WdkTool "signtool.exe"
$inf2cat = Find-WdkTool "inf2cat.exe"

$osTarget = if ($Platform -eq "ARM64") { "10_VB_ARM64,ServerFE_ARM64" } else { "10_VB_X64,ServerFE_X64" }
& $inf2cat /driver:"$OutDir" /os:$osTarget
if ($LASTEXITCODE -ne 0) { throw "inf2cat failed with exit code $LASTEXITCODE" }

& $signtool sign /fd SHA256 /s My /n "AtrisWave Test Signing Certificate" /tr http://timestamp.digicert.com /td SHA256 $SysPath
if ($LASTEXITCODE -ne 0) {
    Write-Warning "Timestamp server unreachable; signing without timestamp for offline test VM..."
    & $signtool sign /fd SHA256 /s My /n "AtrisWave Test Signing Certificate" $SysPath
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $SysPath" }
}

if (Test-Path $CatPath) {
    & $signtool sign /fd SHA256 /s My /n "AtrisWave Test Signing Certificate" $CatPath
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $CatPath" }
}

Write-Host "[4/4] Package ready in: $OutDir" -ForegroundColor Green

if ($Install) {
    Write-Host "Installing test certificate & driver package via pnputil..." -ForegroundColor Yellow
    $cerPath = Join-Path $OutDir "AtrisWaveTest.cer"
    Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null
    Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\Root" | Out-Null
    Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher" | Out-Null

    & pnputil.exe /add-driver $InfPath /install
    if ($LASTEXITCODE -ne 0) { throw "pnputil failed with exit code $LASTEXITCODE" }
    Write-Host "Driver installed. Ensure 'bcdedit /set testsigning on' is active on this VM." -ForegroundColor Green
}
