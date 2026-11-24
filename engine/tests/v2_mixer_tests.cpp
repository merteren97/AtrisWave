#include <atriswave/audio/audio_engine.hpp>
#include <atriswave/audio/failsafe_watchdog.hpp>
#include <atriswave/audio/wasapi_devices.hpp>
#include <atriswave/dsp/dynamic_mixer.hpp>
#include <atriswave/ipc_protocol.h>
#include <atriswave/telemetry_publisher.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

namespace {

int g_failures = 0;

#define ATRIS_CHECK(cond, msg)                                                      \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " " << (msg)   \
                      << "\n";                                                      \
            ++g_failures;                                                           \
        }                                                                           \
    } while (false)

void test_telemetry_v2_layout_and_roundtrip() {
    ATRIS_CHECK(sizeof(TelemetrySnapshot) == 704, "Telemetry V2 size == 704 bytes");
    ATRIS_CHECK(offsetof(TelemetryHeader, bus_count) == 48, "bus_count offset == 48");
    ATRIS_CHECK(offsetof(TelemetrySnapshot, buses) == 576, "buses offset == 576");
    ATRIS_CHECK(ATRISWAVE_TELEMETRY_VERSION == 2u, "Telemetry version == 2");

    const wchar_t* shm = L"Local\\AtrisWaveTelemetryV2UnitTest";
    atriswave::TelemetryPublisher pub;
    ATRIS_CHECK(pub.open(shm), "TelemetryPublisher V2 open succeeds");

    TelemetrySnapshot tx{};
    tx.header.channel_count = 3;
    tx.header.bus_count = 4;
    tx.header.sample_rate = 48000;
    tx.header.buffer_frames = 480;
    tx.header.engine_state = ATRISWAVE_ENGINE_STATE_RUNNING;
    tx.channels[0].channel_id = 0;
    tx.channels[0].peak_left = 0.65f;
    tx.buses[0].channel_id = 0;
    tx.buses[0].peak_left = 0.55f;
    tx.buses[3].channel_id = 3;
    tx.buses[3].peak_right = 0.42f;
    pub.publish(tx);

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, shm);
    ATRIS_CHECK(mapping != nullptr, "OpenFileMappingW V2 succeeds");
    if (mapping != nullptr) {
        const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(TelemetrySnapshot));
        ATRIS_CHECK(view != nullptr, "MapViewOfFile V2 succeeds");
        if (view != nullptr) {
            const auto* shared = static_cast<const TelemetrySnapshot*>(view);
            TelemetrySnapshot rx{};
            std::memcpy(&rx, shared, sizeof(TelemetrySnapshot));
            ATRIS_CHECK(rx.header.version == 2u, "V2 version == 2");
            ATRIS_CHECK(rx.header.bus_count == 4u, "V2 bus_count == 4");
            ATRIS_CHECK(std::fabs(rx.buses[0].peak_left - 0.55f) < 1e-6f, "Bus A1 peak_left matches");
            ATRIS_CHECK(std::fabs(rx.buses[3].peak_right - 0.42f) < 1e-6f, "Bus B2 peak_right matches");
            UnmapViewOfFile(view);
        }
        CloseHandle(mapping);
    }
    pub.close();
}

void test_dynamic_mixer_bus_routing_limiter_and_ducking() {
    atriswave::dsp::DynamicMixer mixer;
    mixer.prepare(48000.0);

    // Channel 0: hot +6 dBFS sine -> Bus A1 (0)
    // Channel 1: steady tone -> Bus B1 (2), also ducks Channel 0 when active
    mixer.set_channel_active(0, true);
    mixer.set_channel_active(1, true);
    mixer.set_channel_mix(0, 1.0f, false, false, 0.0f);
    mixer.set_channel_mix(1, 1.0f, false, false, 0.0f);
    mixer.set_channel_send(0, 0, true, 1.0f);  // Ch0 -> A1
    mixer.set_channel_send(1, 2, true, 1.0f);  // Ch1 -> B1

    constexpr size_t kN = 512;
    std::vector<float> ch0_in(kN, 0.0f);
    std::vector<float> ch1_in(kN, 0.0f);
    for (size_t i = 0; i < kN; ++i) {
        ch0_in[i] = 2.0f * std::sin(0.1f * static_cast<float>(i)); // +6 dBFS
        ch1_in[i] = 0.5f * std::sin(0.05f * static_cast<float>(i));
    }

    std::array<const float*, atriswave::dsp::kDynamicMaxChannels> in_l{};
    std::array<const float*, atriswave::dsp::kDynamicMaxChannels> in_r{};
    in_l[0] = ch0_in.data();
    in_r[0] = ch0_in.data();
    in_l[1] = ch1_in.data();
    in_r[1] = ch1_in.data();

    std::array<std::vector<float>, 4> out_l_buf, out_r_buf;
    std::array<float*, 4> out_l{}, out_r{};
    for (size_t b = 0; b < 4; ++b) {
        out_l_buf[b].assign(kN, 0.0f);
        out_r_buf[b].assign(kN, 0.0f);
        out_l[b] = out_l_buf[b].data();
        out_r[b] = out_r_buf[b].data();
    }

    mixer.process(in_l, in_r, out_l, out_r, kN);

    // Verify -0.5 dBFS bus safety limiter ceiling (~0.9441)
    const float max_allowed = atriswave::dsp::db_to_linear(-0.5f) + 1e-3f;
    float a1_peak = 0.0f;
    float a2_peak = 0.0f;
    for (size_t i = 0; i < kN; ++i) {
        a1_peak = std::max(a1_peak, std::abs(out_l_buf[0][i]));
        a2_peak = std::max(a2_peak, std::abs(out_l_buf[1][i]));
    }
    ATRIS_CHECK(a1_peak <= max_allowed && a1_peak > 0.1f, "Bus A1 limited safely below -0.5 dBFS");
    ATRIS_CHECK(a2_peak == 0.0f, "Bus A2 remains silent when no channel sends to A2");

    // Enable cross-channel sidechain ducking: Ch1 ducks Ch0
    mixer.set_channel_dsp(0, false, 30.0f, 0.0f, 500.0f, 0.0f, 2500.0f, 0.0f, 0.0f,
                          false, -18.0f, 3.0f, 0.0f, 1, -18.0f);
    for (size_t i = 0; i < kN; ++i) {
        ch0_in[i] = 0.5f * std::sin(0.1f * static_cast<float>(i));
    }
    for (int block = 0; block < 4; ++block) {
        mixer.process(in_l, in_r, out_l, out_r, kN);
    }

    std::array<ChannelMeterFrame, ATRISWAVE_MAX_CHANNELS> ch_meters{};
    std::array<ChannelMeterFrame, ATRISWAVE_MAX_BUSES> bus_meters{};
    uint32_t active_cnt = 0;
    mixer.fill_telemetry(ch_meters.data(), active_cnt, bus_meters.data());
    ATRIS_CHECK(active_cnt == 2u, "2 active channels reported");
    ATRIS_CHECK(ch_meters[0].gain_reduction_db > 1.0f, "Ch0 sidechain ducking GR > 1 dB");
}

void test_engine_v2_channels_feedback_and_echo_lock() {
    auto engine_ptr = std::make_unique<atriswave::AudioEngine>();
    auto& engine = *engine_ptr;
    HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ATRIS_CHECK(engine.start(evt, true), "AudioEngine headless v2 start succeeds");

    // 1. Default slots: System (0) and Microphone (1)
    const auto initial_channels = engine.list_channels();
    ATRIS_CHECK(initial_channels.size() == 2, "Default 2 channels (System + Microphone)");
    ATRIS_CHECK(!engine.delete_channel(0).ok, "Cannot delete System slot 0");

    // 2. Create, rename, delete dynamic channel
    const auto created = engine.create_channel("Discord", "app_group", "#5865F2");
    ATRIS_CHECK(created.ok && created.channel_id >= 2, "Created dynamic Discord channel");
    ATRIS_CHECK(engine.rename_channel(created.channel_id, "Voice Chat").ok, "Renamed channel");
    ATRIS_CHECK(engine.bind_app_to_channel("Discord.exe", created.channel_id).ok, "Bound Discord.exe");

    // 3. ProcessLoopback A1/A2 echo lock
    const auto loopback_ch = engine.create_channel("Game Capture", "process_loopback", "#F59E0B");
    ATRIS_CHECK(loopback_ch.ok, "Created ProcessLoopback channel");
    const auto lock_a1 = engine.set_channel_send(loopback_ch.channel_id, "A1", true, 1.0f);
    ATRIS_CHECK(!lock_a1.ok && lock_a1.error == "echo_lock_process_loopback",
                "ProcessLoopback A1 send rejected with echo_lock_process_loopback");
    const auto lock_a2 = engine.set_channel_send(loopback_ch.channel_id, "A2", true, 1.0f);
    ATRIS_CHECK(!lock_a2.ok && lock_a2.error == "echo_lock_process_loopback",
                "ProcessLoopback A2 send rejected with echo_lock_process_loopback");
    ATRIS_CHECK(engine.set_channel_send(loopback_ch.channel_id, "B2", true, 1.0f).ok,
                "ProcessLoopback B2 Stream Mix allowed");

    // 4. Direct ID feedback loop rejection
    const auto bus_bind = engine.bind_output_bus("A1", "{mock-device-id}", 1.0f, false);
    ATRIS_CHECK(bus_bind.ok, "Bound A1 to mock device");
    const auto loop_src = engine.bind_channel_source(created.channel_id, "hardware_input", "{mock-device-id}");
    // Note: created channel has send_a1 == true, so binding its source to {mock-device-id} must reject!
    ATRIS_CHECK(!loop_src.ok && loop_src.error == "feedback_loop_detected",
                "Direct device ID feedback loop rejected");

    // 5. Virtual pair group detection check
    ATRIS_CHECK(atriswave::audio::WasapiDeviceEnumerator::detect_virtual_pair_group(
                    "CABLE Input (VB-Audio Virtual Cable)") == "vb-cable-0",
                "VB-Cable Input pair group detected");
    ATRIS_CHECK(atriswave::audio::WasapiDeviceEnumerator::detect_virtual_pair_group(
                    "CABLE Output (VB-Audio Virtual Cable)") == "vb-cable-0",
                "VB-Cable Output pair group matches Input");
    ATRIS_CHECK(atriswave::audio::WasapiDeviceEnumerator::detect_virtual_pair_group(
                    "AtrisWave System") == "atriswave-vad-0",
                "AtrisWave System pair group detected");

    ATRIS_CHECK(engine.delete_channel(created.channel_id).ok, "Deleted dynamic channel");
    engine.stop();
    CloseHandle(evt);
}

void test_failsafe_watchdog_roundtrip() {
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "atriswave_failsafe_test.json";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    atriswave::audio::FailsafeWatchdog wd(tmp);
    atriswave::audio::FailsafeDefaultsSnapshot snap{};
    snap.armed = true;
    snap.render_multimedia_id = "{0.0.0.00000000}.{phys-speaker}";
    snap.capture_multimedia_id = "{0.0.1.00000000}.{phys-mic}";

    ATRIS_CHECK(wd.arm_snapshot(snap), "Failsafe arm_snapshot succeeds");
    const auto loaded = wd.load_snapshot();
    ATRIS_CHECK(loaded.has_value() && loaded->armed, "Loaded armed snapshot");
    if (loaded.has_value()) {
        ATRIS_CHECK(loaded->render_multimedia_id == snap.render_multimedia_id,
                    "Render default ID preserved");
        ATRIS_CHECK(loaded->capture_multimedia_id == snap.capture_multimedia_id,
                    "Capture default ID preserved");
    }
    ATRIS_CHECK(wd.disarm(), "Disarm removes snapshot");
    ATRIS_CHECK(!wd.load_snapshot().has_value(), "Snapshot gone after disarm");

    std::filesystem::remove(tmp, ec);
}

} // namespace

int main() {
    test_telemetry_v2_layout_and_roundtrip();
    test_dynamic_mixer_bus_routing_limiter_and_ducking();
    test_engine_v2_channels_feedback_and_echo_lock();
    test_failsafe_watchdog_roundtrip();

    if (g_failures == 0) {
        std::cout << "[atriswave_v2_mixer_tests] ALL TESTS PASSED\n";
        return 0;
    }

    std::cerr << "[atriswave_v2_mixer_tests] " << g_failures << " FAILURE(S)\n";
    return 1;
}
