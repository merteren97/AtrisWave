# AtrisWave Virtual Audio Driver (`driver/`)

> **Status:** Planned for **Phase 2** (Kernel-Mode Driver).

This directory will house **`AtrisWaveVAD.sys`**, AtrisWave's open-source Windows Virtual Audio Driver based on Microsoft's **SysVAD / WaveRT** (PortCls) architecture.

## Planned Scope
- Expose multiple clean virtual playback and capture endpoints (*Game*, *Chat*, *Media*, *Aux*, *Processed Mic*, *Stream Mix*).
- Direct shared-memory ring buffer / IOCTL communication with the Session 1+ `AtrisWaveEngine.exe` daemon to eliminate double-buffering latency.
- Strict kernel-mode input validation (`ProbeForRead`/`ProbeForWrite`, bounds-checked ring offsets) and WDK Driver Verifier compliance.
- Production distribution via **EV Code Signing + Microsoft Hardware Dev Center Attestation Signing** (see [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md) for the Phase 1 -> Phase 2 signing roadmap).

> In **Phase 1**, AtrisWave operates driverlessly using Windows 10/11 **WASAPI Process Loopback** (`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`) and an optional signed virtual cable bridge.
