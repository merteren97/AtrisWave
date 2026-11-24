<#
.SYNOPSIS
    End-to-end Windows x64 release build, test, and staging script for AtrisWave.

.DESCRIPTION
    1. Configures and builds C++20 engine/ (msvc-release) and runs CTest.
    2. Configures and builds C++20 plugin-host/ (msvc-release) and runs CTest.
    3. Restores, builds, tests, and publishes ui/AtrisWave.UI (Release, win-x64).
    4. Stages all release binaries and documentation into dist/AtrisWave-win-x64/.

.PARAMETER SkipTests
    Skip CTest and xUnit test execution (not recommended for release builds).

.PARAMETER SelfContained
    Publish AtrisWave.UI as self-contained win-x64 (default: false / framework-dependent).
#>
[CmdletBinding()]
param(
    [switch]$SkipTests,
    [switch]$SelfContained
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot   = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$StagingDir = Join-Path $RepoRoot 'dist\AtrisWave-win-x64'

function Invoke-Step {
    param(
        [Parameter(Mandatory)] [string]$Title,
        [Parameter(Mandatory)] [scriptblock]$Action
    )
    Write-Host "`n=== $Title ===" -ForegroundColor Cyan
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "Step '$Title' failed with exit code $LASTEXITCODE."
    }
}

# 1. C++20 Audio Engine (engine/)
Invoke-Step 'Configure & Build C++20 Engine (msvc-release)' {
    Push-Location (Join-Path $RepoRoot 'engine')
    try {
        cmake --preset msvc-release
        if ($LASTEXITCODE -ne 0) { return }
        cmake --build --preset msvc-release
    }
    finally {
        Pop-Location
    }
}

if (-not $SkipTests) {
    Invoke-Step 'Run C++20 Engine Tests (CTest)' {
        Push-Location (Join-Path $RepoRoot 'engine')
        try {
            ctest --preset msvc-release --output-on-failure
        }
        finally {
            Pop-Location
        }
    }
}

# 2. C++20 Out-of-Process Plugin Host (plugin-host/)
Invoke-Step 'Configure & Build C++20 Plugin Host (msvc-release)' {
    Push-Location (Join-Path $RepoRoot 'plugin-host')
    try {
        cmake --preset msvc-release
        if ($LASTEXITCODE -ne 0) { return }
        cmake --build --preset msvc-release
    }
    finally {
        Pop-Location
    }
}

if (-not $SkipTests) {
    Invoke-Step 'Run Plugin Host Tests (CTest)' {
        Push-Location (Join-Path $RepoRoot 'plugin-host')
        try {
            ctest --preset msvc-release --output-on-failure
        }
        finally {
            Pop-Location
        }
    }
}

# 3. .NET 10 WPF UI & Core Tests (ui/)
$SolutionPath = Join-Path $RepoRoot 'ui\AtrisWave.sln'
$UiProject    = Join-Path $RepoRoot 'ui\AtrisWave.UI\AtrisWave.UI.csproj'

Invoke-Step 'Restore & Build .NET 10 UI Solution (Release)' {
    dotnet restore $SolutionPath
    if ($LASTEXITCODE -ne 0) { return }
    dotnet build $SolutionPath --configuration Release --no-restore
}

if (-not $SkipTests) {
    Invoke-Step 'Run .NET 10 Core & Integration Tests (xUnit)' {
        dotnet test $SolutionPath --configuration Release --no-build --verbosity normal
    }
}

# 4. Clean & Stage Release Artifacts into dist/AtrisWave-win-x64/
if (Test-Path $StagingDir) {
    Remove-Item -Path $StagingDir -Recurse -Force
}
New-Item -Path $StagingDir -ItemType Directory -Force | Out-Null

Invoke-Step 'Publish AtrisWave.UI (win-x64 Release)' {
    $scFlag = if ($SelfContained) { 'true' } else { 'false' }
    dotnet publish $UiProject `
        --configuration Release `
        --runtime win-x64 `
        --self-contained $scFlag `
        --output $StagingDir
}

Invoke-Step 'Copy Native Binaries & Docs to Staging Directory' {
    $EngineExe     = Join-Path $RepoRoot 'engine\build\msvc-release\AtrisWaveEngine.exe'
    $PluginHostExe = Join-Path $RepoRoot 'plugin-host\build\msvc-release\AtrisWavePluginHost.exe'

    if (-not (Test-Path $EngineExe))     { throw "Missing engine binary: $EngineExe" }
    if (-not (Test-Path $PluginHostExe)) { throw "Missing plugin-host binary: $PluginHostExe" }

    Copy-Item -Path $EngineExe     -Destination $StagingDir -Force
    Copy-Item -Path $PluginHostExe -Destination $StagingDir -Force
    Copy-Item -Path (Join-Path $RepoRoot 'LICENSE')   -Destination $StagingDir -Force
    Copy-Item -Path (Join-Path $RepoRoot 'README.md') -Destination $StagingDir -Force
}

Write-Host "`n[OK] Release staged at: $StagingDir" -ForegroundColor Green
Write-Host "Next: Run 'iscc installer\AtrisWave.iss' to build the Inno Setup 6 installer." -ForegroundColor Gray
