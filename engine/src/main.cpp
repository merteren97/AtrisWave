#include <atriswave/control_server.hpp>
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

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

constexpr const char* kEngineVersion = "0.1.0";
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
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--version") {
            std::cout << "AtrisWaveEngine " << kEngineVersion << "\n";
            return 0;
        }
        if (arg == "--self-test") {
            return run_self_test();
        }
    }

    HANDLE shutdown_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (shutdown_event == nullptr) {
        std::cerr << "Failed to create shutdown event\n";
        return 1;
    }
    g_shutdown_event = shutdown_event;
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    atriswave::TelemetryPublisher telemetry;
    if (!telemetry.open(ATRISWAVE_TELEMETRY_SHM_NAME_W)) {
        std::cerr << "Failed to open telemetry shared memory\n";
        CloseHandle(shutdown_event);
        return 1;
    }

    std::atomic<uint32_t> engine_state{ATRISWAVE_ENGINE_STATE_RUNNING};
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kBufferFrames = 128;
    constexpr uint32_t kChannelCount = 6;

    atriswave::ControlServer control_server;
    const bool server_started = control_server.start(
        ATRISWAVE_PIPE_NAME_W,
        [&]() {
            atriswave::ControlServer::StatusSnapshot status{};
            status.engine_state = engine_state.load(std::memory_order_acquire);
            status.sample_rate = kSampleRate;
            status.buffer_frames = kBufferFrames;
            status.channel_count = kChannelCount;
            return status;
        },
        [&]() {
            SetEvent(shutdown_event);
        });

    if (!server_started) {
        std::cerr << "Failed to start control named pipe server\n";
        telemetry.close();
        CloseHandle(shutdown_event);
        return 1;
    }

    TelemetrySnapshot snapshot{};
    snapshot.header.channel_count = kChannelCount;
    snapshot.header.sample_rate = kSampleRate;
    snapshot.header.buffer_frames = kBufferFrames;
    for (uint32_t ch = 0; ch < kChannelCount; ++ch) {
        snapshot.channels[ch].channel_id = ch;
        snapshot.channels[ch].flags = ATRISWAVE_CHANNEL_FLAG_ACTIVE;
    }

    while (WaitForSingleObject(shutdown_event, 16) == WAIT_TIMEOUT) {
        snapshot.header.engine_state = engine_state.load(std::memory_order_acquire);
        snapshot.header.timestamp_qpc = atriswave::TelemetryPublisher::query_qpc();
        telemetry.publish(snapshot);
    }

    engine_state.store(ATRISWAVE_ENGINE_STATE_STOPPED, std::memory_order_release);
    snapshot.header.engine_state = ATRISWAVE_ENGINE_STATE_STOPPED;
    snapshot.header.timestamp_qpc = atriswave::TelemetryPublisher::query_qpc();
    telemetry.publish(snapshot);

    control_server.stop();
    telemetry.close();
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    g_shutdown_event = nullptr;
    CloseHandle(shutdown_event);

    return 0;
}
