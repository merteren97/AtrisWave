# AtrisWave Engineering Architecture

This document is the authoritative technical reference for **AtrisWave**—a native, open-source Windows 10/11 audio routing, virtual mixing, and real-time microphone DSP suite.

---

## 1. Three-Layer Hybrid Architecture

AtrisWave separates **presentation**, **real-time audio processing**, and **kernel device emulation** into three distinct layers so UI frames never stall audio callbacks and third-party crashes never bring down the system.

```text
+-------------------------------------------------------------------------+
| Layer 1: Desktop UI (ui/ — C# .NET 10 WPF + Wpf.Ui + SkiaSharp)         |
|  - 60 FPS SKElement VU meters (zero per-frame allocations)              |
|  - System Tray Quick-Mix (H.NotifyIcon.Wpf), EN/TR live localization    |
+-------------------+-----------------------------------+-----------------+
                    | Reads 60 FPS (Seqlock)            | JSON-Lines Commands
                    v                                   v
      [Local\AtrisWaveTelemetryV1]         [\\.\pipe\AtrisWaveEngine]
           (576-byte SHM)                    (Current-User DACL Pipe)
                    ^                                   ^
                    | Writes RT-safe (Zero-Lock)        | Non-RT Control Thread
+-------------------+-----------------------------------+-----------------+
| Layer 2: Audio Engine Daemon (engine/ — C++20 Session 1+ Process)       |
|  - AtrisWaveEngine.exe (MMCSS "Pro Audio" + FTZ/DAZ)                    |
|  - Dual-Path Mic DSP Graph + Per-App WASAPI Process Loopback            |
|  - Lock-free SPSC Ring Buffers + Cytomic/Simper ZDF SVF Filters         |
+-----------------------------------+-------------------------------------+
                                    | Phase 2: Direct Shared Ring / IOCTL
                                    v
+-------------------------------------------------------------------------+
| Layer 3: Virtual Audio Driver (driver/ — Phase 2 C++ WDK)               |
|  - AtrisWaveVAD.sys (SysVAD / WaveRT PortCls)                           |
|  - Exposes Game, Chat, Media, Aux, Processed Mic, Stream Mix endpoints  |
+-------------------------------------------------------------------------+
```

---

## 2. Session 1+ Engine Daemon Rationale (Session 0 Veto)

AtrisWave **explicitly vetoes** running the audio engine as a traditional Session 0 Windows Service:

1. **WASAPI & Per-User Endpoint Isolation**: Windows audio endpoints, `IAudioClient3` low-latency streams, and `AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS` are bound to the interactive user session (Session 1+). A Session 0 service cannot reliably enumerate or capture user-session audio devices or per-app audio without brittle cross-session impersonation.
2. **IPC & Shared Memory Boundary**: Running `AtrisWaveEngine.exe` as a headless **Session 1+** daemon in the user's desktop session allows direct use of `Local\AtrisWaveTelemetryV1` shared memory and a Named Pipe (`\\.\pipe\AtrisWaveEngine`) secured with the current user's SID—without exposing system-wide `Global\` objects across session boundaries.
3. **Auto-Start**: Managed via a user-context startup task / tray launcher rather than `services.msc`.

---

## 3. Dual-Path Microphone DSP Graph & Latency Budget

Traditional software DSP chains introduce 15–40 ms of latency when neural noise suppression is enabled, which causes distracting "comb-filtering" echo when monitoring your own voice. AtrisWave solves this with a **Dual-Path Microphone Graph**:

| Path | Target Latency | Processing Chain | Purpose |
| :--- | :--- | :--- | :--- |
| **Path A: Direct Monitor (Sidetone)** | **~6–12 ms** (WASAPI `IAudioClient3` low-latency software round-trip) | Input -> Optional High-Pass / Light EQ -> Output Headphones | Zero-distraction, natural real-time voice monitoring |
| **Path B: Broadcast / Stream Mix** | **10 – 25 ms** (lookahead + AI frame size) | Input -> AI Denoise (DeepFilterNet3-LL) -> Noise Gate -> Parametric EQ (Simper SVF) -> Compressor -> Reverb -> Limiter -> Virtual Mic / Stream Mix | Studio-grade processed voice for Discord, OBS, and recordings |

---

## 4. Shared IPC & Telemetry Contract (`shared/`)

The binary C-ABI contract in [`shared/include/atriswave/ipc_protocol.h`](../shared/include/atriswave/ipc_protocol.h) is shared byte-for-byte between C++20 and C# (`[StructLayout(LayoutKind.Sequential, Pack = 4)]`).

### Shared Memory: `Local\AtrisWaveTelemetryV1`
- **Total Size**: **576 bytes** (`TelemetrySnapshot` = 64-byte `TelemetryHeader` + `16 × 32-byte` `ChannelMeterFrame`).
- **Magic & Version**: `ATRISWAVE_TELEMETRY_MAGIC = 0x4157544Du` (`"AWTM"`), `ATRISWAVE_TELEMETRY_VERSION = 1u`, `ATRISWAVE_MAX_CHANNELS = 16u`.
- **Lock-Free Seqlock Protocol**:
  - `TelemetryHeader::sequence` (`uint64_t`, offset 24) is incremented before and after every RT write.
  - **Odd sequence** = write in progress; **Even sequence** = stable snapshot.
  - The C# reader (`SharedMemoryTelemetryReader`) reads `sequence` before and after copying the 576-byte snapshot and discards/retries if `sequence` is odd or changed mid-read.

### Named Pipe: `\\.\pipe\AtrisWaveEngine`
- **Security**: Constructed with an explicit **current-user DACL** (fail-closed: if building the security descriptor fails, the server refuses to start).
- **Transport**: Non-RT control thread handling newline-delimited UTF-8 JSON commands (e.g., `{"id":1,"cmd":"ping"}\n`, `get_status`, `shutdown`).

---

## 5. Real-Time Safety Rules (`engine/`)

Every function reachable from the audio callback thread must obey strict real-time invariants:

1. **MMCSS & Denormals**: Audio threads enter the `"Pro Audio"` Multimedia Class Scheduler Service (`AvSetMmThreadCharacteristicsW`) and enable **FTZ/DAZ** (`_MM_SET_FLUSH_ZERO_MODE`, `_MM_SET_DENORMALS_ZERO_MODE`) via `RtSafety` RAII guards to prevent CPU spikes on near-zero float tails.
2. **Zero Allocations & Zero Locks**: No `new`/`malloc`, no `std::mutex`, no file/pipe I/O, and no exceptions on the audio thread.
3. **Lock-Free SPSC Ring Buffers**: Audio and meter data cross thread boundaries exclusively through cache-line-aligned (`alignas(64)`) power-of-two `SpscRingBuffer` queues using `std::atomic` acquire/release semantics.
4. **ZDF Filtering**: Parametric EQ and crossovers use the **Cytomic / Simper Zero-Delay Feedback State Variable Filter** (`SimperSvf`), providing unconditional numerical stability under fast parameter modulation.

---

## 6. Phase 2 Kernel Driver Hardening & Signing Plan

In Phase 1, AtrisWave operates **driverlessly** using Windows 10/11 WASAPI Process Loopback (`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`) and an optional signed virtual cable bridge. In **Phase 2**, `driver/` will introduce `AtrisWaveVAD.sys` (SysVAD / WaveRT PortCls):

- **Hardening**: All user-mode IOCTL buffers and shared ring-buffer pointers will be validated with `ProbeForRead`/`ProbeForWrite`, strict bounds checks on ring offsets, and continuous **WDK Driver Verifier** testing.
- **Signing & Distribution**: Windows 10/11 x64 requires kernel drivers to be signed via an **EV Code Signing Certificate** and **Microsoft Hardware Dev Center Attestation Signing** (or WHQL). Until signing infrastructure is in place for official releases, Phase 1 ensures full core functionality without requiring Test-Signing mode.
