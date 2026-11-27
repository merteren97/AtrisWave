<#
.SYNOPSIS
    Builds AtrisWaveVAD.sys (PortCls WaveRT Virtual Audio Driver), generates a local
    test-signing certificate, creates/signs the catalog (.cat) and driver (.sys), and
    optionally installs or uninstalls ROOT\AtrisWaveVAD onto the local machine / Test VM.

.PARAMETER Configuration
    Build configuration: Debug or Release (default: Release).

.PARAMETER Platform
    Target architecture: x64 or ARM64 (default: x64).

.PARAMETER Install
    Installs the test certificate into TrustedRoot/TrustedPublisher and installs
    ROOT\AtrisWaveVAD via devcon/pnputil (requires Administrator + testsigning on).

.PARAMETER Uninstall
    Removes ROOT\AtrisWaveVAD via devcon/pnputil (requires Administrator).
#>
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64", "ARM64")]
    [string]$Platform = "x64",

    [switch]$Install,
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

$DriverRoot = Split-Path -Parent $PSScriptRoot
$RepoRoot   = Split-Path -Parent $DriverRoot
$OutDir     = Join-Path $DriverRoot "build\$Platform\$Configuration"
$ObjDir     = Join-Path $DriverRoot "build\obj\$Platform\$Configuration"
$SysPath    = Join-Path $OutDir "AtrisWaveVAD.sys"
$InfSrc     = Join-Path $DriverRoot "AtrisWaveVAD.inf"
$InfPath    = Join-Path $OutDir "AtrisWaveVAD.inf"
$CatPath    = Join-Path $OutDir "AtrisWaveVAD.cat"
$CertSubject = "CN=AtrisWave Test Signing Certificate"
$CertStore   = "Cert:\LocalMachine\My"

function Find-WdkVersion {
    $kitRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\Include"
    if (-not (Test-Path $kitRoot)) {
        throw "Windows Kits 10 not found under $kitRoot. Install Windows 11 WDK."
    }
    $ver = Get-ChildItem $kitRoot -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName "km\portcls.h") } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if (-not $ver) {
        throw "No WDK with km\portcls.h found under $kitRoot."
    }
    return $ver.Name
}

function Find-WdkTool([string]$ToolName, [string]$WdkVer) {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin\$WdkVer\x64\$ToolName",
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin\$WdkVer\x86\$ToolName",
        "${env:ProgramFiles(x86)}\Windows Kits\10\Tools\$WdkVer\x64\$ToolName"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    $kitRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
    $found = Get-ChildItem -Path $kitRoot -Filter $ToolName -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\(x64|x86)\\" } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if ($found) { return $found.FullName }
    $cmd = Get-Command $ToolName -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Find-VcVars64 {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -property installationPath | Select-Object -First 1
        if ($installPath) {
            $vcvars = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $vcvars) { return $vcvars }
        }
    }
    throw "vcvars64.bat not found. Install Visual Studio 2022+ with Desktop C++."
}

$WdkVer = Find-WdkVersion

if ($Uninstall) {
    Write-Host "Removing ROOT\AtrisWaveVAD..." -ForegroundColor Yellow
    $devcon = Find-WdkTool "devcon.exe" $WdkVer
    if ($devcon) {
        & $devcon remove "ROOT\AtrisWaveVAD"
    }
    Write-Host "Done." -ForegroundColor Green
    exit 0
}

Write-Host "[1/4] Building AtrisWaveVAD.sys ($Platform | $Configuration | WDK $WdkVer)..." -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $OutDir, $ObjDir | Out-Null

$KitBase  = "${env:ProgramFiles(x86)}\Windows Kits\10"
$IncKm    = "$KitBase\Include\$WdkVer\km"
$IncKmCrt = "$KitBase\Include\$WdkVer\km\crt"
$IncShDir = Get-ChildItem "$KitBase\Include" -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "shared\specstrings.h") } |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $IncShDir) { throw "Windows SDK shared\specstrings.h not found." }
$IncSh    = Join-Path $IncShDir.FullName "shared"
$IncApp   = "$RepoRoot\shared\include"
$IncSrc   = "$DriverRoot\src"
$LibKm    = "$KitBase\Lib\$WdkVer\km\$Platform"

$vcvars   = Find-VcVars64
$optFlags = if ($Configuration -eq "Debug") { "/Od /Zi /DDBG=1" } else { "/O2 /Oi /DNDEBUG" }
$archDef  = if ($Platform -eq "ARM64") { "/D_ARM64_=1" } else { "/D_AMD64_=1 /DAMD64" }
$machine  = if ($Platform -eq "ARM64") { "ARM64" } else { "X64" }

$sources = @(
    "$DriverRoot\src\driver_entry.cpp",
    "$DriverRoot\src\control_device.cpp",
    "$DriverRoot\src\virtual_endpoints.cpp",
    "$DriverRoot\src\wavert_miniport.cpp"
)

$objs = @()
$clCmds = @()
foreach ($src in $sources) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj  = Join-Path $ObjDir "$name.obj"
    $objs += "`"$obj`""
    $clCmds += "cl.exe /nologo /c /X /kernel /std:c++20 /W4 /WX /GS /GR- /EHs-c- /Zl $optFlags /DPOOL_NX_OPTIN=1 /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00 $archDef /I`"$IncKm`" /I`"$IncKmCrt`" /I`"$IncSh`" /I`"$IncApp`" /I`"$IncSrc`" /Fo`"$obj`" `"$src`""
}

$linkCmd = "link.exe /nologo /DRIVER /SUBSYSTEM:NATIVE,10.00 /MACHINE:$machine /NODEFAULTLIB /ENTRY:DriverEntry /INTEGRITYCHECK /OPT:REF /OPT:ICF /MERGE:_TEXT=.text /OUT:`"$SysPath`" $($objs -join ' ') `"$LibKm\portcls.lib`" `"$LibKm\stdunk.lib`" `"$LibKm\ksguid.lib`" `"$LibKm\wdmsec.lib`" `"$LibKm\ntoskrnl.lib`" `"$LibKm\hal.lib`" `"$LibKm\BufferOverflowFastFailK.lib`" `"$LibKm\libcntpr.lib`""

$fullBatch = "`"$vcvars`" >nul && " + ($clCmds -join " && ") + " && " + $linkCmd
cmd.exe /c $fullBatch
if ($LASTEXITCODE -ne 0) {
    throw "Kernel driver compile/link failed with exit code $LASTEXITCODE"
}

Copy-Item -Path $InfSrc -Destination $InfPath -Force
Write-Host "Built: $SysPath" -ForegroundColor Green

Write-Host "[2/4] Generating catalog (inf2cat)..." -ForegroundColor Cyan
$inf2cat = Find-WdkTool "inf2cat.exe" $WdkVer
if ($inf2cat) {
    $osTarget = if ($Platform -eq "ARM64") { "10_VB_ARM64,ServerFE_ARM64" } else { "10_VB_X64,ServerFE_X64" }
    & $inf2cat /driver:"$OutDir" /os:$osTarget
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "inf2cat returned exit code $LASTEXITCODE (continuing)."
    }
} else {
    Write-Warning "inf2cat.exe not found; skipping .cat generation."
}

Write-Host "[3/4] Test-signing .sys / .cat (if running elevated or cert exists)..." -ForegroundColor Cyan
$signtool = Find-WdkTool "signtool.exe" $WdkVer
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if ($signtool -and $isAdmin) {
    $cert = Get-ChildItem -Path $CertStore -ErrorAction SilentlyContinue | Where-Object { $_.Subject -eq $CertSubject } | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject $CertSubject `
            -CertStoreLocation $CertStore `
            -NotAfter (Get-Date).AddYears(3)
        Write-Host "Created test-signing cert: $($cert.Thumbprint)" -ForegroundColor Green
    }
    & $signtool sign /fd SHA256 /s My /n "AtrisWave Test Signing Certificate" $SysPath
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $SysPath" }
    if (Test-Path $CatPath) {
        & $signtool sign /fd SHA256 /s My /n "AtrisWave Test Signing Certificate" $CatPath
        if ($LASTEXITCODE -ne 0) { throw "signtool failed on $CatPath" }
    }
} elseif (-not $isAdmin) {
    Write-Host "Not elevated; skipping Cert:\LocalMachine\My test-signing. Run with Administrator + -Install to sign & install." -ForegroundColor Yellow
}

Write-Host "[4/4] Driver package ready in: $OutDir" -ForegroundColor Green

if ($Install) {
    if (-not $isAdmin) {
        throw "Administrator privileges are required for -Install."
    }
    Write-Host "Installing test certificate & ROOT\AtrisWaveVAD..." -ForegroundColor Yellow
    $cert = Get-ChildItem -Path $CertStore | Where-Object { $_.Subject -eq $CertSubject } | Select-Object -First 1
    $cerPath = Join-Path $OutDir "AtrisWaveTest.cer"
    Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null
    Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\Root" | Out-Null
    Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher" | Out-Null

    $devcon = Find-WdkTool "devcon.exe" $WdkVer
    if ($devcon) {
        & $devcon remove "ROOT\AtrisWaveVAD" 2>$null | Out-Null
        & $devcon install $InfPath "ROOT\AtrisWaveVAD"
        if ($LASTEXITCODE -ne 0) { throw "devcon install failed with exit code $LASTEXITCODE" }
    } else {
        & pnputil.exe /add-driver $InfPath /install
        if ($LASTEXITCODE -ne 0) { throw "pnputil failed with exit code $LASTEXITCODE" }
    }
    Write-Host "AtrisWaveVAD installed! Ensure 'bcdedit /set testsigning on' is active (reboot required if just enabled)." -ForegroundColor Green
}
