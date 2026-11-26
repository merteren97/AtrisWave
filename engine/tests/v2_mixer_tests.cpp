#include <atriswave/audio/audio_engine.hpp>
#include <atriswave/audio/failsafe_watchdog.hpp>
#include <atriswave/audio/wasapi_devices.hpp>
#include <atriswave/dsp/convolution_reverb.hpp>
#include <atriswave/dsp/dynamic_mixer.hpp>
#include <atriswave/dsp/soundboard.hpp>
#include <atriswave/dsp/spectrum_analyzer.hpp>
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
#include <fstream>
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

void test_telemetry_v3_layout_and_roundtrip() {
    ATRIS_CHECK(sizeof(TelemetrySnapshot) == 832, "Telemetry V3 size == 832 bytes");
    ATRIS_CHECK(offsetof(TelemetryHeader, bus_count) == 48, "bus_count offset == 48");
    ATRIS_CHECK(offsetof(TelemetryHeader, spectrum_bins) == 52, "spectrum_bins offset == 52");
    ATRIS_CHECK(offsetof(TelemetrySnapshot, buses) == 576, "buses offset == 576");
    ATRIS_CHECK(offsetof(TelemetrySnapshot, spectrum_db) == 704, "spectrum_db offset == 704");
    ATRIS_CHECK(ATRISWAVE_TELEMETRY_VERSION == 3u, "Telemetry version == 3");

    const wchar_t* shm = L"Local\\AtrisWaveTelemetryV3UnitTest";
    atriswave::TelemetryPublisher pub;
    ATRIS_CHECK(pub.open(shm), "TelemetryPublisher V3 open succeeds");

    TelemetrySnapshot tx{};
    tx.header.channel_count = 3;
    tx.header.bus_count = 4;
    tx.header.spectrum_bins = ATRISWAVE_SPECTRUM_BINS;
    tx.header.sample_rate = 48000;
    tx.header.buffer_frames = 480;
    tx.header.engine_state = ATRISWAVE_ENGINE_STATE_RUNNING;
    tx.channels[0].channel_id = 0;
    tx.channels[0].peak_left = 0.65f;
    tx.buses[0].channel_id = 0;
    tx.buses[0].peak_left = 0.55f;
    tx.buses[3].channel_id = 3;
    tx.buses[3].peak_right = 0.42f;
    tx.spectrum_db[15] = -12.5f;
    pub.publish(tx, tx.spectrum_db);

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, shm);
    ATRIS_CHECK(mapping != nullptr, "OpenFileMappingW V3 succeeds");
    if (mapping != nullptr) {
        const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(TelemetrySnapshot));
        ATRIS_CHECK(view != nullptr, "MapViewOfFile V3 succeeds");
        if (view != nullptr) {
            const auto* shared = static_cast<const TelemetrySnapshot*>(view);
            TelemetrySnapshot rx{};
            std::memcpy(&rx, shared, sizeof(TelemetrySnapshot));
            ATRIS_CHECK(rx.header.version == 3u, "V3 version == 3");
            ATRIS_CHECK(rx.header.bus_count == 4u, "V3 bus_count == 4");
            ATRIS_CHECK(rx.header.spectrum_bins == 32u, "V3 spectrum_bins == 32");
            ATRIS_CHECK(std::fabs(rx.buses[0].peak_left - 0.55f) < 1e-6f, "Bus A1 peak_left matches");
            ATRIS_CHECK(std::fabs(rx.buses[3].peak_right - 0.42f) < 1e-6f, "Bus B2 peak_right matches");
            ATRIS_CHECK(std::fabs(rx.spectrum_db[15] - (-12.5f)) < 1e-6f, "Spectrum bin 15 matches");
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

void test_spectrum_analyzer() {
    atriswave::dsp::SpectrumAnalyzer analyzer;
    analyzer.prepare(48000.0);

    constexpr size_t kN = 2048;
    std::vector<float> sine(kN, 0.0f);
    constexpr float kTwoPi = 6.283185307f;
    for (size_t i = 0; i < kN; ++i) {
        sine[i] = 0.5f * std::sin(kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
    }
    analyzer.push_block(sine.data(), sine.data(), kN);

    std::array<float, ATRISWAVE_SPECTRUM_BINS> bins{};
    analyzer.read_bins(bins.data(), bins.size());

    float max_bin_db = -90.0f;
    size_t max_idx = 0;
    for (size_t i = 0; i < bins.size(); ++i) {
        ATRIS_CHECK(std::isfinite(bins[i]) && bins[i] >= -90.0f && bins[i] <= 0.0f,
                    "Spectrum bin finite in [-90, 0]");
        if (bins[i] > max_bin_db) {
            max_bin_db = bins[i];
            max_idx = i;
        }
    }
    // 1 kHz is in the middle of 30 Hz .. 18 kHz log scale (around bins 12..22)
    ATRIS_CHECK(max_bin_db > -20.0f && max_idx >= 10 && max_idx <= 24,
                "1 kHz sine peaks in mid-band");
}

void test_soundboard_wav_decode_and_playback() {
    const std::filesystem::path wav_path =
        std::filesystem::temp_directory_path() / "atriswave_soundboard_test.wav";
    std::error_code ec;
    std::filesystem::remove(wav_path, ec);

    // Write a minimal 48 kHz mono PCM16 WAV (2400 frames = 50ms)
    constexpr uint32_t kFrames = 2400;
    constexpr uint32_t kDataBytes = kFrames * 2;
    {
        std::ofstream out(wav_path, std::ios::binary | std::ios::trunc);
        auto write_u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        auto write_u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        out.write("RIFF", 4);
        write_u32(36 + kDataBytes);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        write_u32(16);
        write_u16(1);     // PCM
        write_u16(1);     // Mono
        write_u32(48000); // SampleRate
        write_u32(96000); // ByteRate
        write_u16(2);     // BlockAlign
        write_u16(16);    // BitsPerSample
        out.write("data", 4);
        write_u32(kDataBytes);
        for (uint32_t i = 0; i < kFrames; ++i) {
            const int16_t sample = static_cast<int16_t>(16000.0 * std::sin(0.1 * i));
            write_u16(static_cast<uint16_t>(sample));
        }
    }

    atriswave::dsp::SoundboardEngine sb;
    atriswave::dsp::SoundboardEngine::PlayRequest req{};
    req.wav_path = wav_path;
    req.gain = 0.8f;
    req.send_a1 = true;
    req.send_b1 = true;
    req.send_b2 = true;
    const auto res = sb.play(req);
    ATRIS_CHECK(res.ok && res.slot == 0 && res.duration_ms == 50,
                "Soundboard decoded temp WAV cleanly");
    ATRIS_CHECK(sb.active_voices() == 1u, "1 active voice");

    std::vector<float> a1_l(480, 0.0f), a1_r(480, 0.0f);
    std::vector<float> b1_l(480, 0.0f), b1_r(480, 0.0f);
    std::vector<float> b2_l(480, 0.0f), b2_r(480, 0.0f);
    sb.mix_into_buses(a1_l.data(), a1_r.data(), b1_l.data(), b1_r.data(), b2_l.data(), b2_r.data(), 480);

    float peak = 0.0f;
    for (float v : a1_l) peak = std::max(peak, std::abs(v));
    ATRIS_CHECK(peak > 0.1f, "Soundboard mixed non-zero samples into A1");

    sb.stop(-1);
    for (int i = 0; i < 4; ++i) {
        sb.mix_into_buses(a1_l.data(), a1_r.data(), b1_l.data(), b1_r.data(), b2_l.data(), b2_r.data(), 480);
    }
    ATRIS_CHECK(sb.active_voices() == 0u, "Soundboard stopped cleanly after fade-out");

    std::filesystem::remove(wav_path, ec);
}

void test_44100_to_48000_to_44100_continuous_sine() {
    atriswave::dsp::AdaptivePllResampler cap_pll;
    atriswave::dsp::AdaptivePllResampler ren_pll;
    cap_pll.prepare(44100.0, 48000.0);
    ren_pll.prepare(48000.0, 44100.0);

    constexpr double kInRate = 44100.0;
    constexpr double kFreq = 440.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    const double max_expected_step = 2.0 * std::sin(kTwoPi * kFreq / (2.0 * kInRate)) * 0.5 + 0.05;

    std::vector<float> cap_leftover_l, cap_leftover_r;
    std::vector<float> ren_leftover_l, ren_leftover_r;
    std::vector<float> rendered;
    rendered.reserve(44100 * 2);

    uint64_t in_sample_idx = 0;
    for (int tick = 0; tick < 150; ++tick) {
        // Produce 441 frames (10ms @ 44.1 kHz)
        std::vector<float> in_l = cap_leftover_l;
        std::vector<float> in_r = cap_leftover_r;
        for (int i = 0; i < 441; ++i) {
            const float s = 0.5f * static_cast<float>(
                std::sin(kTwoPi * kFreq * static_cast<double>(in_sample_idx++) / kInRate));
            in_l.push_back(s);
            in_r.push_back(s);
        }

        std::vector<float> mid_l(480, 0.0f), mid_r(480, 0.0f);
        size_t cap_prod = 0;
        const size_t cap_cons = cap_pll.process_stereo(
            in_l.data(), in_r.data(), in_l.size(), mid_l.data(), mid_r.data(), 480, cap_prod);
        cap_leftover_l.assign(in_l.begin() + static_cast<ptrdiff_t>(cap_cons), in_l.end());
        cap_leftover_r.assign(in_r.begin() + static_cast<ptrdiff_t>(cap_cons), in_r.end());

        std::vector<float> ren_in_l = ren_leftover_l;
        std::vector<float> ren_in_r = ren_leftover_r;
        ren_in_l.insert(ren_in_l.end(), mid_l.begin(), mid_l.begin() + static_cast<ptrdiff_t>(cap_prod));
        ren_in_r.insert(ren_in_r.end(), mid_r.begin(), mid_r.begin() + static_cast<ptrdiff_t>(cap_prod));

        std::vector<float> out_l(512, 0.0f), out_r(512, 0.0f);
        size_t ren_prod = 0;
        const size_t ren_cons = ren_pll.process_stereo(
            ren_in_l.data(), ren_in_r.data(), ren_in_l.size(), out_l.data(), out_r.data(), 512, ren_prod);
        ren_leftover_l.assign(ren_in_l.begin() + static_cast<ptrdiff_t>(ren_cons), ren_in_l.end());
        ren_leftover_r.assign(ren_in_r.begin() + static_cast<ptrdiff_t>(ren_cons), ren_in_r.end());

        rendered.insert(rendered.end(), out_l.begin(), out_l.begin() + static_cast<ptrdiff_t>(ren_prod));
    }

    ATRIS_CHECK(rendered.size() > 60000, "44.1k->48k->44.1k produced continuous output");

    // Check steady-state region (skip first 4410 frames = 100ms filter warmup)
    size_t zero_crossings = 0;
    float max_step = 0.0f;
    float peak = 0.0f;
    const size_t start = 4410;
    const size_t end = rendered.size() - 1;
    for (size_t i = start; i < end; ++i) {
        const float a = rendered[i];
        const float b = rendered[i + 1];
        ATRIS_CHECK(std::isfinite(a), "Resampled output finite");
        peak = std::max(peak, std::abs(a));
        max_step = std::max(max_step, std::abs(b - a));
        if (a <= 0.0f && b > 0.0f) {
            ++zero_crossings;
        }
    }
    const double duration_s = static_cast<double>(end - start) / kInRate;
    const double measured_hz = static_cast<double>(zero_crossings) / duration_s;

    ATRIS_CHECK(peak > 0.40f, "Sine amplitude preserved (> 0.40)");
    ATRIS_CHECK(max_step < max_expected_step, "Sample-to-sample derivative bounded (no clicks/crackles)");
    ATRIS_CHECK(std::abs(measured_hz - kFreq) < 10.0, "440 Hz frequency preserved (no chipmunk pitch shift)");
}

void test_convolution_reverb_precomputed_presets_and_bus_mix() {
    atriswave::dsp::ConvolutionReverb conv;
    conv.prepare(48000.0);
    conv.set_mix(0.5f, 0.5f);

    static_assert(noexcept(conv.set_preset(atriswave::dsp::IrPreset::StudioBooth)),
                  "ConvolutionReverb::set_preset must be noexcept");

    const atriswave::dsp::IrPreset presets[] = {
        atriswave::dsp::IrPreset::Algorithmic,
        atriswave::dsp::IrPreset::StudioBooth,
        atriswave::dsp::IrPreset::BroadcastRoom,
        atriswave::dsp::IrPreset::ConcertHall,
    };

    for (auto p : presets) {
        conv.set_preset(p);
        float max_abs = 0.0f;
        for (int i = 0; i < 1024; ++i) {
            float l = (i == 0) ? 1.0f : 0.0f;
            float r = (i == 0) ? 1.0f : 0.0f;
            conv.process_stereo(l, r);
            ATRIS_CHECK(std::isfinite(l) && std::isfinite(r), "ConvolutionReverb output finite");
            max_abs = std::max(max_abs, std::abs(l));
        }
        ATRIS_CHECK(max_abs > 0.01f, "ConvolutionReverb preset produces non-zero impulse response");
    }

    auto engine_ptr = std::make_unique<atriswave::AudioEngine>();
    HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ATRIS_CHECK(engine_ptr->start(evt, true), "AudioEngine headless start for bus mix test");
    const auto bind1 = engine_ptr->bind_output_bus("A1", "{mock-idempotent-bus}", 1.0f, false);
    ATRIS_CHECK(bind1.ok, "Initial bind_output_bus succeeds");
    const auto bind_idem = engine_ptr->bind_output_bus("A1", "{mock-idempotent-bus}", 0.5f, true);
    ATRIS_CHECK(bind_idem.ok, "Idempotent bind_output_bus succeeds without deadlock");
    const auto mix_res = engine_ptr->set_output_bus_mix("a1", 0.65f, true);
    ATRIS_CHECK(mix_res.ok && mix_res.bus_id == "A1" && std::fabs(mix_res.volume - 0.65f) < 1e-4f && mix_res.muted,
                "set_output_bus_mix updates A1 cleanly");
    const auto bad_mix = engine_ptr->set_output_bus_mix("Z9", 1.0f, false);
    ATRIS_CHECK(!bad_mix.ok && bad_mix.error == "invalid_bus_id", "Invalid bus_id rejected");
    engine_ptr->stop();
    CloseHandle(evt);
}

} // namespace

int main() {
    test_telemetry_v3_layout_and_roundtrip();
    test_dynamic_mixer_bus_routing_limiter_and_ducking();
    test_engine_v2_channels_feedback_and_echo_lock();
    test_failsafe_watchdog_roundtrip();
    test_spectrum_analyzer();
    test_soundboard_wav_decode_and_playback();
    test_44100_to_48000_to_44100_continuous_sine();
    test_convolution_reverb_precomputed_presets_and_bus_mix();

    if (g_failures == 0) {
        std::cout << "[atriswave_v2_mixer_tests] ALL TESTS PASSED\n";
        return 0;
    }

    std::cerr << "[atriswave_v2_mixer_tests] " << g_failures << " FAILURE(S)\n";
    return 1;
}
