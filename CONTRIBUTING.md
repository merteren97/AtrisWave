# Contributing to AtrisWave

Thank you for your interest in contributing to **AtrisWave**! Every contribution—from bug reports and documentation improvements to real-time DSP modules and WPF UI enhancements—helps build a better open-source Windows audio suite.

Please read our [Code of Conduct](CODE_OF_CONDUCT.md) and [Architecture Reference](docs/ARCHITECTURE.md) before getting started.

## Development Environment

### Prerequisites
- **OS**: Windows 10 (2004 / Build 19041+) or Windows 11 (x64)
- **C++20 Engine**: Visual Studio 2022/2026 with **Desktop development with C++**, CMake 3.25+, Ninja
- **.NET UI**: [.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0) (`10.0.x`)

### Building & Testing Locally

**C++20 Audio Engine:**
```powershell
cmake --preset msvc-debug -S engine -B engine/build/debug
cmake --build engine/build/debug
ctest --test-dir engine/build/debug --output-on-failure
```

**C# .NET 10 WPF UI & Core:**
```powershell
dotnet restore ui/AtrisWave.sln
dotnet build ui/AtrisWave.sln --configuration Release
dotnet test ui/AtrisWave.sln --configuration Release
```

## Coding Standards

### C++20 (`engine/`, `shared/`, `driver/`)
- Follow `.clang-format` (4-space indent, 100-column limit, Allman braces).
- Compile cleanly with `/W4 /permissive-` (zero warnings).
- **Real-Time Audio Thread Rules**: Never allocate memory (`new`, `malloc`, `std::vector::push_back`), never acquire locks (`std::mutex`), never perform file/network/pipe I/O, and never throw exceptions on the audio callback thread.
- Keep `shared/include/atriswave/ipc_protocol.h` strictly C-ABI compatible with C# `StructLayout(LayoutKind.Sequential, Pack = 4)` and update `static_assert` checks if any struct changes.

### C# / WPF (`ui/`)
- Target `.NET 10` (`net10.0-windows` for UI, `net10.0` / `net10.0-windows` for Core/Tests).
- Use `Wpf.Ui` for theming; do **not** mix with `.NET 10 Application.ThemeMode`.
- Use `SkiaSharp.Views.WPF` (`SKElement`, never `SKGLControl`) for 60 FPS meters with zero per-frame allocations.
- Keep **100% key parity** between `Strings.en.xaml` and `Strings.tr.xaml` (enforced by unit tests).

## Pull Request Process

1. Fork the repository and create a feature branch from `main` (e.g., `feat/mic-compressor` or `fix/seqlock-retry`).
2. Ensure both `engine/` and `ui/` build and pass all tests locally.
3. Keep PRs focused and well-scoped; reference related issues in the description.
4. By submitting a contribution, you agree that your work is licensed under the [Apache License 2.0](LICENSE).
