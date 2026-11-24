# AtrisWave Plugin Host (`plugin-host/`)

> **Status:** Planned for **Phase 3** (Third-Party Audio Plugin Hosting).

This directory will house AtrisWave's out-of-process audio plugin host supporting **CLAP** and **VST3 (3.8+)** plugins.

## Planned Scope
- **Out-of-process sandboxing**: Isolate third-party plugin crashes, UI threads, and memory corruption from the core `AtrisWaveEngine.exe` real-time audio graph.
- **Lock-free audio & parameter transport**: Shared-memory audio buffers and lock-free SPSC parameter/MIDI event queues between the engine and the plugin host process.
- **Plugin scanning & crash-safe blacklisting**: Background scanner that validates plugin metadata without blocking the mixer UI.
- **License-clean SDK integration**: Native CLAP C-ABI support alongside VST3 3.8+ hosting.
