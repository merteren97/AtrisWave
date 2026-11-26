#include <atriswave/audio/audio_engine.hpp>
#include <atriswave/audio/com_utils.hpp>
#include <atriswave/ipc_protocol.h>
#include <atriswave/rt_safety.hpp>
#include <atriswave/simper_svf.hpp>
#include <atriswave/spsc_ring_buffer.hpp>
#include <atriswave/telemetry_publisher.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kEngineVersion = "0.1.1";
constexpr const wchar_t* kSingleInstanceMutexName = L"Local\\AtrisWaveEngineSingleInstance";
constexpr const wchar_t* kShutdownEventName = L"Local\\AtrisWaveEngineShutdown";
constexpr const wchar_t* kHiddenWindowClassName = L"AtrisWaveEngineHiddenSessionWindow";

HANDLE g_shutdown_event = nullptr;

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

LRESULT CALLBACK hidden_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept {
    switch (msg) {
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wparam != FALSE && g_shutdown_event != nullptr) {
            SetEvent(g_shutdown_event);
        }
        return 0;
    case WM_CLOSE:
        if (g_shutdown_event != nullptr) {
            SetEvent(g_shutdown_event);
        }
        return 0;
    case WM_DESTROY:
        if (g_shutdown_event != nullptr) {
            SetEvent(g_shutdown_event);
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

HWND create_hidden_session_window(HINSTANCE instance) noexcept {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = hidden_window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = kHiddenWindowClassName;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    return CreateWindowExW(
        0,
        kHiddenWindowClassName,
        L"AtrisWaveEngine",
        WS_OVERLAPPED,
        0, 0, 0, 0,
        nullptr,
        nullptr,
        instance,
        nullptr);
}

int stop_running_engine() noexcept {
    HANDLE evt = OpenEventW(EVENT_MODIFY_STATE, FALSE, kShutdownEventName);
    if (evt != nullptr) {
        SetEvent(evt);
        CloseHandle(evt);
    }

    HANDLE mtx = OpenMutexW(SYNCHRONIZE, FALSE, kSingleInstanceMutexName);
    if (mtx != nullptr) {
        (void)WaitForSingleObject(mtx, 3000);
        (void)ReleaseMutex(mtx);
        CloseHandle(mtx);
    }
    return 0;
}

int run_self_test() {
    atriswave::DenormalGuard denormal_guard;
    atriswave::MmcssGuard mmcss_guard(L"Pro Audio");

    // 1. SPSC ring buffer sanity check
    atriswave::SpscRingBuffer<float, 16> ring;
    for (int i = 0; i < 16; ++i) {
        if (!ring.try_push(static_cast<float>(i))) {
            std::cerr << "[self-test] FAIL: SpscRingBuffer push failed at " << i << "\n";
            return 1;
        }
    }
    for (int i = 0; i < 16; ++i) {
        float val = -1.0f;
        if (!ring.try_pop(val) || val != static_cast<float>(i)) {
            std::cerr << "[self-test] FAIL: SpscRingBuffer pop mismatch at " << i << "\n";
            return 1;
        }
    }

    // 2. SimperSvf stability & 0 dB passthrough check
    atriswave::SimperSvf svf;
    svf.set_parameters(atriswave::SimperSvf::Mode::Peak, 48000.0f, 1000.0f, 1.0f, 0.0f);
    for (int i = 0; i < 128; ++i) {
        const float in = std::sin(0.1f * static_cast<float>(i));
        const float out = svf.process_sample(in);
        if (!std::isfinite(out) || std::fabs(out - in) > 1e-5f) {
            std::cerr << "[self-test] FAIL: SimperSvf passthrough mismatch\n";
            return 1;
        }
    }

    // 3. TelemetryPublisher check (isolated self-test mapping)
    atriswave::TelemetryPublisher telemetry;
    if (!telemetry.open(L"Local\\AtrisWaveTelemetrySelfTest")) {
        std::cerr << "[self-test] FAIL: TelemetryPublisher open failed\n";
        return 1;
    }
    TelemetrySnapshot snap{};
    snap.header.channel_count = 6;
    snap.header.sample_rate = 48000;
    snap.header.buffer_frames = 128;
    snap.header.engine_state = ATRISWAVE_ENGINE_STATE_RUNNING;
    snap.header.timestamp_qpc = atriswave::TelemetryPublisher::query_qpc();
    telemetry.publish(snap);
    telemetry.close();

    std::cout << "[self-test] PASS (mmcss=" << (mmcss_guard.is_active() ? "active" : "unavailable")
              << ", abi_size=" << sizeof(TelemetrySnapshot) << ")\n";
    return 0;
}

int run_engine_main(HINSTANCE instance, const std::vector<std::string>& args) {
    bool headless_test = false;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg(args[i]);
        if (arg == "--version") {
            attach_parent_console_if_needed();
            std::cout << "AtrisWaveEngine " << kEngineVersion << "\n";
            return 0;
        }
        if (arg == "--help") {
            attach_parent_console_if_needed();
            std::cout << "AtrisWaveEngine [--version] [--self-test] [--headless-test] [--stop] [--background|--autostart]\n";
            return 0;
        }
        if (arg == "--self-test") {
            attach_parent_console_if_needed();
            return run_self_test();
        }
        if (arg == "--stop") {
            attach_parent_console_if_needed();
            return stop_running_engine();
        }
        if (arg == "--headless-test") {
            headless_test = true;
        }
    }

    HANDLE single_instance_mutex = nullptr;
    if (!headless_test) {
        single_instance_mutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
        if (single_instance_mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(single_instance_mutex);
            return 0;
        }
    }

    HANDLE shutdown_event = CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        headless_test ? nullptr : kShutdownEventName);
    if (shutdown_event == nullptr) {
        if (single_instance_mutex != nullptr) {
            ReleaseMutex(single_instance_mutex);
            CloseHandle(single_instance_mutex);
        }
        return 1;
    }
    ResetEvent(shutdown_event);
    g_shutdown_event = shutdown_event;

    HWND hidden_hwnd = headless_test ? nullptr : create_hidden_session_window(instance);

    auto engine = std::make_unique<atriswave::AudioEngine>();
    if (!engine->start(shutdown_event, headless_test)) {
        if (hidden_hwnd != nullptr) {
            DestroyWindow(hidden_hwnd);
        }
        g_shutdown_event = nullptr;
        CloseHandle(shutdown_event);
        if (single_instance_mutex != nullptr) {
            ReleaseMutex(single_instance_mutex);
            CloseHandle(single_instance_mutex);
        }
        return 1;
    }

    while (true) {
        const DWORD wait_rc = MsgWaitForMultipleObjects(1, &shutdown_event, FALSE, 16, QS_ALLINPUT);
        if (wait_rc == WAIT_OBJECT_0) {
            break;
        }
        if (wait_rc == WAIT_OBJECT_0 + 1) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    SetEvent(shutdown_event);
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0) {
                break;
            }
        }
        engine->publish_telemetry_tick();
    }

    engine->stop();
    if (hidden_hwnd != nullptr) {
        DestroyWindow(hidden_hwnd);
    }
    g_shutdown_event = nullptr;
    CloseHandle(shutdown_event);
    if (single_instance_mutex != nullptr) {
        ReleaseMutex(single_instance_mutex);
        CloseHandle(single_instance_mutex);
    }

    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    if (argv_w != nullptr && argc > 0) {
        args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            args.push_back(atriswave::audio::wide_to_utf8(argv_w[i]));
        }
        LocalFree(argv_w);
    } else {
        args.emplace_back("AtrisWaveEngine.exe");
    }

    return run_engine_main(instance, args);
}
