# AtrisWave Installer & Packaging (`installer/`)

> **Status:** Planned for **Phase 1+** (Desktop Packaging & Distribution).

This directory will house the packaging scripts and installer manifests for distributing AtrisWave on Windows 10/11 (x64).

## Planned Scope
- **WiX Toolset v5 / MSIX** installer bundling `AtrisWave.UI.exe` (.NET 10), `AtrisWaveEngine.exe` (C++20 Session 1+ daemon), and (in Phase 2) the signed `AtrisWaveVAD.sys` INF package.
- **Portable ZIP** release packaging for Phase 1 driverless deployments.
- **WinGet** manifest templates for automated community package updates.
- Automated startup task registration (Session 1+ user-context auto-launch, never Session 0 Windows Service).
