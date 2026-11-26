#include <atriswave/audio/com_utils.hpp>
#include <atriswave/audio/failsafe_watchdog.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void attach_parent_console_if_needed() noexcept {
    HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (std_out != nullptr && std_out != INVALID_HANDLE_VALUE && GetFileType(std_out) != FILE_TYPE_UNKNOWN) {
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f_out = nullptr;
        FILE* f_err = nullptr;
        (void)freopen_s(&f_out, "CONOUT$", "w", stdout);
        (void)freopen_s(&f_err, "CONOUT$", "w", stderr);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    if (argv_w != nullptr && argc > 0) {
        args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            args.push_back(atriswave::audio::wide_to_utf8(argv_w[i]));
        }
        LocalFree(argv_w);
    }

    DWORD watch_pid = 0;
    std::filesystem::path snapshot_path;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg(args[i]);
        if (arg == "--watch-pid" && (i + 1) < args.size()) {
            watch_pid = static_cast<DWORD>(std::strtoul(args[++i].c_str(), nullptr, 10));
        } else if (arg == "--snapshot" && (i + 1) < args.size()) {
            snapshot_path = args[++i];
        } else if (arg == "--version") {
            attach_parent_console_if_needed();
            std::cout << "AtrisWaveWatchdog 0.1.0\n";
            return 0;
        }
    }

    if (watch_pid == 0) {
        attach_parent_console_if_needed();
        std::cerr << "Usage: AtrisWaveWatchdog.exe --watch-pid <pid> [--snapshot <path>]\n";
        return 1;
    }

    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, watch_pid);
    if (proc == nullptr) {
        atriswave::audio::FailsafeWatchdog wd = snapshot_path.empty()
            ? atriswave::audio::FailsafeWatchdog()
            : atriswave::audio::FailsafeWatchdog(snapshot_path);
        (void)wd.restore_if_needed();
        return 0;
    }

    WaitForSingleObject(proc, INFINITE);

    atriswave::audio::FailsafeWatchdog wd = snapshot_path.empty()
        ? atriswave::audio::FailsafeWatchdog()
        : atriswave::audio::FailsafeWatchdog(snapshot_path);
    (void)wd.restore_if_needed();

    CloseHandle(proc);
    return 0;
}
