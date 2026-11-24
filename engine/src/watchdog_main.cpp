#include <atriswave/audio/failsafe_watchdog.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

HANDLE g_shutdown_event = nullptr;

BOOL WINAPI watchdog_ctrl_handler(DWORD ctrl_type) noexcept {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_shutdown_event != nullptr) {
            SetEvent(g_shutdown_event);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    DWORD watch_pid = 0;
    std::filesystem::path snapshot_path;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--watch-pid" && (i + 1) < argc) {
            watch_pid = static_cast<DWORD>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--snapshot" && (i + 1) < argc) {
            snapshot_path = argv[++i];
        } else if (arg == "--version") {
            std::cout << "AtrisWaveWatchdog 2.1.0-alpha\n";
            return 0;
        }
    }

    if (watch_pid == 0) {
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

    HANDLE shutdown_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_shutdown_event = shutdown_evt;
    SetConsoleCtrlHandler(watchdog_ctrl_handler, TRUE);

    HANDLE handles[2] = {proc, shutdown_evt};
    WaitForMultipleObjects(2, handles, FALSE, INFINITE);

    atriswave::audio::FailsafeWatchdog wd = snapshot_path.empty()
        ? atriswave::audio::FailsafeWatchdog()
        : atriswave::audio::FailsafeWatchdog(snapshot_path);
    (void)wd.restore_if_needed();

    SetConsoleCtrlHandler(watchdog_ctrl_handler, FALSE);
    g_shutdown_event = nullptr;
    if (shutdown_evt != nullptr) {
        CloseHandle(shutdown_evt);
    }
    CloseHandle(proc);
    return 0;
}
