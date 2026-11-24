# AtrisWave Engineering Architecture

This document is the authoritative technical reference for **AtrisWave**—a native, open-source Windows 10/11 audio routing, virtual mixing, real-time microphone DSP, and sandboxed plugin-hosting suite.

---

## 1. Four-Component Hybrid Architecture

AtrisWave separates **presentation**, **real-time audio processing**, **third-party plugin execution**, and **kernel device emulation** so UI frames never stall audio callbacks and third-party plugin crashes never drop your microphone or stream audio.

```text
+-----------------------------------------------------------------------------+
| 1. Desktop UI (ui/ — C# .NET 10 WPF + Wpf.Ui + SkiaSharp)                   |
|    - 60 FPS SKElement VU meters & live EQ curve (zero per-frame allocs)     |
|    - System Tray Quick-Mix, EN/TR live localization, AutoEq .txt importer   |
|    - Win32 Global Hotkeys (Ctrl+Alt+M / Ctrl+Alt+S)                         |
|    - Loopback-only JSON-RPC 2.0 Server (http://127.0.0.1:18923/rpc)         |
+--------------------+------------------------------------+-------------------+
                     | Reads 60 FPS (Seqlock)             | JSON-Lines Pipe
                     v                                    v
       [Local\AtrisWaveTelemetryV1]          [\\.\pipe\AtrisWaveEngine]
            (576-byte SHM)                     (Current-User DACL Pipe)
                     ^                                    ^
                     | Writes RT-safe (Zero-Lock)         | Non-RT Control
+--------------------+------------------------------------+-------------------+
| 2. Audio Engine Daemon (engine/ — C++20 Session 1+ Process)                 |
|    - AtrisWaveEngine.exe (MMCSS "Pro Audio" + FTZ/DAZ)                      |
|    - WASAPI IAudioClient3 + Process Loopback + Persistent App Routing       |
|    - Dual-Path Mic DSP Graph (Spectral Denoise, Gate, 5-Band Simper SVF EQ, |
|      Compressor + Sidechain Ducking, Algorithmic/FFT Reverb, Pitch, Limiter)|
+-------------------+-------------------------------------+-------------------+
                    | Phase 3: Lock-Free SHM + 5ms Bypass | Phase 2: Ring/IOCTL
                    v                                     v
+---------------------------------------+   +---------------------------------+
| 3. Out-of-Process Plugin Host         |   | 4. Virtual Audio Driver         |
|    (plugin-host/ — C++20)             |   |    (driver/ — C++ WDK)          |
|    - AtrisWavePluginHost.exe          |   |    - AtrisWaveVAD.sys           |
|    - CLAP & VST3 (3.8+ MIT) sandbox   |   |    - SysVAD / WaveRT PortCls    |
|    - Automatic 5ms crash-bypass       |   |    - 6 virtual endpoints        |
+---------------------------------------+   +---------------------------------+
```

---

## 2. Session 1+ Engine Daemon Rationale (Session 0 Veto)

AtrisWave **explicitly vetoes** running the audio engine as a traditional Session 0 Windows Service:

1. **WASAPI & Per-User Endpoint Isolation**: Windows audio endpoints, `IAudioClient3` low-latency streams, and `AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS` are bound to the interactive user session (Session 1+). A Session 0 service cannot reliably enumerate or capture user-session audio devices or per-app audio without brittle cross-session impersonation.
2. **IPC & Shared Memory Boundary**: Running `AtrisWaveEngine.exe` as a headless **Session 1+** daemon in the user's desktop session allows direct use of `Local\AtrisWaveTelemetryV1` shared memory and a Named Pipe (`\\.\pipe\AtrisWaveEngine`) secured with the current user's SID—without exposing system-wide `Global\` objects across session boundaries.
3. **Auto-Start**: Managed via an optional user-context `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` entry in the Inno Setup 6 installer (`installer/AtrisWave.iss`), never `services.msc`.

---

## 3. Dual-Path Microphone DSP Graph & Latency Budget

Traditional software DSP chains introduce 15–40 ms of latency when spectral/AI noise suppression and convolution reverb are enabled, which causes distracting "comb-filtering" echo when monitoring your own voice. AtrisWave solves this with a **Dual-Path Microphone Graph**:

| Path | Target Latency | Processing Chain | Purpose |
| :--- | :--- | :--- | :--- |
| **Path A: Direct Monitor (Sidetone)** | **~6–12 ms** (WASAPI `IAudioClient3` low-latency software round-trip) | Input -> Optional High-Pass / Light EQ -> Output Headphones | Zero-distraction, natural real-time voice monitoring |
| **Path B: Broadcast / Stream Mix** | **10 – 25 ms** (lookahead + spectral/IR frame size) | Input -> Spectral Denoiser -> Noise Gate -> 5-Band Parametric EQ (Simper SVF + AutoEq) -> Compressor (with Sidechain Ducking) -> Reverb (Algorithmic or FFT Convolution IR) -> Pitch Shifter (`-12 .. +12 st`) -> True-Peak Limiter -> Virtual Mic / Stream Mix | Studio-grade processed voice for Discord, OBS, and recordings |

---

## 4. Shared IPC & Telemetry Contracts (`shared/`)

All cross-process binary contracts live in `shared/include/atriswave/` and use `#pragma pack(push, 4)` with `static_assert` size/offset verification:

### 4.1 Telemetry Shared Memory: `Local\AtrisWaveTelemetryV1` (`ipc_protocol.h`)
- **Total Size**: **576 bytes** (`TelemetrySnapshot` = 64-byte `TelemetryHeader` + `16 × 32-byte` `ChannelMeterFrame`), matched byte-for-byte in C# (`StructLayout(LayoutKind.Sequential, Pack = 4)`).
- **Magic & Version**: `ATRISWAVE_TELEMETRY_MAGIC = 0x4157544Du` (`"AWTM"`), `ATRISWAVE_TELEMETRY_VERSION = 1u`, `ATRISWAVE_MAX_CHANNELS = 16u`.
- **Lock-Free Seqlock Protocol**:
  - `TelemetryHeader::sequence` (`uint64_t`, offset 24, 8-byte aligned) is updated with `std::memory_order_relaxed` + `std::atomic_thread_fence(std::memory_order_acq_rel)` before and after every RT write.
  - **Odd sequence** = write in progress; **Even sequence** = stable snapshot.
  - The C# reader (`SharedMemoryTelemetryReader`) reuses a pre-allocated buffer, reads `sequence` before and after copying, and discards/retries if `sequence` is odd or changed mid-read.

### 4.2 Named Pipe Control: `\\.\pipe\AtrisWaveEngine`
- **Security**: Constructed with `PIPE_REJECT_REMOTE_CLIENTS` and an explicit **current-user SID DACL** (fail-closed: if building the security descriptor fails, the server refuses to start).
- **Transport**: Non-RT control thread handling newline-delimited UTF-8 JSON commands (with a `64 KiB` max line cap) such as `ping`, `get_status`, `shutdown`, DSP parameter updates, and per-app routing rules.

### 4.3 Out-of-Process Plugin IPC (`plugin_ipc.h`) & 5 ms Crash-Bypass
- `AtrisWavePluginHost.exe` (`plugin-host/`) runs **CLAP** and **VST3 (3.8+ MIT)** plugins out-of-process over lock-free shared-memory audio buffers.
- If a third-party plugin hangs or crashes, the engine's **5 ms watchdog / crash-bypass** seamlessly routes dry/pre-plugin audio around the failed slot with zero audio dropout.

---

## 5. Real-Time Safety Rules (`engine/`)

Every function reachable from the audio callback thread must obey strict real-time invariants:

1. **MMCSS & Denormals**: Audio threads enter the `"Pro Audio"` Multimedia Class Scheduler Service (`AvSetMmThreadCharacteristicsW`) and enable **FTZ/DAZ** (`_MM_SET_FLUSH_ZERO_MODE`, `_MM_SET_DENORMALS_ZERO_MODE`) via `RtSafety` RAII guards.
2. **Zero Allocations & Zero Locks**: No `new`/`malloc`, no `std::mutex`, no file/pipe I/O, and no exceptions on the audio thread.
3. **Lock-Free SPSC Ring Buffers**: Audio and telemetry data cross thread boundaries exclusively through cache-line-aligned (`alignas(64)`) power-of-two `SpscRingBuffer` queues using `std::atomic` acquire/release semantics.
4. **ZDF Filtering**: Parametric EQ and crossovers use the **Cytomic / Simper Zero-Delay Feedback State Variable Filter** (`SimperSvf`), providing unconditional numerical stability under fast parameter modulation.

---

## 6. Kernel Driver Hardening & Signing (`driver/`)

While Phase 1 operates **driverlessly** using Windows 10/11 WASAPI Process Loopback (`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`) and persistent per-app routing, `driver/` provides **`AtrisWaveVAD.sys`** (SysVAD / WaveRT PortCls) exposing 6 virtual endpoints (*Game*, *Chat*, *Media*, *Aux*, *Processed Mic*, *Stream Mix*):

- **Ring-Buffer Validation (`ring_buffer_validate.h`)**: Single-read volatile snapshotting of shared header fields (`magic`, `abi_version`, `capacity_frames`, `write_frame_idx`, `read_frame_idx`) onto local stack variables, `UINT64_MAX` overflow clamping, and `STATUS_DEVICE_BUSY` guards on active format changes.
- **Signing & Distribution**: `driver/scripts/build-and-testsign.ps1` automates WDK builds and RFC 3161 SHA-256 test-signing for local development. Production releases require an **EV Code Signing Certificate** and **Microsoft Hardware Dev Center Attestation Signing** (or WHQL).

---

## 7. Automation & Packaging (`ui/`, `installer/`)

- **Local Automation API**: `LocalAutomationServer` binds strictly to `127.0.0.1` (`IPAddress.Loopback`, default port `18923`) via `TcpListener` (no admin URLACL required) for Stream Deck / Touch Portal `POST /rpc` JSON-RPC 2.0 control with optional Bearer token authentication.
- **Release & Installer**: `installer/scripts/build-release.ps1` builds and tests `engine/`, `plugin-host/`, and `ui/`, staging artifacts into `dist/AtrisWave-win-x64/` for **Inno Setup 6** (`installer/AtrisWave.iss`) and **WinGet** (`installer/winget/AtrisWave.yaml`).
