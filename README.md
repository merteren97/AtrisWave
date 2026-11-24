English | [Türkçe](README-TR.md)

# AtrisWave

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](LICENSE)
[![Platform: Windows 10/11](https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-0078D6.svg)](#)
[![Stack: .NET 10 WPF + C++20](https://img.shields.io/badge/Stack-.NET%2010%20WPF%20%2B%20C%2B%2B20-512BD4.svg)](#)

**AtrisWave** is a native, open-source, all-in-one Windows audio suite built for gamers, streamers, podcasters, and creators. It combines **virtual channel splitting**, **per-application audio routing**, **real-time microphone DSP**, and **low-latency dual-path monitoring** in a clean, bloat-free desktop studio.

---

## Vision & Core Capabilities

- **Virtual Channel Splitting**: Route *Master*, *Game*, *Chat*, *Media*, *Aux*, and *Microphone* independently with dedicated faders, mute/solo states, and stream/monitor sub-mixes.
- **Per-Application Routing**: Isolate and route individual Windows applications cleanly without fragile workarounds.
- **Real-Time Microphone DSP**: Low-latency voice processing chain featuring AI noise suppression, noise gate, parametric EQ (Cytomic/Simper ZDF SVF), compressor, reverb, and brickwall limiter.
- **Dual-Path Low-Latency Monitoring**: Hear your raw/lightly-EQ'd voice in **~6–12 ms** (WASAPI `IAudioClient3`) on your headphones while sending the full AI-denoised broadcast chain to your stream or voice chat.
- **Extensible Plugin Hosting (Planned)**: Out-of-process sandboxed **CLAP** and **VST3 (3.8+)** plugin hosting.
- **60 FPS Studio UI**: Modern Windows 11 dark Fluent interface (`Wpf.Ui`) with allocation-free 60 FPS `SkiaSharp` VU metering, system tray Quick-Mix, and live English / Turkish localization.

---

## 3-Layer Hybrid Architecture

AtrisWave decouples UI rendering, real-time DSP, and kernel audio endpoints into three specialized layers (see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full engineering specification):

```text
  [ C# .NET 10 WPF UI ]  <── 576B Seqlock SHM (60 FPS) ──  [ C++20 Engine Daemon ]
   (ui/ — Wpf.Ui + Skia)  ── Named Pipe JSON Control ──>   (engine/ — Session 1+)
                                                                    │
                                                     Phase 2: Direct Ring / IOCTL
                                                                    v
                                                     [ AtrisWaveVAD.sys (WDK) ]
                                                      (driver/ — WaveRT/SysVAD)
```

1. **Desktop UI (`ui/`)**: C# `.NET 10` WPF application using `Wpf.Ui` Fluent styling, `SkiaSharp.Views.WPF` (`SKElement`) 60 FPS meters, and `H.NotifyIcon.Wpf` system tray controls.
2. **Audio Engine (`engine/`)**: Headless **C++20** Session 1+ daemon (`AtrisWaveEngine.exe`) running with MMCSS `"Pro Audio"` priority, FTZ/DAZ denormal flushing, lock-free SPSC ring buffers, and zero-allocation DSP.
3. **Virtual Audio Driver (`driver/`)**: Planned Phase 2 C++ WDK **WaveRT / SysVAD** kernel driver (`AtrisWaveVAD.sys`) exposing native virtual playback and capture endpoints.

---

## Phased Roadmap & Driver Signing Strategy

Kernel-mode Windows drivers require an **EV Code Signing Certificate** and **Microsoft Hardware Dev Center Attestation Signing** to load on standard Windows 10/11 systems without enabling Test-Signing mode. AtrisWave uses a pragmatic phased rollout so users get value immediately:

- **Phase 0 — Foundation & Real-Time Skeleton (Current)**
  - Shared C-ABI IPC contract (`Local\AtrisWaveTelemetryV1` seqlock shared memory + `\\.\pipe\AtrisWaveEngine` Named Pipe with current-user DACL).
  - C++20 engine skeleton (`RtSafety`, `SpscRingBuffer`, `SimperSvf`, `TelemetryPublisher`, `ControlServer`).
  - .NET 10 WPF Studio UI with 6-channel mixer, 60 FPS SkiaSharp VU meters, System Tray, and EN/TR localization.
- **Phase 1 — Driverless Routing & Mic DSP**
  - Per-app audio capture via Windows 10/11 **WASAPI Process Loopback** (`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`) + optional signed virtual cable bridge (e.g., VB-Cable).
  - Full Dual-Path Microphone DSP chain (DeepFilterNet3-LL AI denoise, gate, EQ, compressor, reverb, limiter).
- **Phase 2 — Native Virtual Audio Driver (`AtrisWaveVAD.sys`)**
  - Custom open-source SysVAD/WaveRT driver with direct shared-memory ring buffer to `AtrisWaveEngine.exe`, distributed via Attestation Signing.
- **Phase 3 — Out-of-Process Plugin Host (`plugin-host/`)**
  - Sandboxed **CLAP** and **VST3 (3.8+)** plugin hosting with crash isolation.

---

## Build & Test Instructions

### Prerequisites
- Windows 10 (2004+) or Windows 11 (x64)
- Visual Studio 2022/2026 with **Desktop development with C++**, CMake 3.25+, Ninja
- [.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0) (`10.0.x`)

### Build & Test the C++20 Engine (`engine/`)
Run from an **x64 Native Tools Command Prompt for VS** (or after running `vcvars64.bat`):

```powershell
cd engine
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug --output-on-failure
```

*(Replace `msvc-debug` with `msvc-release` for Release builds.)*

### Build & Test the .NET 10 WPF UI (`ui/`)

```powershell
dotnet build ui/AtrisWave.sln --configuration Release
dotnet test ui/AtrisWave.sln --configuration Release
```

---

## Contributing, Security & License

- **Contributing**: Contributions are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) and our [Code of Conduct](CODE_OF_CONDUCT.md).
- **Security**: Please report vulnerabilities privately via GitHub Security Advisories as described in [SECURITY.md](SECURITY.md).
- **License**: AtrisWave is licensed under the [Apache License 2.0](LICENSE).
