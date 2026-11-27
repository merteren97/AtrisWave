English | [Türkçe](README-TR.md)

# AtrisWave

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](LICENSE)
[![CI](https://github.com/merteren97/AtrisWave/actions/workflows/ci.yml/badge.svg)](https://github.com/merteren97/AtrisWave/actions/workflows/ci.yml)
[![Release](https://github.com/merteren97/AtrisWave/actions/workflows/release.yml/badge.svg)](https://github.com/merteren97/AtrisWave/releases)
[![Platform: Windows 10/11](https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-0078D6.svg)](#)
[![Stack: .NET 10 WPF + C++20](https://img.shields.io/badge/Stack-.NET%2010%20WPF%20%2B%20C%2B%2B20-512BD4.svg)](#)

**AtrisWave** is a native, open-source, all-in-one Windows audio suite built for gamers, streamers, podcasters, and creators. It combines **virtual channel splitting**, **per-application audio routing**, **real-time microphone DSP**, **low-latency dual-path monitoring**, and **sandboxed CLAP/VST3 plugin hosting** in a clean, bloat-free desktop studio.

---

## Key Capabilities

- **Dynamic 16-Channel Rack (`+ Add Channel`)**: Start cleanly with default *System* (catch-all) and *Microphone* strips, or dynamically add/rename/delete up to 16 virtual channels with dedicated faders, mute/solo states, and drag-and-drop per-app binding.
- **`A1 / A2 / B1 / B2` Hardware & Virtual Output Matrix**: Route any channel independently to 4 master output buses (*A1/A2* physical headphones/speakers, *B1/B2* virtual stream/chat outputs) with per-bus `-0.5 dBFS` lookahead limiting, **Feedback Loop Blocker**, and **Process Loopback `A1/A2` Echo Lock**.
- **Dedicated High-Res Master Clock & Auto-Watchdog**: 10 ms `CreateWaitableTimerExW` Master Clock with full stereo `AdaptivePllResampler`, plus automatic sibling `AtrisWaveWatchdog.exe` launch (3-layer default-device failsafe) so Windows audio never stays stranded on a virtual device.
- **12-Pad Real-Time WAV Soundboard**: Trigger custom `.wav` clips directly into any virtual mix or output bus in real time.
- **WASAPI `IAudioClient3` & Per-App Routing**: Low-latency Windows audio backend with **WASAPI Process Loopback** (`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`) and persistent per-application routing rules.
- **Full Dual-Path Microphone DSP**:
  - **Direct Monitor (Sidetone)**: Hear your voice in **~6–12 ms** (WASAPI `IAudioClient3`) bypassing heavy spectral/reverb stages.
  - **Broadcast / Stream Chain**: Spectral Denoiser, Noise Gate, 5-Band Cytomic/Simper ZDF SVF Parametric EQ (with **AutoEq `.txt` import** & **Live 32-Band FFT Spectrum Analyzer**), Compressor + **Cross-Channel Sidechain Ducking**, Algorithmic + **FFT Convolution Reverb** (*Studio Booth*, *Broadcast Room*, *Concert Hall*), Real-Time **Pitch Shifter** (`-12 .. +12 st`), and **True-Peak Limiter**.
- **Out-of-Process Plugin Sandbox (`plugin-host/`)**: Host **CLAP** and **VST3 (3.8+ MIT)** plugins in an isolated `AtrisWavePluginHost.exe` process with **5 ms crash-bypass** protection.
- **Automation, Hotkeys & Auto-Updater**: Native Win32 global hotkeys (`Ctrl+Alt+M` Mic Mute, `Ctrl+Alt+S` Sidetone), loopback-only Stream Deck / Touch Portal **JSON-RPC 2.0 API** (`http://127.0.0.1:18923/rpc`), and a built-in **GitHub Releases Auto-Updater** with **SHA-256** (`SHA256SUMS.txt`) checksum verification.
- **60 FPS Obsidian Studio Console UI (`v0.1.4`)**: Sonar-inspired Console Mixer with Master/System, Game, Chat, Mic, and 1-click Media/Aux/Custom virtual channels, live active render/capture app chips with drag-and-drop routing, real-time ChatMix Game/Chat balance slider, optional Streamer Mode (`A1/A2/B1/B2`), Simple/Pro Mic Studio with **5 One-Click Studio Voice Presets** & **A/B Compare**, crystal-clear **44.1 / 48 / 96 / 192 kHz** sample-rate PLL matching, custom 60 FPS `SkiaSharp` Rotary Knobs / Console Faders / VU Meters, 12-Pad WAV Soundboard, Quick-Mix Tray Flyout, and EN/TR localization.

---

## 4-Component Hybrid Architecture

AtrisWave decouples UI rendering, real-time DSP, third-party plugin execution, and kernel audio endpoints (see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full engineering specification):

```text
  [ C# .NET 10 WPF UI ]  <── 832B Seqlock SHM V3 (60 FPS) ──  [ C++20 Engine Daemon ]
   (ui/ — Wpf.Ui + Skia)  ── Named Pipe JSON Control ───────>   (engine/ — Session 1+)
                                                                  │       │       │
                                           5ms Crash-Bypass SHM ──┘       │       └── Direct Ring / IOCTL
                                                   v                      v                  v
                                      [ AtrisWavePluginHost.exe ]  [ Watchdog.exe ]  [ AtrisWaveVAD.sys ]
                                      (plugin-host/ — CLAP/VST3)   (Auto-Failsafe)   (driver/ — 16-Slot Pool)
```

1. **Desktop UI (`ui/`)**: C# `.NET 10` WPF Studio Console (`AtrisWave.UI.exe`) with Left Fluent Navigation Rail, custom 60 FPS `SkiaSharp` Rotary Knobs / Console Faders / VU Meters / 32-band FFT Spectrum EQ, Slide-Out Channel DSP Drawer, EarTrumpet-style Quick-Mix Tray Flyout, custom `atriswave.ico`, global hotkeys, loopback JSON-RPC server, and GitHub Releases update checker.
2. **Audio Engine & Failsafe (`engine/`)**: Headless **C++20** Session 1+ daemon (`AtrisWaveEngine.exe`) running a dedicated `CreateWaitableTimerExW` Master Clock with MMCSS `"Pro Audio"` priority, FTZ/DAZ denormal flushing, `AdaptivePllResampler`, lock-free SPSC ring buffers, and `AtrisWaveWatchdog.exe` default-device failsafe.
3. **Virtual Audio Driver (`driver/`)**: C++ WDK **WaveRT / SysVAD** kernel driver (`AtrisWaveVAD.sys`) with a **16-slot `KSJACK_DESCRIPTION.IsConnected` pooled endpoint architecture** and RFC 3161 test-signing & Attestation Signing scripts (`driver/scripts/build-and-testsign.ps1`).
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
