# Security Policy

## Supported Versions

AtrisWave is currently in active early development (Phase 0 -> Phase 1). Security fixes are applied directly to the `main` branch and included in the latest tagged pre-release/release.

| Version / Branch | Supported          |
| ---------------- | ------------------ |
| `main`           | :white_check_mark: |

## Threat Model & High-Priority Surfaces

Because AtrisWave combines a low-latency C++ audio engine, local IPC, and (in Phase 2) a Windows kernel-mode virtual audio driver, we treat the following attack surfaces with strict scrutiny:

1. **Kernel-Mode Virtual Audio Driver (`driver/` — Phase 2 `AtrisWaveVAD.sys`)**:
   - **IOCTL & IRP Handling**: Improperly validated user-mode buffers, integer overflows in ring-buffer offsets, or missing `ProbeForRead`/`ProbeForWrite` checks can lead to kernel panics (BSOD) or local privilege escalation (LPE).
   - **Kernel/User Shared Memory**: Any memory mapped between kernel space and the user-mode engine must be strictly bounds-checked and never trust user-writable metadata without validation.
2. **Local Named Pipe IPC (`\\.\pipe\AtrisWaveEngine`)**:
   - The engine runs in the user's Session 1+ desktop context and restricts pipe access using an explicit current-user DACL (failing closed if the security descriptor cannot be constructed).
   - Any flaw that allows cross-session/cross-user command injection, buffer/JSON parser crashes, or unauthorized `shutdown`/device control is considered a security issue.
3. **Shared Memory Telemetry (`Local\AtrisWaveTelemetryV1`)**:
   - Designed for single-writer (engine) / multi-reader (UI) lock-free seqlock reads. Readers must handle malformed or torn data gracefully without out-of-bounds reads.
4. **Third-Party Plugin Hosting (`plugin-host/` — Phase 3 CLAP / VST3)**:
   - Out-of-process sandboxing is planned for third-party plugins to isolate crashes and memory corruption from the core audio engine.

## Reporting a Vulnerability

**Please do NOT open a public GitHub issue for security vulnerabilities.**

Instead, please report vulnerabilities privately via **GitHub Security Advisories** ("Report a vulnerability" under the **Security** tab of this repository).

When reporting, please include:
- A clear description of the vulnerability and affected component (`engine/`, `ui/`, `shared/`, or `driver/`).
- Steps to reproduce or a minimal proof-of-concept.
- Potential impact (e.g., local privilege escalation, denial of service / BSOD, memory corruption).

We aim to acknowledge reports within **72 hours** and provide a remediation plan or status update within **7 days**. Responsible disclosure is appreciated and will be credited in release notes upon fix (unless anonymity is requested).
