# AtrisWave Installer & Release Packaging (`installer/`)

This directory contains the release staging script, **Inno Setup 6** installer definition, and **WinGet** manifest template for distributing **AtrisWave** on Windows 10/11 (x64).

## Contents
- `scripts/build-release.ps1` — End-to-end release pipeline that builds and tests `engine/` (`msvc-release`), `plugin-host/` (`msvc-release`), and `ui/` (`Release`, `win-x64`), then stages all binaries into `dist/AtrisWave-win-x64/`.
- `AtrisWave.iss` — Production Inno Setup 6 script (Apache-2.0 license, English & Turkish installer languages, packages `AtrisWave.UI.exe`, `AtrisWaveEngine.exe`, and `AtrisWavePluginHost.exe`, optional Session 1+ logon auto-start for `AtrisWaveEngine.exe`, Start Menu & Desktop shortcuts, clean uninstall).
- `winget/AtrisWave.yaml` — WinGet v1.6 singleton manifest template (`AtrisWave.AtrisWave`, Apache-2.0, `v1.0.0-rc1`).

## Building a Release Installer

Run from an **x64 Native Tools Command Prompt for VS** (or after running `vcvars64.bat`):

```powershell
pwsh -File installer/scripts/build-release.ps1
iscc installer/AtrisWave.iss
```

> **Note:** `AtrisWaveEngine.exe` runs in **Session 1+** user context (auto-started via `HKCU\...\Run` if selected), never as a Session 0 Windows Service. For Phase 2 kernel driver signing (`AtrisWaveVAD.sys`), see `driver/scripts/build-and-testsign.ps1`.
