English | [Türkçe](README-TR.md)

# AtrisWave

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](LICENSE)
[![Platform: Windows 10/11](https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-0078D6.svg)](#)
[![Stack: .NET 10 WPF + C++20](https://img.shields.io/badge/Stack-.NET%2010%20WPF%20%2B%20C%2B%2B20-512BD4.svg)](#)

**AtrisWave** is a native, open-source, all-in-one Windows audio suite built for gamers, streamers, podcasters, and creators. It combines **virtual channel splitting**, **per-application audio routing**, **real-time microphone DSP**, **low-latency dual-path monitoring**, and **sandboxed CLAP/VST3 plugin hosting** in a clean, bloat-free desktop studio.

---

## Key Capabilities

- **Virtual Channel Splitting**: Route *Master*, *Game*, *Chat*, *Media*, *Aux*, and *Microphone* independently with dedicated faders, mute/solo states, and stream/monitor sub-mixes.
- **WASAPI `IAudioClient3` & Per-App Routing**: Low-latency Windows audio backend with **WASAPI Process Loopback** (`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`) and persistent per-application routing rules.
- **Full Dual-Path Microphone DSP**:
  - **Direct Monitor (Sidetone)**: Hear your voice in **~6–12 ms** (WASAPI `IAudioClient3`) bypassing heavy spectral/reverb stages.
  - **Broadcast / Stream Chain**: Spectral Denoiser, Noise Gate, 5-Band Cytomic/Simper ZDF SVF Parametric EQ (with **AutoEq `.txt` import**), Compressor + **Sidechain Ducking**, Algorithmic + **FFT Convolution Reverb** (*Studio Booth*, *Broadcast Room*, *Concert Hall*), Real-Time **Pitch Shifter** (`-12 .. +12 st`), and **True-Peak Limiter**.
- **Out-of-Process Plugin Sandbox (`plugin-host/`)**: Host **CLAP** and **VST3 (3.8+ MIT)** plugins in an isolated `AtrisWavePluginHost.exe` process with **5 ms crash-bypass** protection.
- **Automation & Global Hotkeys**: Native Win32 global hotkeys (`Ctrl+Alt+M` Mic Mute, `Ctrl+Alt+S` Sidetone) and a loopback-only Stream Deck / Touch Portal **JSON-RPC 2.0 API** (`http://127.0.0.1:18923/rpc`).
- **60 FPS Studio UI**: Modern Windows 11 dark Fluent interface (`Wpf.Ui`) with allocation-free 60 FPS `SkiaSharp` VU metering, live EQ curve editor, system tray Quick-Mix, and instant English / Turkish localization.

---

## 4-Component Hybrid Architecture

AtrisWave decouples UI rendering, real-time DSP, third-party plugin execution, and kernel audio endpoints (see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full engineering specification):

```text
  [ C# .NET 10 WPF UI ]  <── 576B Seqlock SHM (60 FPS) ──  [ C++20 Engine Daemon ]
   (ui/ — Wpf.Ui + Skia)  ── Named Pipe JSON Control ──>   (engine/ — Session 1+)
                                                                 │            │
                                          5ms Crash-Bypass SHM ──┘            └── Direct Ring / IOCTL
                                                  v                                        v
                                     [ AtrisWavePluginHost.exe ]              [ AtrisWaveVAD.sys (WDK) ]
                                     (plugin-host/ — CLAP/VST3)                (driver/ — 6 Endpoints)
```

1. **Desktop UI (`ui/`)**: C# `.NET 10` WPF application (`AtrisWave.UI.exe`) using `Wpf.Ui` Fluent styling, `SkiaSharp.Views.WPF` (`SKElement`) 60 FPS meters, `H.NotifyIcon.Wpf` system tray, global hotkeys, and loopback JSON-RPC server.
2. **Audio Engine (`engine/`)**: Headless **C++20** Session 1+ daemon (`AtrisWaveEngine.exe`) running with MMCSS `"Pro Audio"` priority, FTZ/DAZ denormal flushing, lock-free SPSC ring buffers, WASAPI `IAudioClient3` / Process Loopback, and zero-allocation DSP.
3. **Virtual Audio Driver (`driver/`)**: C++ WDK **WaveRT / SysVAD** kernel driver (`AtrisWaveVAD.sys`) exposing 6 virtual endpoints (*Game*, *Chat*, *Media*, *Aux*, *Processed Mic*, *Stream Mix*) with RFC 3161 test-signing & Attestation Signing scripts (`driver/scripts/build-and-testsign.ps1`).
4. **Out-of-Process Plugin Host (`plugin-host/`)**: Isolated C++20 sandbox (`AtrisWavePluginHost.exe`) for **CLAP** and **VST3 (3.8+)** plugins with lock-free shared-memory audio transport and automatic 5 ms crash-bypass failover.

---

## Phased Delivery & Driver Signing Strategy

Kernel-mode Windows drivers require an **EV Code Signing Certificate** and **Microsoft Hardware Dev Center Attestation Signing** to load on standard Windows 10/11 systems without Test-Signing mode. AtrisWave supports both modes seamlessly:

- **Phase 0 — Foundation & Real-Time Skeleton**: Shared C-ABI IPC (`Local\AtrisWaveTelemetryV1` seqlock SHM + `\\.\pipe\AtrisWaveEngine` current-user DACL Named Pipe), C++20 RT primitives, and .NET 10 WPF Studio UI.
- **Phase 1 — Driverless Routing & Full Mic DSP**: Windows 10/11 **WASAPI Process Loopback** (`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`), persistent per-app routing, `IAudioClient3`, and the complete Dual-Path Microphone DSP chain.
- **Phase 2 — Native Virtual Audio Driver (`driver/`)**: Open-source SysVAD/WaveRT driver (`AtrisWaveVAD.sys`) exposing 6 virtual endpoints with validated shared ring buffers and automated test-signing / Attestation packaging.
- **Phase 3 — Out-of-Process Plugin Sandbox, Automation & Release (`plugin-host/`, `installer/`)**: Sandboxed CLAP/VST3 hosting with 5 ms crash bypass, AutoEq `.txt` import, FFT convolution reverb, pitch shifter, global hotkeys, Stream Deck JSON-RPC API, Inno Setup 6 installer, and WinGet manifest.

---

## Build, Test & Packaging

### Prerequisites
- Windows 10 (2004+) or Windows 11 (x64)
- Visual Studio 2022/2026 with **Desktop development with C++**, CMake 3.25+, Ninja
- [.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0) (`10.0.x`)
- *(Optional)* Inno Setup 6 (`iscc.exe`) for building the Windows installer, and Windows WDK for `driver/`

### 1. C++20 Audio Engine (`engine/`)
Run from an **x64 Native Tools Command Prompt for VS** (or after running `vcvars64.bat`):

```powershell
cd engine
cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --preset msvc-release --output-on-failure
```

### 2. C++20 Out-of-Process Plugin Host (`plugin-host/`)

```powershell
cd plugin-host
cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --preset msvc-release --output-on-failure
```

### 3. .NET 10 WPF UI & Tests (`ui/`)

```powershell
dotnet restore ui/AtrisWave.sln
dotnet build ui/AtrisWave.sln --configuration Release --no-restore
dotnet test ui/AtrisWave.sln --configuration Release --no-build
```

### 4. Release Staging & Installer (`installer/`)

```powershell
pwsh -File installer/scripts/build-release.ps1
iscc installer/AtrisWave.iss
```

See [installer/README.md](installer/README.md) and [installer/winget/AtrisWave.yaml](installer/winget/AtrisWave.yaml) for WinGet packaging details.

---

## Contributing, Security & License

- **Contributing**: Contributions are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) and our [Code of Conduct](CODE_OF_CONDUCT.md).
- **Security**: Please report vulnerabilities privately via GitHub Security Advisories as described in [SECURITY.md](SECURITY.md).
- **License**: AtrisWave is licensed under the [Apache License 2.0](LICENSE).
