# AtrisWave Plugin Host (`plugin-host/`)

Out-of-process audio plugin sandbox for **AtrisWave** (Phase 3), isolating third-party DSP plugins from the core real-time audio engine via Win32 Shared Memory and auto-reset Events.

## Architecture
- **Shared IPC Contract (`../shared/include/atriswave/plugin_ipc.h`)**:
  - Fixed-size `#pragma pack(push, 4)` zero-pointer shared memory layout (`AtrisWavePluginSlotLayout`, magic `0x41575048` `"AWPH"`, ABI v1, `max_frames = 2048`).
  - Minimal C ABI (`AtrisWavePluginApi`, exported via `atriswave_plugin_get_api`) plus export discovery helpers for CLAP (`clap_entry`) and VST3 (`GetPluginFactory`).
- **Host Process (`AtrisWavePluginHost.exe`, `src/host_main.cpp`)**:
  - Attaches to `Local\<slot>`, `Local\<slot>_req`, and `Local\<slot>_done`.
  - Registers the audio thread with Windows MMCSS (`Pro Audio`) and enables SSE/SSE2 FTZ+DAZ.
  - Executes `Process`, `SetParam`, `Shutdown`, and deterministic `SimulateCrash` commands.
- **Sandbox Client (`include/atriswave/plugin_sandbox_client.hpp`)**:
  - Spawns `AtrisWavePluginHost.exe` (`CREATE_NO_WINDOW`) with current-user SDDL security on IPC handles.
  - On child crash or timeout, engages a click-free 5 ms (240 samples @ 48 kHz) crossfade to dry bypass and increments `crash_count()`.
- **Sample Plugin (`atriswave_sample_gain_plugin.dll`, `src/sample_gain_plugin.cpp`)**:
  - Stereo float32 linear gain C-ABI plugin (parameter `0` = gain, default `0.5`).

## Build & Test (VS 2026 Community)

```bat
cmd /c "\"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat\" && cd /d D:\Mert\Mert\Projects\MCorp\NEW_PRODUCT\AtrisWave\plugin-host && \"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe\" --preset msvc-release && \"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe\" --build --preset msvc-release && \"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe\" --preset msvc-release --output-on-failure"
```
