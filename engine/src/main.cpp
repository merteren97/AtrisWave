#include <atriswave/audio/audio_engine.hpp>
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

#include <cmath>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

constexpr const char* kEngineVersion = "2.2.0-alpha";
HANDLE g_shutdown_event = nullptr;

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) noexcept {
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

} // namespace

int main(int argc, char* argv[]) {
    bool headless_test = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--version") {
            std::cout << "AtrisWaveEngine " << kEngineVersion << "\n";
            return 0;
        }
        if (arg == "--self-test") {
            return run_self_test();
        }
        if (arg == "--headless-test") {
            headless_test = true;
        }
    }

    HANDLE shutdown_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (shutdown_event == nullptr) {
        std::cerr << "Failed to create shutdown event\n";
        return 1;
    }
    g_shutdown_event = shutdown_event;
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    auto engine = std::make_unique<atriswave::AudioEngine>();
    if (!engine->start(shutdown_event, headless_test)) {
        std::cerr << "Failed to start AtrisWave AudioEngine\n";
        SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
        g_shutdown_event = nullptr;
        CloseHandle(shutdown_event);
        return 1;
    }

    while (WaitForSingleObject(shutdown_event, 16) == WAIT_TIMEOUT) {
        engine->publish_telemetry_tick();
    }

    engine->stop();
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    g_shutdown_event = nullptr;
    CloseHandle(shutdown_event);

    return 0;
}
