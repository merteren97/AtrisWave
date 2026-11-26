#include <atriswave/audio/config_store.hpp>
#include <atriswave/audio/wasapi_devices.hpp>
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
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
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

void test_c_abi_layout() {
    ATRIS_CHECK(sizeof(ChannelMeterFrame) == 32, "ChannelMeterFrame size == 32");
    ATRIS_CHECK(sizeof(TelemetryHeader) == 64, "TelemetryHeader size == 64");
    ATRIS_CHECK(sizeof(TelemetrySnapshot) == 832, "TelemetrySnapshot size == 832");
    ATRIS_CHECK(offsetof(TelemetryHeader, sequence) == 24, "sequence offset == 24");
    ATRIS_CHECK(offsetof(TelemetryHeader, timestamp_qpc) == 32, "timestamp_qpc offset == 32");
    ATRIS_CHECK(offsetof(TelemetryHeader, bus_count) == 48, "bus_count offset == 48");
    ATRIS_CHECK(offsetof(TelemetryHeader, spectrum_bins) == 52, "spectrum_bins offset == 52");
    ATRIS_CHECK(offsetof(TelemetrySnapshot, channels) == 64, "channels offset == 64");
    ATRIS_CHECK(offsetof(TelemetrySnapshot, buses) == 576, "buses offset == 576");
    ATRIS_CHECK(offsetof(TelemetrySnapshot, spectrum_db) == 704, "spectrum_db offset == 704");
}

void test_spsc_ring_buffer() {
    atriswave::SpscRingBuffer<uint32_t, 8> ring;

    ATRIS_CHECK(ring.empty(), "Initially empty");
    ATRIS_CHECK(!ring.full(), "Initially not full");
    ATRIS_CHECK(ring.capacity() == 8, "Capacity is 8");

    uint32_t tmp = 0;
    ATRIS_CHECK(!ring.try_pop(tmp), "Pop on empty returns false");

    // Fill to capacity
    for (uint32_t i = 0; i < 8; ++i) {
        ATRIS_CHECK(ring.try_push(100 + i), "Push within capacity succeeds");
    }
    ATRIS_CHECK(ring.full(), "Full after 8 pushes");
    ATRIS_CHECK(!ring.try_push(999), "Push when full returns false");

    // Pop 4 and push 4 to test wrap-around
    for (uint32_t i = 0; i < 4; ++i) {
        ATRIS_CHECK(ring.try_pop(tmp), "Pop succeeds");
        ATRIS_CHECK(tmp == 100 + i, "FIFO order on first 4");
    }
    for (uint32_t i = 0; i < 4; ++i) {
        ATRIS_CHECK(ring.try_push(200 + i), "Push wrap-around succeeds");
    }
    ATRIS_CHECK(ring.full(), "Full again after wrap-around");

    // Drain all 8 and verify FIFO ordering across wrap-around
    const uint32_t expected[8] = {104, 105, 106, 107, 200, 201, 202, 203};
    for (uint32_t i = 0; i < 8; ++i) {
        ATRIS_CHECK(ring.try_pop(tmp), "Drain pop succeeds");
        ATRIS_CHECK(tmp == expected[i], "Wrap-around FIFO value matches");
    }
    ATRIS_CHECK(ring.empty(), "Empty after draining");
}

void test_simper_svf() {
    atriswave::DenormalGuard denormal_guard;
    atriswave::SimperSvf svf;

    // 1. Flat 0 dB gain = passthrough for Peak, LowShelf, HighShelf
    const atriswave::SimperSvf::Mode eq_modes[] = {
        atriswave::SimperSvf::Mode::Peak,
        atriswave::SimperSvf::Mode::LowShelf,
        atriswave::SimperSvf::Mode::HighShelf
    };

    for (auto mode : eq_modes) {
        svf.reset();
        svf.set_parameters(mode, 48000.0f, 1500.0f, 1.0f, 0.0f);
        for (int i = 0; i < 256; ++i) {
            const float x = std::sin(0.15f * static_cast<float>(i));
            const float y = svf.process_sample(x);
            ATRIS_CHECK(std::isfinite(y), "0dB EQ output finite");
            ATRIS_CHECK(std::fabs(y - x) < 1e-5f, "0dB EQ is passthrough");
        }
    }

    // 2. HighPass attenuates DC (steady 1.0f input decays to ~0)
    svf.reset();
    svf.set_parameters(atriswave::SimperSvf::Mode::HighPass, 48000.0f, 500.0f, 0.70710678f, 0.0f);
    float hp_out = 0.0f;
    for (int i = 0; i < 4096; ++i) {
        hp_out = svf.process_sample(1.0f);
        ATRIS_CHECK(std::isfinite(hp_out), "HPF output finite");
    }
    ATRIS_CHECK(std::fabs(hp_out) < 1e-4f, "HPF attenuates DC");

    // 3. LowPass passes DC (steady 1.0f input converges to ~1.0)
    svf.reset();
    svf.set_parameters(atriswave::SimperSvf::Mode::LowPass, 48000.0f, 1000.0f, 0.70710678f, 0.0f);
    float lp_out = 0.0f;
    for (int i = 0; i < 4096; ++i) {
        lp_out = svf.process_sample(1.0f);
        ATRIS_CHECK(std::isfinite(lp_out), "LPF output finite");
    }
    ATRIS_CHECK(std::fabs(lp_out - 1.0f) < 1e-4f, "LPF passes DC");

    // 4. BandPass & boosted Peak stability under extreme clamped params
    svf.reset();
    svf.set_parameters(atriswave::SimperSvf::Mode::BandPass, 48000.0f, 30000.0f, 50.0f, 24.0f);
    for (int i = 0; i < 512; ++i) {
        const float y = svf.process_sample((i % 2 == 0) ? 0.5f : -0.5f);
        ATRIS_CHECK(std::isfinite(y), "Extreme clamped SVF remains finite");
    }

    // 5. NaN input recovery
    const float nan_val = std::nanf("");
    const float recovered = svf.process_sample(nan_val);
    ATRIS_CHECK(recovered == 0.0f, "NaN input resets cleanly to 0");
}

void test_telemetry_roundtrip() {
    const wchar_t* test_shm = L"Local\\AtrisWaveTelemetryUnitTestV1";

    atriswave::TelemetryPublisher publisher;
    ATRIS_CHECK(publisher.open(test_shm), "TelemetryPublisher open succeeds");

    TelemetrySnapshot tx{};
    tx.header.channel_count = 6;
    tx.header.sample_rate = 48000;
    tx.header.buffer_frames = 128;
    tx.header.engine_state = ATRISWAVE_ENGINE_STATE_RUNNING;
    tx.header.timestamp_qpc = atriswave::TelemetryPublisher::query_qpc();
    tx.header.cpu_load_percent = 3.25f;
    tx.header.xrun_count = 1;

    tx.channels[0].channel_id = 0;
    tx.channels[0].peak_left = 0.85f;
    tx.channels[0].peak_right = 0.75f;
    tx.channels[0].rms_left = 0.40f;
    tx.channels[0].rms_right = 0.35f;
    tx.channels[0].gain_reduction_db = -2.5f;
    tx.channels[0].flags = ATRISWAVE_CHANNEL_FLAG_ACTIVE;

    publisher.publish(tx);

    // Reader side via Win32 OpenFileMappingW + seqlock read
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, test_shm);
    ATRIS_CHECK(mapping != nullptr, "OpenFileMappingW succeeds");
    if (mapping != nullptr) {
        const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(TelemetrySnapshot));
        ATRIS_CHECK(view != nullptr, "MapViewOfFile succeeds");
        if (view != nullptr) {
            const auto* shared = static_cast<const TelemetrySnapshot*>(view);
            TelemetrySnapshot rx{};
            bool stable = false;
            for (int attempt = 0; attempt < 16; ++attempt) {
                const uint64_t seq1 =
                    std::atomic_ref<const uint64_t>(shared->header.sequence).load(std::memory_order_acquire);
                if ((seq1 & 1ULL) != 0ULL) {
                    continue;
                }
                std::memcpy(&rx, shared, sizeof(TelemetrySnapshot));
                std::atomic_thread_fence(std::memory_order_acquire);
                const uint64_t seq2 =
                    std::atomic_ref<const uint64_t>(shared->header.sequence).load(std::memory_order_acquire);
                if (seq1 == seq2) {
                    stable = true;
                    break;
                }
            }

            ATRIS_CHECK(stable, "Seqlock read stabilized");
            ATRIS_CHECK(rx.header.magic == ATRISWAVE_TELEMETRY_MAGIC, "Magic matches");
            ATRIS_CHECK(rx.header.version == ATRISWAVE_TELEMETRY_VERSION, "Version matches");
            ATRIS_CHECK((rx.header.sequence & 1ULL) == 0ULL && rx.header.sequence >= 2ULL,
                        "Sequence is even and >= 2");
            ATRIS_CHECK(rx.header.channel_count == 6, "Channel count matches");
            ATRIS_CHECK(rx.header.sample_rate == 48000, "Sample rate matches");
            ATRIS_CHECK(rx.header.buffer_frames == 128, "Buffer frames matches");
            ATRIS_CHECK(rx.header.engine_state == ATRISWAVE_ENGINE_STATE_RUNNING, "Engine state matches");
            ATRIS_CHECK(std::fabs(rx.channels[0].peak_left - 0.85f) < 1e-6f, "Channel 0 peak_left matches");
            ATRIS_CHECK(std::fabs(rx.channels[0].gain_reduction_db - (-2.5f)) < 1e-6f,
                        "Channel 0 gain_reduction_db matches");
            ATRIS_CHECK(rx.channels[0].flags == ATRISWAVE_CHANNEL_FLAG_ACTIVE, "Channel 0 flags match");

            UnmapViewOfFile(view);
        }
        CloseHandle(mapping);
    }

    publisher.close();
}

void test_control_server_roundtrip() {
    const wchar_t* test_pipe = L"\\\\.\\pipe\\AtrisWaveEngineUnitTest";
    std::atomic<bool> shutdown_called{false};

    atriswave::ControlServer server;
    const bool started = server.start(
        test_pipe,
        []() {
            atriswave::ControlServer::StatusSnapshot st{};
            st.engine_state = ATRISWAVE_ENGINE_STATE_RUNNING;
            st.sample_rate = 48000;
            st.buffer_frames = 128;
            st.channel_count = 6;
            return st;
        },
        [&]() { shutdown_called.store(true, std::memory_order_release); });

    ATRIS_CHECK(started, "ControlServer started");
    if (!started) {
        return;
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 50; ++i) {
        pipe = CreateFileW(test_pipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ATRIS_CHECK(pipe != INVALID_HANDLE_VALUE, "Connected to test named pipe");

    if (pipe != INVALID_HANDLE_VALUE) {
        auto send_cmd = [&](const std::string& req) -> std::string {
            DWORD written = 0;
            if (!WriteFile(pipe, req.data(), static_cast<DWORD>(req.size()), &written, nullptr)) {
                return {};
            }
            char buf[512]{};
            DWORD read = 0;
            if (!ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr)) {
                return {};
            }
            return std::string(buf, read);
        };

        const std::string ping_resp = send_cmd("{\"id\":1,\"cmd\":\"ping\"}\n");
        ATRIS_CHECK(ping_resp.find("\"ok\":true") != std::string::npos &&
                        ping_resp.find("\"pong\":true") != std::string::npos,
                    "ping response valid");

        const std::string status_resp = send_cmd("{\"id\":2,\"cmd\":\"get_status\"}\n");
        ATRIS_CHECK(status_resp.find("\"sample_rate\":48000") != std::string::npos &&
                        status_resp.find("\"channel_count\":6") != std::string::npos,
                    "get_status response valid");

        const std::string shutdown_resp = send_cmd("{\"id\":3,\"cmd\":\"shutdown\"}\n");
        ATRIS_CHECK(shutdown_resp.find("\"shutting_down\":true") != std::string::npos,
                    "shutdown response valid");

        CloseHandle(pipe);
    }

    for (int i = 0; i < 50 && !shutdown_called.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ATRIS_CHECK(shutdown_called.load(std::memory_order_acquire), "Shutdown callback invoked");

    server.stop();
}

void test_audio_helpers() {
    ATRIS_CHECK(atriswave::audio::WasapiDeviceEnumerator::is_virtual_device_name("CABLE Input (VB-Audio Virtual Cable)"),
                "VB-Cable Input detected as virtual");
    ATRIS_CHECK(atriswave::audio::WasapiDeviceEnumerator::is_virtual_device_name("AtrisWave Virtual Mic"),
                "AtrisWave detected as virtual");
    ATRIS_CHECK(!atriswave::audio::WasapiDeviceEnumerator::is_virtual_device_name("Realtek(R) Audio"),
                "Realtek is not virtual");

    ATRIS_CHECK(!atriswave::audio::ConfigStore::is_valid_preset_name(""), "Empty preset rejected");
    ATRIS_CHECK(!atriswave::audio::ConfigStore::is_valid_preset_name("../evil"), "Path traversal rejected");
    ATRIS_CHECK(!atriswave::audio::ConfigStore::is_valid_preset_name("sub/dir"), "Slash rejected");
    ATRIS_CHECK(atriswave::audio::ConfigStore::is_valid_preset_name("Studio_Podcast-1"), "Valid preset accepted");
}

void test_concurrent_pipe_and_unicode_json() {
    const wchar_t* test_pipe = L"\\\\.\\pipe\\AtrisWaveEngineConcurrentTest";
    std::string last_renamed;
    std::string last_bus_mix_id;
    float last_bus_mix_vol = 0.0f;

    class MockHandler final : public atriswave::ControlServer::IControlHandler {
    public:
        std::string& renamed_ref;
        std::string& bus_id_ref;
        float& bus_vol_ref;
        MockHandler(std::string& r, std::string& b, float& v)
            : renamed_ref(r), bus_id_ref(b), bus_vol_ref(v) {}

        atriswave::ControlServer::StatusSnapshot get_status() override { return {}; }
        void on_shutdown() override {}
        std::vector<atriswave::audio::AudioDeviceInfo> list_devices() override { return {}; }
        std::vector<atriswave::audio::AudioSessionInfo> list_sessions() override { return {}; }
        bool set_channel(const atriswave::ChannelConfig&) override { return true; }
        bool set_mic_dsp(const atriswave::MicDspConfig&) override { return true; }
        bool select_devices(const std::string&, const std::string&, const std::string&) override { return true; }
        atriswave::audio::RouteResult route_app(uint32_t, const std::string&, bool) override { return {}; }
        bool save_preset(const std::string&) override { return true; }
        std::optional<atriswave::MicDspConfig> load_preset(const std::string&) override { return std::nullopt; }
        std::vector<std::string> list_presets() override { return {}; }
        atriswave::audio::DriverStatus get_driver_status() override { return {}; }
        std::vector<atriswave::audio::RoutingRule> list_routing_rules() override { return {}; }
        atriswave::ControlServer::RoutingRuleApplyResult set_routing_rule(const atriswave::audio::RoutingRule&) override { return {}; }
        bool delete_routing_rule(const std::string&) override { return true; }
        uint32_t apply_routing_rules() override { return 0; }
        std::vector<atriswave::DynamicChannelInfo> list_channels() override { return {}; }
        atriswave::V2CommandResult create_channel(const std::string&, const std::string&, const std::string&) override { return {}; }
        atriswave::V2CommandResult delete_channel(uint32_t) override { return {}; }
        atriswave::V2CommandResult rename_channel(uint32_t, const std::string& name) override {
            renamed_ref = name;
            return {true, {}, 0};
        }
        atriswave::V2CommandResult bind_channel_source(uint32_t, const std::string&, const std::string&) override { return {}; }
        std::vector<atriswave::OutputBusInfo> list_output_buses() override { return {}; }
        atriswave::V2CommandResult bind_output_bus(const std::string&, const std::string&, float, bool) override { return {}; }
        atriswave::OutputBusMixResult set_output_bus_mix(const std::string& bus_id, float volume, bool muted) override {
            bus_id_ref = bus_id;
            bus_vol_ref = volume;
            return {true, bus_id, volume, muted, {}};
        }
        atriswave::V2CommandResult set_channel_send(uint32_t, const std::string&, bool, float) override { return {}; }
        atriswave::V2CommandResult set_channel_dsp(const atriswave::ChannelDspConfig&) override { return {}; }
        atriswave::V2CommandResult bind_app_to_channel(const std::string&, uint32_t) override { return {}; }
        atriswave::V2CommandResult unbind_app_from_channel(const std::string&, uint32_t) override { return {}; }
        atriswave::V2CommandResult auto_setup_defaults() override { return {}; }
        atriswave::SoundboardPlayResult play_soundboard(const std::string&, float, bool, bool, bool, bool) override { return {}; }
        bool stop_soundboard(int) override { return true; }
        atriswave::SoundboardStatus get_soundboard_status() override { return {}; }
        atriswave::V2CommandResult set_spectrum_source(uint32_t) override { return {}; }
    } handler(last_renamed, last_bus_mix_id, last_bus_mix_vol);

    atriswave::ControlServer server;
    ATRIS_CHECK(server.start(test_pipe, &handler), "Concurrent test ControlServer started");

    auto open_client = [&]() -> HANDLE {
        for (int i = 0; i < 50; ++i) {
            HANDLE h = CreateFileW(test_pipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) return h;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return INVALID_HANDLE_VALUE;
    };

    HANDLE c1 = open_client();
    HANDLE c2 = open_client();
    ATRIS_CHECK(c1 != INVALID_HANDLE_VALUE && c2 != INVALID_HANDLE_VALUE, "Two concurrent pipe clients connected");

    auto send_cmd = [&](HANDLE pipe, const std::string& req) -> std::string {
        DWORD written = 0;
        if (!WriteFile(pipe, req.data(), static_cast<DWORD>(req.size()), &written, nullptr)) return {};
        char buf[512]{};
        DWORD read = 0;
        if (!ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr)) return {};
        return std::string(buf, read);
    };

    if (c1 != INVALID_HANDLE_VALUE && c2 != INVALID_HANDLE_VALUE) {
        const std::string r2 = send_cmd(c2, "{\"id\":10,\"cmd\":\"set_output_bus_mix\",\"bus_id\":\"A1\",\"volume\":0.75,\"muted\":false}\n");
        ATRIS_CHECK(r2.find("\"ok\":true") != std::string::npos && last_bus_mix_id == "A1" && std::fabs(last_bus_mix_vol - 0.75f) < 1e-4f,
                    "set_output_bus_mix handled on second concurrent client");

        // Turkish "Kulaklık" = Kulakl\u0131k
        const std::string r1 = send_cmd(c1, "{\"id\":11,\"cmd\":\"rename_channel\",\"channel_id\":2,\"name\":\"Kulakl\\u0131k \\u015f\\u011f\"}\n");
        ATRIS_CHECK(r1.find("\"ok\":true") != std::string::npos, "Unicode rename_channel succeeded");
        const std::string expected_utf8 = "Kulakl\xC4\xB1k \xC5\x9F\xC4\x9F";
        ATRIS_CHECK(last_renamed == expected_utf8, "Decoded \\uXXXX Turkish UTF-8 matches");
    }

    if (c1 != INVALID_HANDLE_VALUE) CloseHandle(c1);
    if (c2 != INVALID_HANDLE_VALUE) CloseHandle(c2);
    server.stop();
}

} // namespace

int main() {
    test_c_abi_layout();
    test_spsc_ring_buffer();
    test_simper_svf();
    test_telemetry_roundtrip();
    test_control_server_roundtrip();
    test_audio_helpers();
    test_concurrent_pipe_and_unicode_json();

    if (g_failures == 0) {
        std::cout << "[atriswave_engine_tests] ALL TESTS PASSED\n";
        return 0;
    }

    std::cerr << "[atriswave_engine_tests] " << g_failures << " FAILURE(S)\n";
    return 1;
}
