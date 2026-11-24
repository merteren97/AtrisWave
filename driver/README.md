# AtrisWave Virtual Audio Driver (`driver/`)

`AtrisWaveVAD.sys` is AtrisWave's Phase 2 KMDF 1.33 / WaveRT-ACX virtual audio driver providing a **16-slot pre-provisioned dynamic endpoint pool** connected to `AtrisWaveEngine.exe` via hardened `METHOD_BUFFERED` IOCTLs and a lock-free shared ring buffer.

## Exposed Virtual Endpoints (`ROOT\AtrisWaveVAD`)
| ID | Endpoint Name | Direction | Default State | Purpose |
|---:|---|---|---|---|
| `0` | **AtrisWave System** | Render (Playback) | Connected | Default catch-all Windows playback channel |
| `1` | **AtrisWave Mic** | Capture (Recording) | Connected | Processed microphone output (`B1`) |
| `2` | **AtrisWave Stream Mix** | Capture (Recording) | Connected | Final OBS / streaming mix output (`B2`) |
| `3..15` | **AtrisWave Ch 4..16** | Render (Playback) | Unplugged (`IsConnected=0`) | Dynamic pooled channels toggled via `IOCTL_ATRISWAVE_SET_ENDPOINT_CONNECTED` |

## Directory Structure
- [AtrisWaveVAD.inf](AtrisWaveVAD.inf): Multi-endpoint Windows 10/11 (`x64` & `ARM64`) INF registering `ROOT\AtrisWaveVAD`, KMDF `1.33`, and `GUID_DEVINTERFACE_ATRISWAVE_VAD`.
- [AtrisWaveVAD.vcxproj](AtrisWaveVAD.vcxproj): WDK MSBuild project (`WindowsKernelModeDriver10.0`, `/W4 /WX`, C++20, Spectre mitigation, CFG, CET compat) referencing `..\shared\include`.
- [src/driver_internal.h](src/driver_internal.h): Internal KMDF device context, endpoint descriptors, and dispatch prototypes.
- [src/driver_entry.cpp](src/driver_entry.cpp): `DriverEntry` + `EvtDeviceAdd` with hardened SDDL (`D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`), symbolic link `\DosDevices\AtrisWaveVAD`, and device interface registration.
- [src/control_device.cpp](src/control_device.cpp): `EvtIoDeviceControl` handler for `IOCTL_ATRISWAVE_GET_VERSION`, `IOCTL_ATRISWAVE_GET_ENDPOINT_STATS`, `IOCTL_ATRISWAVE_MAP_ENDPOINT_RING`, and `IOCTL_ATRISWAVE_SET_ENDPOINT_FORMAT`.
- [src/virtual_endpoints.cpp](src/virtual_endpoints.cpp): Zero-DSP 6-endpoint descriptor table and `DISPATCH_LEVEL`-safe cyclic ring-buffer bridge using `ring_buffer_validate.h`.
- [scripts/build-and-testsign.ps1](scripts/build-and-testsign.ps1): Local WDK build, `inf2cat`, test-cert signing, and `pnputil` install helper.
- [scripts/package-attestation-cab.ps1](scripts/package-attestation-cab.ps1): `makecab` `.ddf` + EV `signtool` packager for Microsoft Partner Center Attestation Signing.
- [docs/SIGNING_AND_TEST_VM.md](docs/SIGNING_AND_TEST_VM.md): WDK, Hyper-V Test VM, WinDbg, Driver Verifier, and EV Attestation Signing runbook.

## Shared ABI Headers
- [../shared/include/atriswave/driver_ioctl.h](../shared/include/atriswave/driver_ioctl.h)
- [../shared/include/atriswave/ring_buffer_validate.h](../shared/include/atriswave/ring_buffer_validate.h)
