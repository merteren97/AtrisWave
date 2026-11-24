## Summary

<!-- Describe what this PR changes and why. Reference any related issue (e.g., Closes #123). -->

## Component(s) Touched

- [ ] `engine/` (C++20 Real-Time Audio Engine)
- [ ] `shared/` (C-ABI IPC & Telemetry Contract)
- [ ] `ui/` (.NET 10 WPF Desktop UI & Core)
- [ ] `driver/` (Windows Virtual Audio Driver)
- [ ] `docs/` / Root / CI

## Verification Checklist

- [ ] `engine/` builds cleanly under `/W4 /permissive-` with zero warnings and `ctest` passes.
- [ ] `ui/AtrisWave.sln` builds cleanly in `Release` and `dotnet test` passes.
- [ ] Real-time audio threads contain **zero** heap allocations, locks, or blocking I/O.
- [ ] If `shared/include/atriswave/ipc_protocol.h` changed, C# `TelemetryContracts.cs` and ABI size/offset tests were updated to match.
- [ ] If UI strings were added/modified, `Strings.en.xaml` and `Strings.tr.xaml` maintain 100% key parity.
