#include <atriswave/audio/audio_engine.hpp>
#include <atriswave/audio/com_utils.hpp>
#include <atriswave/audio/config_store.hpp>
#include <atriswave/ipc_protocol.h>
#include <atriswave/rt_safety.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace atriswave {
namespace {

dsp::MicDspParams to_dsp_params(const MicDspConfig& cfg) noexcept {
    dsp::MicDspParams p{};
    p.bypass_all = cfg.bypass_all;
    p.direct_monitor = cfg.direct_monitor;
    p.monitor_processed = cfg.monitor_processed;
    p.monitor_gain = cfg.monitor_gain;
    p.denoise_enabled = cfg.denoise_enabled;
    p.denoise_strength = cfg.denoise_strength;
    p.gate_enabled = cfg.gate_enabled;
    p.gate_threshold_db = cfg.gate_threshold_db;
    p.gate_close_threshold_db = cfg.gate_threshold_db - 6.0f;
    p.eq_enabled = cfg.eq_enabled;
    p.eq_hpf_hz = cfg.eq_hpf_hz;
    p.eq_low_db = cfg.eq_low_db;
    p.eq_mid1_hz = cfg.eq_mid1_hz;
    p.eq_mid1_db = cfg.eq_mid1_db;
    p.eq_mid2_hz = cfg.eq_mid2_hz;
    p.eq_mid2_db = cfg.eq_mid2_db;
    p.eq_high_db = cfg.eq_high_db;
    p.comp_enabled = cfg.comp_enabled;
    p.comp_threshold_db = cfg.comp_threshold_db;
    p.comp_ratio = cfg.comp_ratio;
    p.comp_makeup_db = cfg.comp_makeup_db;
    p.ducking_enabled = cfg.ducking_enabled;
    p.ducking_depth_db = cfg.ducking_depth_db;
    p.reverb_enabled = cfg.reverb_enabled;
    p.reverb_wet = cfg.reverb_wet;
    p.reverb_room = cfg.reverb_room;
    p.reverb_damp = cfg.reverb_damp;
    p.reverb_mode = cfg.reverb_mode;
    p.pitch_enabled = cfg.pitch_enabled;
    p.pitch_semitones = cfg.pitch_semitones;
    p.limiter_enabled = cfg.limiter_enabled;
    p.limiter_ceiling_db = cfg.limiter_ceiling_db;
    return p;
}

std::string serialize_mic_cfg_json(const MicDspConfig& c) {
    char buf[1024]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "{\"bypass_all\":%s,\"direct_monitor\":%s,\"monitor_processed\":%s,\"monitor_gain\":%.4f,\"denoise_enabled\":%s,\"denoise_strength\":%.4f,"
        "\"gate_enabled\":%s,\"gate_threshold_db\":%.4f,\"eq_enabled\":%s,\"eq_hpf_hz\":%.4f,"
        "\"eq_low_db\":%.4f,\"eq_mid1_hz\":%.4f,\"eq_mid1_db\":%.4f,\"eq_mid2_hz\":%.4f,"
        "\"eq_mid2_db\":%.4f,\"eq_high_db\":%.4f,\"comp_enabled\":%s,\"comp_threshold_db\":%.4f,"
        "\"comp_ratio\":%.4f,\"comp_makeup_db\":%.4f,\"ducking_enabled\":%s,\"ducking_depth_db\":%.4f,"
        "\"reverb_enabled\":%s,\"reverb_wet\":%.4f,\"reverb_room\":%.4f,\"reverb_damp\":%.4f,"
        "\"reverb_mode\":%u,\"pitch_enabled\":%s,\"pitch_semitones\":%.4f,"
        "\"limiter_enabled\":%s,\"limiter_ceiling_db\":%.4f}",
        c.bypass_all ? "true" : "false",
        c.direct_monitor ? "true" : "false",
        c.monitor_processed ? "true" : "false",
        c.monitor_gain,
        c.denoise_enabled ? "true" : "false", c.denoise_strength,
        c.gate_enabled ? "true" : "false", c.gate_threshold_db,
        c.eq_enabled ? "true" : "false", c.eq_hpf_hz,
        c.eq_low_db, c.eq_mid1_hz, c.eq_mid1_db, c.eq_mid2_hz, c.eq_mid2_db, c.eq_high_db,
        c.comp_enabled ? "true" : "false", c.comp_threshold_db, c.comp_ratio, c.comp_makeup_db,
        c.ducking_enabled ? "true" : "false", c.ducking_depth_db,
        c.reverb_enabled ? "true" : "false", c.reverb_wet, c.reverb_room, c.reverb_damp,
        c.reverb_mode, c.pitch_enabled ? "true" : "false", c.pitch_semitones,
        c.limiter_enabled ? "true" : "false", c.limiter_ceiling_db);
    return std::string(buf);
}

std::optional<double> find_number(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t kp = json.find(needle);
    if (kp == std::string_view::npos) return std::nullopt;
    size_t pos = json.find(':', kp + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    char* end = nullptr;
    const double val = std::strtod(json.data() + pos, &end);
    if (end == json.data() + pos) return std::nullopt;
    return val;
}

std::optional<bool> find_bool(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t kp = json.find(needle);
    if (kp == std::string_view::npos) return std::nullopt;
    size_t pos = json.find(':', kp + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    const auto tail = json.substr(pos);
    if (tail.starts_with("true")) return true;
    if (tail.starts_with("false")) return false;
    return std::nullopt;
}

MicDspConfig parse_mic_cfg_json(std::string_view json) {
    MicDspConfig c{};
    if (auto v = find_bool(json, "bypass_all")) c.bypass_all = *v;
    if (auto v = find_bool(json, "direct_monitor")) c.direct_monitor = *v;
    if (auto v = find_bool(json, "monitor_processed")) c.monitor_processed = *v;
    if (auto v = find_number(json, "monitor_gain")) c.monitor_gain = static_cast<float>(*v);
    if (auto v = find_bool(json, "denoise_enabled")) c.denoise_enabled = *v;
    if (auto v = find_number(json, "denoise_strength")) c.denoise_strength = static_cast<float>(*v);
    if (auto v = find_bool(json, "gate_enabled")) c.gate_enabled = *v;
    if (auto v = find_number(json, "gate_threshold_db")) c.gate_threshold_db = static_cast<float>(*v);
    if (auto v = find_bool(json, "eq_enabled")) c.eq_enabled = *v;
    if (auto v = find_number(json, "eq_hpf_hz")) c.eq_hpf_hz = static_cast<float>(*v);
    if (auto v = find_number(json, "eq_low_db")) c.eq_low_db = static_cast<float>(*v);
    if (auto v = find_number(json, "eq_mid1_hz")) c.eq_mid1_hz = static_cast<float>(*v);
    if (auto v = find_number(json, "eq_mid1_db")) c.eq_mid1_db = static_cast<float>(*v);
    if (auto v = find_number(json, "eq_mid2_hz")) c.eq_mid2_hz = static_cast<float>(*v);
    if (auto v = find_number(json, "eq_mid2_db")) c.eq_mid2_db = static_cast<float>(*v);
    if (auto v = find_number(json, "eq_high_db")) c.eq_high_db = static_cast<float>(*v);
    if (auto v = find_bool(json, "comp_enabled")) c.comp_enabled = *v;
    if (auto v = find_number(json, "comp_threshold_db")) c.comp_threshold_db = static_cast<float>(*v);
    if (auto v = find_number(json, "comp_ratio")) c.comp_ratio = static_cast<float>(*v);
    if (auto v = find_number(json, "comp_makeup_db")) c.comp_makeup_db = static_cast<float>(*v);
    if (auto v = find_bool(json, "ducking_enabled")) c.ducking_enabled = *v;
    if (auto v = find_number(json, "ducking_depth_db")) c.ducking_depth_db = static_cast<float>(*v);
    if (auto v = find_bool(json, "reverb_enabled")) c.reverb_enabled = *v;
    if (auto v = find_number(json, "reverb_wet")) c.reverb_wet = static_cast<float>(*v);
    if (auto v = find_number(json, "reverb_room")) c.reverb_room = static_cast<float>(*v);
    if (auto v = find_number(json, "reverb_damp")) c.reverb_damp = static_cast<float>(*v);
    if (auto v = find_number(json, "reverb_mode")) c.reverb_mode = static_cast<uint32_t>(*v);
    if (auto v = find_bool(json, "pitch_enabled")) c.pitch_enabled = *v;
    if (auto v = find_number(json, "pitch_semitones")) c.pitch_semitones = static_cast<float>(*v);
    if (auto v = find_bool(json, "limiter_enabled")) c.limiter_enabled = *v;
    if (auto v = find_number(json, "limiter_ceiling_db")) c.limiter_ceiling_db = static_cast<float>(*v);
    return c;
}

} // namespace

AudioEngine::~AudioEngine() {
    stop();
}

void AudioEngine::init_default_slots() {
    for (size_t i = 0; i < dsp::kDynamicMaxChannels; ++i) {
        channels_[i].occupied = false;
        channels_[i].stream_active.store(false, std::memory_order_relaxed);
        channels_[i].send_a1_rt.store(false, std::memory_order_relaxed);
        channels_[i].info = DynamicChannelInfo{};
        channels_[i].info.channel_id = static_cast<uint32_t>(i);
        channels_[i].pair_group.clear();
        channels_[i].pll_l.reset();
        channels_[i].pll_r.reset();
        channels_[i].leftover_count = 0;
        dynamic_mixer_.set_channel_active(i, false);
    }

    // Slot 0: System (All Apps)
    {
        auto& sys = channels_[0];
        sys.occupied = true;
        sys.info.channel_id = 0;
        sys.info.name = "System";
        sys.info.source_type = "app_group";
        sys.info.color = "#6366F1";
        sys.info.is_system = true;
        sys.info.send_a1 = true;
        sys.send_a1_rt.store(true, std::memory_order_relaxed);
        dynamic_mixer_.set_channel_active(0, true);
        dynamic_mixer_.set_channel_mix(0, 1.0f, false, false, 0.0f);
        dynamic_mixer_.set_channel_send(0, 0, true, 1.0f);
    }

    // Slot 1: Microphone (Hardware Input)
    {
        auto& mic = channels_[1];
        mic.occupied = true;
        mic.info.channel_id = 1;
        mic.info.name = "Microphone";
        mic.info.source_type = "hardware_input";
        mic.info.color = "#10B981";
        mic.info.is_system = false;
        mic.info.send_b1 = true;
        mic.info.send_b2 = true;
        dynamic_mixer_.set_channel_active(1, true);
        dynamic_mixer_.set_channel_mix(1, 1.0f, false, false, 0.0f);
        dynamic_mixer_.set_channel_send(1, 2, true, 1.0f);
        dynamic_mixer_.set_channel_send(1, 3, true, 1.0f);
    }

    // 4 Output Buses: A1, A2, B1, B2
    const char* bus_ids[4] = {"A1", "A2", "B1", "B2"};
    const char* bus_names[4] = {"Headphones", "Speakers", "Mic Out", "Stream Mix"};
    for (size_t b = 0; b < dsp::kDynamicMaxBuses; ++b) {
        buses_[b].info = OutputBusInfo{};
        buses_[b].info.bus_id = bus_ids[b];
        buses_[b].info.role = bus_ids[b];
        buses_[b].info.name = bus_names[b];
        buses_[b].pair_group.clear();
        buses_[b].stream_active.store(false, std::memory_order_relaxed);
        buses_[b].pll_l.reset();
        buses_[b].pll_r.reset();
        dynamic_mixer_.set_bus_mix(b, 1.0f, false);
    }
}

bool AudioEngine::start(void* shutdown_event, bool headless_test) {
    stop();
    shutdown_event_ = shutdown_event;

    if (!headless_test) {
        (void)failsafe_.restore_if_needed();
    }

    for (size_t ch = 0; ch < dsp::kDynamicMaxChannels; ++ch) {
        ch_in_l_[ch].assign(kMasterTickFrames, 0.0f);
        ch_in_r_[ch].assign(kMasterTickFrames, 0.0f);
    }
    pll_in_l_.assign(4096, 0.0f);
    pll_in_r_.assign(4096, 0.0f);
    pll_out_l_.assign(4096, 0.0f);
    pll_out_r_.assign(4096, 0.0f);
    mic_mono_scratch_.assign(kMasterTickFrames, 0.0f);
    mic_mon_l_.assign(kMasterTickFrames, 0.0f);
    mic_mon_r_.assign(kMasterTickFrames, 0.0f);
    mic_bcast_l_.assign(kMasterTickFrames, 0.0f);
    mic_bcast_r_.assign(kMasterTickFrames, 0.0f);
    for (size_t b = 0; b < dsp::kDynamicMaxBuses; ++b) {
        bus_out_l_[b].assign(kMasterTickFrames, 0.0f);
        bus_out_r_[b].assign(kMasterTickFrames, 0.0f);
    }

    mic_pipeline_.prepare(48000.0, kMasterTickFrames);
    dynamic_mixer_.prepare(48000.0);
    spectrum_analyzer_.prepare(48000.0);
    mic_pipeline_.set_params(to_dsp_params(current_mic_cfg_));

    {
        std::lock_guard lock(state_mutex_);
        init_default_slots();
    }

    if (!telemetry_.open(ATRISWAVE_TELEMETRY_SHM_NAME_W)) {
        return false;
    }

    master_stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (master_stop_event_ == nullptr) {
        telemetry_.close();
        return false;
    }

    engine_state_.store(ATRISWAVE_ENGINE_STATE_RUNNING, std::memory_order_release);

    master_running_.store(true, std::memory_order_release);
    master_thread_ = std::thread(&AudioEngine::master_clock_loop, this);

    if (!control_server_.start(ATRISWAVE_PIPE_NAME_W, this)) {
        stop();
        return false;
    }

    if (!headless_test) {
        (void)failsafe_.arm_current_system_defaults();
        launch_watchdog_if_present();
        (void)auto_setup_defaults();
    }

    return true;
}

void AudioEngine::stop() {
    engine_state_.store(ATRISWAVE_ENGINE_STATE_STOPPED, std::memory_order_release);
    control_server_.stop();

    master_running_.store(false, std::memory_order_release);
    if (master_stop_event_ != nullptr) {
        SetEvent(static_cast<HANDLE>(master_stop_event_));
    }
    if (master_thread_.joinable()) {
        master_thread_.join();
    }
    if (master_stop_event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(master_stop_event_));
        master_stop_event_ = nullptr;
    }

    {
        std::lock_guard lock(state_mutex_);
        for (auto& ch : channels_) {
            ch.stream_active.store(false, std::memory_order_release);
            ch.loopback_capture.stop();
            ch.capture_stream.close();
        }
        for (auto& bus : buses_) {
            bus.stream_active.store(false, std::memory_order_release);
            bus.render_stream.close();
        }
    }

    publish_telemetry_tick();
    telemetry_.close();
    (void)failsafe_.disarm();
    if (watchdog_process_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(watchdog_process_));
        watchdog_process_ = nullptr;
    }
    shutdown_event_ = nullptr;
}

void AudioEngine::launch_watchdog_if_present() noexcept {
    wchar_t exe_path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    const std::filesystem::path watchdog_exe =
        std::filesystem::path(exe_path).parent_path() / L"AtrisWaveWatchdog.exe";
    std::error_code ec;
    if (!std::filesystem::exists(watchdog_exe, ec)) {
        return;
    }

    const std::wstring cmd =
        L"\"" + watchdog_exe.wstring() + L"\" --watch-pid " + std::to_wstring(GetCurrentProcessId());
    std::wstring cmd_mutable = cmd;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(
            watchdog_exe.c_str(),
            cmd_mutable.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        CloseHandle(pi.hThread);
        watchdog_process_ = pi.hProcess;
    }
}

void AudioEngine::master_clock_loop() {
    MmcssGuard mmcss(L"Pro Audio");
    DenormalGuard denormal;

    HANDLE timer = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    if (timer != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -100000LL; // 10ms
        SetWaitableTimer(timer, &due, 10, nullptr, nullptr, FALSE);
    }

    HANDLE handles[2] = {
        static_cast<HANDLE>(master_stop_event_),
        timer
    };

    while (master_running_.load(std::memory_order_acquire)) {
        if (timer != nullptr) {
            const DWORD res = WaitForMultipleObjects(2, handles, FALSE, 100);
            if (res == WAIT_OBJECT_0) {
                break;
            }
        } else {
            if (WaitForSingleObject(static_cast<HANDLE>(master_stop_event_), 10) == WAIT_OBJECT_0) {
                break;
            }
        }
        process_master_tick(kMasterTickFrames);
    }

    if (timer != nullptr) {
        CancelWaitableTimer(timer);
        CloseHandle(timer);
    }
}

void AudioEngine::process_master_tick(size_t frames) noexcept {
    std::array<const float*, dsp::kDynamicMaxChannels> in_l{};
    std::array<const float*, dsp::kDynamicMaxChannels> in_r{};

    for (size_t ch = 0; ch < dsp::kDynamicMaxChannels; ++ch) {
        if (!channels_[ch].stream_active.load(std::memory_order_acquire)) {
            continue;
        }
        auto& ring = channels_[ch].input_ring;
        const size_t avail = ring.available_read();
        const uint32_t ch_sr = channels_[ch].capture_sr.load(std::memory_order_relaxed);
        const size_t prime_frames = std::max<size_t>(ch_sr / 50u, 256u);

        if (!channels_[ch].primed.load(std::memory_order_acquire)) {
            if (avail < prime_frames) {
                std::fill_n(ch_in_l_[ch].data(), frames, 0.0f);
                std::fill_n(ch_in_r_[ch].data(), frames, 0.0f);
                continue;
            }
            channels_[ch].primed.store(true, std::memory_order_release);
        }

        const float target_fill =
            static_cast<float>(prime_frames + ch_sr / 100u) / static_cast<float>(ring.capacity());
        const float fill_ratio = static_cast<float>(avail) / static_cast<float>(ring.capacity());
        channels_[ch].pll_l.update_pll(fill_ratio, target_fill);

        size_t total_in = channels_[ch].leftover_count;
        for (size_t i = 0; i < total_in; ++i) {
            pll_in_l_[i] = channels_[ch].leftover_l[i];
            pll_in_r_[i] = channels_[ch].leftover_r[i];
        }

        const size_t needed_total =
            static_cast<size_t>(std::ceil(static_cast<double>(frames) * channels_[ch].pll_l.current_ratio())) + 4;
        const size_t to_pop = (needed_total > total_in)
            ? std::min({avail, needed_total - total_in, pll_in_l_.size() - total_in})
            : 0;

        for (size_t i = 0; i < to_pop; ++i) {
            StereoFrame f{};
            if (!ring.try_pop(f)) {
                f = StereoFrame{};
            }
            pll_in_l_[total_in + i] = f.l;
            pll_in_r_[total_in + i] = f.r;
        }
        total_in += to_pop;

        if (total_in > 0) {
            size_t produced = 0;
            const size_t consumed = channels_[ch].pll_l.process_stereo(
                pll_in_l_.data(),
                pll_in_r_.data(),
                total_in,
                ch_in_l_[ch].data(),
                ch_in_r_[ch].data(),
                frames,
                produced);
            const size_t rem = (total_in > consumed) ? std::min<size_t>(total_in - consumed, 16) : 0;
            for (size_t i = 0; i < rem; ++i) {
                channels_[ch].leftover_l[i] = pll_in_l_[consumed + i];
                channels_[ch].leftover_r[i] = pll_in_r_[consumed + i];
            }
            channels_[ch].leftover_count = rem;
            if (produced > 0) {
                channels_[ch].last_in_l = ch_in_l_[ch][produced - 1];
                channels_[ch].last_in_r = ch_in_r_[ch][produced - 1];
            }
            if (produced < frames) {
                const size_t decay_len = std::min<size_t>(frames - produced, 32);
                for (size_t k = 0; k < decay_len; ++k) {
                    const float att = 1.0f - static_cast<float>(k + 1) / static_cast<float>(decay_len);
                    ch_in_l_[ch][produced + k] = channels_[ch].last_in_l * att;
                    ch_in_r_[ch][produced + k] = channels_[ch].last_in_r * att;
                }
                for (size_t i = produced + decay_len; i < frames; ++i) {
                    ch_in_l_[ch][i] = 0.0f;
                    ch_in_r_[ch][i] = 0.0f;
                }
                channels_[ch].last_in_l = 0.0f;
                channels_[ch].last_in_r = 0.0f;
            }
        } else {
            const size_t decay_len = std::min<size_t>(frames, 32);
            for (size_t k = 0; k < decay_len; ++k) {
                const float att = 1.0f - static_cast<float>(k + 1) / static_cast<float>(decay_len);
                ch_in_l_[ch][k] = channels_[ch].last_in_l * att;
                ch_in_r_[ch][k] = channels_[ch].last_in_r * att;
            }
            for (size_t i = decay_len; i < frames; ++i) {
                ch_in_l_[ch][i] = 0.0f;
                ch_in_r_[ch][i] = 0.0f;
            }
            channels_[ch].last_in_l = 0.0f;
            channels_[ch].last_in_r = 0.0f;
        }

        if (ch == 1) {
            for (size_t i = 0; i < frames; ++i) {
                mic_mono_scratch_[i] = ch_in_l_[1][i];
            }
            mic_pipeline_.process(
                mic_mono_scratch_.data(),
                mic_mon_l_.data(),
                mic_mon_r_.data(),
                mic_bcast_l_.data(),
                mic_bcast_r_.data(),
                frames);
            in_l[1] = mic_bcast_l_.data();
            in_r[1] = mic_bcast_r_.data();
        } else {
            in_l[ch] = ch_in_l_[ch].data();
            in_r[ch] = ch_in_r_[ch].data();
        }
    }

    if (spectrum_reset_pending_.exchange(false, std::memory_order_acq_rel)) {
        spectrum_analyzer_.reset();
    }
    const uint32_t spec_ch = spectrum_source_ch_.load(std::memory_order_relaxed);
    if (spec_ch < dsp::kDynamicMaxChannels && in_l[spec_ch] != nullptr) {
        spectrum_analyzer_.push_block(in_l[spec_ch], in_r[spec_ch], frames);
    }

    std::array<float*, dsp::kDynamicMaxBuses> out_l{};
    std::array<float*, dsp::kDynamicMaxBuses> out_r{};
    for (size_t b = 0; b < dsp::kDynamicMaxBuses; ++b) {
        out_l[b] = bus_out_l_[b].data();
        out_r[b] = bus_out_r_[b].data();
    }

    dynamic_mixer_.process(in_l, in_r, out_l, out_r, frames);

    // Mix Soundboard polyphonic voices into A1, B1, B2
    soundboard_.mix_into_buses(
        bus_out_l_[0].data(), bus_out_r_[0].data(),
        bus_out_l_[2].data(), bus_out_r_[2].data(),
        bus_out_l_[3].data(), bus_out_r_[3].data(),
        frames);

    // Sum low-latency Direct Monitor Sidetone into Bus A1 ONLY if Mic->A1 send is OFF (anti-double-audio)
    if (channels_[1].stream_active.load(std::memory_order_acquire) &&
        !channels_[1].send_a1_rt.load(std::memory_order_acquire)) {
        for (size_t i = 0; i < frames; ++i) {
            bus_out_l_[0][i] += mic_mon_l_[i];
            bus_out_r_[0][i] += mic_mon_r_[i];
        }
    }

    // Clamp A1, B1, B2 after post-limiter Soundboard + Sidetone summing
    for (size_t b : {size_t{0}, size_t{2}, size_t{3}}) {
        for (size_t i = 0; i < frames; ++i) {
            bus_out_l_[b][i] = std::clamp(bus_out_l_[b][i], -0.95f, 0.95f);
            bus_out_r_[b][i] = std::clamp(bus_out_r_[b][i], -0.95f, 0.95f);
        }
    }

    for (size_t b = 0; b < dsp::kDynamicMaxBuses; ++b) {
        if (!buses_[b].stream_active.load(std::memory_order_acquire)) {
            continue;
        }
        auto& ring = buses_[b].output_ring;
        const uint32_t bus_sr = buses_[b].render_sr.load(std::memory_order_relaxed);
        const float target_fill =
            static_cast<float>(bus_sr / 40u) / static_cast<float>(ring.capacity());
        const float fill_ratio = static_cast<float>(ring.available_read()) / static_cast<float>(ring.capacity());
        buses_[b].pll_l.update_pll(fill_ratio, target_fill);

        size_t total_in = buses_[b].leftover_count;
        for (size_t i = 0; i < total_in; ++i) {
            pll_in_l_[i] = buses_[b].leftover_l[i];
            pll_in_r_[i] = buses_[b].leftover_r[i];
        }
        const size_t to_copy = std::min(frames, pll_in_l_.size() - total_in);
        std::copy_n(bus_out_l_[b].data(), to_copy, pll_in_l_.data() + total_in);
        std::copy_n(bus_out_r_[b].data(), to_copy, pll_in_r_.data() + total_in);
        total_in += to_copy;

        const size_t max_out = std::min<size_t>(
            pll_out_l_.size(),
            static_cast<size_t>(std::ceil(static_cast<double>(total_in) / buses_[b].pll_l.current_ratio())) + 4);
        size_t produced = 0;
        const size_t consumed = buses_[b].pll_l.process_stereo(
            pll_in_l_.data(),
            pll_in_r_.data(),
            total_in,
            pll_out_l_.data(),
            pll_out_r_.data(),
            max_out,
            produced);

        const size_t rem = (total_in > consumed) ? std::min<size_t>(total_in - consumed, 16) : 0;
        for (size_t i = 0; i < rem; ++i) {
            buses_[b].leftover_l[i] = pll_in_l_[consumed + i];
            buses_[b].leftover_r[i] = pll_in_r_[consumed + i];
        }
        buses_[b].leftover_count = rem;

        for (size_t i = 0; i < produced; ++i) {
            if (!ring.try_push(StereoFrame{pll_out_l_[i], pll_out_r_[i]})) {
                xrun_count_.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
}

void AudioEngine::publish_telemetry_tick() noexcept {
    if (!telemetry_.is_open()) {
        return;
    }

    TelemetrySnapshot snap{};
    snap.header.version = ATRISWAVE_TELEMETRY_VERSION;
    snap.header.sample_rate = sample_rate_.load(std::memory_order_relaxed);
    snap.header.buffer_frames = buffer_frames_.load(std::memory_order_relaxed);
    snap.header.engine_state = engine_state_.load(std::memory_order_acquire);
    snap.header.timestamp_qpc = TelemetryPublisher::query_qpc();
    snap.header.xrun_count = xrun_count_.load(std::memory_order_relaxed);
    snap.header.bus_count = ATRISWAVE_MAX_BUSES;
    snap.header.spectrum_bins = ATRISWAVE_SPECTRUM_BINS;

    uint32_t active_ch = 0;
    dynamic_mixer_.fill_telemetry(snap.channels, active_ch, snap.buses);
    snap.header.channel_count = active_ch;
    snap.channels[1].gain_reduction_db = mic_pipeline_.gain_reduction_db();
    spectrum_analyzer_.read_bins(snap.spectrum_db, ATRISWAVE_SPECTRUM_BINS);

    telemetry_.publish(snap, snap.spectrum_db);
}

int AudioEngine::bus_index_from_id(const std::string& bus_id) noexcept {
    if (bus_id == "A1" || bus_id == "a1") return 0;
    if (bus_id == "A2" || bus_id == "a2") return 1;
    if (bus_id == "B1" || bus_id == "b1") return 2;
    if (bus_id == "B2" || bus_id == "b2") return 3;
    return -1;
}

std::string AudioEngine::resolve_pair_group(const std::string& device_id) const {
    if (device_id.empty()) {
        return {};
    }
    const auto devices = audio::WasapiDeviceEnumerator::enumerate_devices();
    for (const auto& d : devices) {
        if (d.id == device_id) {
            return d.virtual_pair_group;
        }
    }
    return {};
}

bool AudioEngine::would_create_feedback_loop_locked(
    uint32_t /*ch_idx*/,
    const std::string& ch_device_id,
    const std::string& ch_pair_group,
    size_t /*bus_idx*/,
    const std::string& bus_device_id,
    const std::string& bus_pair_group) const {
    if (!ch_device_id.empty() && !bus_device_id.empty() && ch_device_id == bus_device_id) {
        return true;
    }
    if (!ch_pair_group.empty() && !bus_pair_group.empty() && ch_pair_group == bus_pair_group) {
        return true;
    }
    return false;
}

ControlServer::StatusSnapshot AudioEngine::get_status() {
    ControlServer::StatusSnapshot s{};
    s.engine_state = engine_state_.load(std::memory_order_acquire);
    s.sample_rate = sample_rate_.load(std::memory_order_relaxed);
    s.buffer_frames = buffer_frames_.load(std::memory_order_relaxed);
    std::lock_guard lock(state_mutex_);
    uint32_t count = 0;
    for (const auto& ch : channels_) {
        if (ch.occupied) ++count;
    }
    s.channel_count = count;
    return s;
}

void AudioEngine::on_shutdown() {
    if (shutdown_event_ != nullptr) {
        SetEvent(static_cast<HANDLE>(shutdown_event_));
    }
}

std::vector<audio::AudioDeviceInfo> AudioEngine::list_devices() {
    return audio::WasapiDeviceEnumerator::enumerate_devices();
}

std::vector<audio::AudioSessionInfo> AudioEngine::list_sessions() {
    return audio::AppRouter::list_sessions();
}

bool AudioEngine::set_channel(const ChannelConfig& cfg) {
    if (cfg.channel_id >= dsp::kDynamicMaxChannels) {
        return false;
    }
    std::lock_guard lock(state_mutex_);
    if (!channels_[cfg.channel_id].occupied) {
        return false;
    }
    channels_[cfg.channel_id].info.volume = cfg.volume;
    channels_[cfg.channel_id].info.muted = cfg.muted;
    channels_[cfg.channel_id].info.solo = cfg.solo;
    dynamic_mixer_.set_channel_mix(
        cfg.channel_id, cfg.volume, cfg.muted, cfg.solo, channels_[cfg.channel_id].info.pan);

    if (channels_[cfg.channel_id].info.source_type == "app_group") {
        for (const auto& proc : channels_[cfg.channel_id].info.bound_apps) {
            (void)audio::AppRouter::set_app_session_volume_mute(proc, cfg.volume, cfg.muted);
        }
    }
    return true;
}

bool AudioEngine::set_mic_dsp(const MicDspConfig& cfg) {
    {
        std::lock_guard lock(config_mutex_);
        current_mic_cfg_ = cfg;
    }
    mic_pipeline_.set_params(to_dsp_params(cfg));
    return true;
}

bool AudioEngine::select_devices(const std::string& input_id,
                                 const std::string& monitor_id,
                                 const std::string& output_id) {
    bool ok = true;
    if (!input_id.empty()) {
        ok = bind_channel_source(1, "hardware_input", input_id).ok && ok;
    }
    if (!monitor_id.empty()) {
        ok = bind_output_bus("A1", monitor_id, 1.0f, false).ok && ok;
    }
    if (!output_id.empty()) {
        ok = bind_output_bus("B1", output_id, 1.0f, false).ok && ok;
    }
    return ok;
}

audio::RouteResult AudioEngine::route_app(uint32_t pid,
                                          const std::string& target_device_id,
                                          bool loopback_capture) {
    if (loopback_capture) {
        std::lock_guard lock(state_mutex_);
        uint32_t slot = 0;
        for (uint32_t i = 2; i < dsp::kDynamicMaxChannels; ++i) {
            if (!channels_[i].occupied) {
                slot = i;
                break;
            }
        }
        if (slot == 0) {
            return {false, "unsupported", "No free channel slots"};
        }
        channels_[slot].occupied = true;
        channels_[slot].info.channel_id = slot;
        channels_[slot].info.name = "Loopback " + std::to_string(pid);
        channels_[slot].info.source_type = "process_loopback";
        channels_[slot].info.echo_locked_a1_a2 = true;
        channels_[slot].info.send_b2 = true;
        dynamic_mixer_.set_channel_active(slot, true);
        dynamic_mixer_.set_channel_send(slot, 3, true, 1.0f);

        const bool ok = channels_[slot].loopback_capture.start(
            pid,
            true,
            [this, slot](const float* data, uint32_t frames, uint32_t ch) {
                on_channel_loopback(slot, data, frames, ch);
            });
        channels_[slot].stream_active.store(ok, std::memory_order_release);
        return {ok, ok ? "loopback_capture" : "unsupported",
                ok ? "Process loopback capture active" : "Process loopback unavailable"};
    }
    return audio::AppRouter::route_app_to_device(pid, target_device_id);
}

bool AudioEngine::save_preset(const std::string& name) {
    MicDspConfig copy{};
    {
        std::lock_guard lock(config_mutex_);
        copy = current_mic_cfg_;
    }
    return audio::ConfigStore::save_preset(name, serialize_mic_cfg_json(copy));
}

std::optional<MicDspConfig> AudioEngine::load_preset(const std::string& name) {
    const auto json = audio::ConfigStore::load_preset(name);
    if (!json.has_value()) {
        return std::nullopt;
    }
    const MicDspConfig cfg = parse_mic_cfg_json(*json);
    (void)set_mic_dsp(cfg);
    return cfg;
}

std::vector<std::string> AudioEngine::list_presets() {
    return audio::ConfigStore::list_presets();
}

audio::DriverStatus AudioEngine::get_driver_status() {
    return audio::DriverBridge::probe_driver();
}

std::vector<audio::RoutingRule> AudioEngine::list_routing_rules() {
    return routing_rules_.list_rules();
}

ControlServer::RoutingRuleApplyResult AudioEngine::set_routing_rule(const audio::RoutingRule& rule) {
    ControlServer::RoutingRuleApplyResult result{};
    result.saved = routing_rules_.set_rule(rule);
    if (!result.saved) {
        return result;
    }
    const auto sessions = audio::AppRouter::list_sessions();
    for (const auto& s : sessions) {
        if (_stricmp(s.process_name.c_str(), rule.process_name.c_str()) == 0) {
            const auto route_res = route_app(s.pid, rule.target_device_id, rule.loopback_capture);
            result.applied = route_res.ok;
            result.method = route_res.method;
            break;
        }
    }
    return result;
}

bool AudioEngine::delete_routing_rule(const std::string& process_name) {
    return routing_rules_.delete_rule(process_name);
}

uint32_t AudioEngine::apply_routing_rules() {
    const auto rules = routing_rules_.list_rules();
    if (rules.empty()) {
        return 0;
    }
    const auto sessions = audio::AppRouter::list_sessions();
    uint32_t applied_count = 0;
    for (const auto& s : sessions) {
        for (const auto& r : rules) {
            if (_stricmp(s.process_name.c_str(), r.process_name.c_str()) == 0) {
                const auto res = route_app(s.pid, r.target_device_id, r.loopback_capture);
                if (res.ok) {
                    ++applied_count;
                }
                break;
            }
        }
    }
    return applied_count;
}

std::vector<DynamicChannelInfo> AudioEngine::list_channels() {
    std::lock_guard lock(state_mutex_);
    std::vector<DynamicChannelInfo> out;
    for (const auto& ch : channels_) {
        if (ch.occupied) {
            out.push_back(ch.info);
        }
    }
    return out;
}

V2CommandResult AudioEngine::create_channel(const std::string& name,
                                            const std::string& source_type,
                                            const std::string& color) {
    if (name.empty()) {
        return {false, "invalid_name", 0};
    }
    std::lock_guard lock(state_mutex_);
    for (uint32_t i = 2; i < dsp::kDynamicMaxChannels; ++i) {
        if (!channels_[i].occupied) {
            auto& ch = channels_[i];
            ch.occupied = true;
            ch.stream_active.store(false, std::memory_order_release);
            ch.pair_group.clear();
            ch.info = DynamicChannelInfo{};
            ch.info.channel_id = i;
            ch.info.name = name;
            ch.info.source_type = source_type.empty() ? "app_group" : source_type;
            ch.info.color = color.empty() ? "#6366F1" : color;
            ch.info.is_system = false;
            ch.info.echo_locked_a1_a2 = (ch.info.source_type == "process_loopback");
            ch.info.send_a1 = !ch.info.echo_locked_a1_a2;
            ch.send_a1_rt.store(ch.info.send_a1, std::memory_order_release);
            ch.info.send_b2 = ch.info.echo_locked_a1_a2;

            dynamic_mixer_.set_channel_active(i, true);
            dynamic_mixer_.set_channel_mix(i, 1.0f, false, false, 0.0f);
            dynamic_mixer_.set_channel_send(i, 0, ch.info.send_a1, 1.0f);
            dynamic_mixer_.set_channel_send(i, 1, false, 1.0f);
            dynamic_mixer_.set_channel_send(i, 2, false, 1.0f);
            dynamic_mixer_.set_channel_send(i, 3, ch.info.send_b2, 1.0f);
            return {true, {}, i};
        }
    }
    return {false, "max_channels_reached", 0};
}

V2CommandResult AudioEngine::delete_channel(uint32_t channel_id) {
    if (channel_id == 0) {
        return {false, "cannot_delete_system_channel", channel_id};
    }
    if (channel_id >= dsp::kDynamicMaxChannels) {
        return {false, "invalid_channel_id", channel_id};
    }
    std::lock_guard lock(state_mutex_);
    if (!channels_[channel_id].occupied) {
        return {false, "channel_not_found", channel_id};
    }
    channels_[channel_id].stream_active.store(false, std::memory_order_release);
    channels_[channel_id].loopback_capture.stop();
    channels_[channel_id].capture_stream.close();
    channels_[channel_id].occupied = false;
    channels_[channel_id].info = DynamicChannelInfo{};
    channels_[channel_id].pair_group.clear();
    dynamic_mixer_.set_channel_active(channel_id, false);
    return {true, {}, channel_id};
}

V2CommandResult AudioEngine::rename_channel(uint32_t channel_id, const std::string& name) {
    if (channel_id >= dsp::kDynamicMaxChannels || name.empty()) {
        return {false, "invalid_params", channel_id};
    }
    std::lock_guard lock(state_mutex_);
    if (!channels_[channel_id].occupied) {
        return {false, "channel_not_found", channel_id};
    }
    channels_[channel_id].info.name = name;
    return {true, {}, channel_id};
}

V2CommandResult AudioEngine::bind_channel_source(uint32_t channel_id,
                                                 const std::string& source_type,
                                                 const std::string& device_id) {
    if (channel_id >= dsp::kDynamicMaxChannels) {
        return {false, "invalid_channel_id", channel_id};
    }
    const std::string new_pair = resolve_pair_group(device_id);

    std::lock_guard lock(state_mutex_);
    auto& ch = channels_[channel_id];
    if (!ch.occupied) {
        return {false, "channel_not_found", channel_id};
    }

    const bool sends[4] = {ch.info.send_a1, ch.info.send_a2, ch.info.send_b1, ch.info.send_b2};
    for (size_t b = 0; b < dsp::kDynamicMaxBuses; ++b) {
        if (sends[b] && would_create_feedback_loop_locked(
                            channel_id, device_id, new_pair, b, buses_[b].info.device_id, buses_[b].pair_group)) {
            return {false, "feedback_loop_detected", channel_id};
        }
    }

    ch.stream_active.store(false, std::memory_order_release);
    ch.loopback_capture.stop();
    ch.capture_stream.close();

    ch.info.source_type = source_type;
    ch.info.device_id = device_id;
    ch.pair_group = new_pair;
    ch.info.echo_locked_a1_a2 = (source_type == "process_loopback");
    if (ch.info.echo_locked_a1_a2) {
        ch.info.send_a1 = false;
        ch.send_a1_rt.store(false, std::memory_order_release);
        ch.info.send_a2 = false;
        dynamic_mixer_.set_channel_send(channel_id, 0, false, 1.0f);
        dynamic_mixer_.set_channel_send(channel_id, 1, false, 1.0f);
    }

    if ((source_type == "hardware_input" || source_type == "virtual_endpoint") && !device_id.empty()) {
        if (ch.capture_stream.open(device_id, audio::StreamDirection::Capture)) {
            const uint32_t cap_sr = ch.capture_stream.sample_rate();
            ch.pll_l.prepare(static_cast<double>(cap_sr), 48000.0);
            ch.input_ring.reset();
            ch.leftover_count = 0;
            ch.last_in_l = 0.0f;
            ch.last_in_r = 0.0f;
            ch.primed.store(false, std::memory_order_release);
            ch.capture_sr.store(cap_sr, std::memory_order_relaxed);
            if (ch.capture_stream.start([this, channel_id](float* data, uint32_t frames, uint32_t nch) {
                    on_channel_capture(channel_id, data, frames, nch);
                })) {
                ch.stream_active.store(true, std::memory_order_release);
            }
        }
    }
    return {true, {}, channel_id};
}

std::vector<OutputBusInfo> AudioEngine::list_output_buses() {
    std::lock_guard lock(state_mutex_);
    std::vector<OutputBusInfo> out;
    out.reserve(dsp::kDynamicMaxBuses);
    for (const auto& b : buses_) {
        out.push_back(b.info);
    }
    return out;
}

V2CommandResult AudioEngine::bind_output_bus(const std::string& bus_id,
                                             const std::string& device_id,
                                             float volume,
                                             bool muted) {
    const int b_idx = bus_index_from_id(bus_id);
    if (b_idx < 0) {
        return {false, "invalid_bus_id", 0};
    }
    const size_t bus_idx = static_cast<size_t>(b_idx);

    std::string dev_name;
    bool dev_virtual = false;
    std::string bus_pair;
    if (!device_id.empty()) {
        const auto devices = audio::WasapiDeviceEnumerator::enumerate_devices();
        for (const auto& d : devices) {
            if (d.id == device_id) {
                dev_name = d.name;
                dev_virtual = d.is_virtual;
                bus_pair = d.virtual_pair_group;
                break;
            }
        }
    }

    std::lock_guard lock(state_mutex_);
    for (uint32_t c = 0; c < dsp::kDynamicMaxChannels; ++c) {
        if (!channels_[c].occupied) {
            continue;
        }
        const auto& ci = channels_[c].info;
        const bool sent = (bus_idx == 0 && ci.send_a1) || (bus_idx == 1 && ci.send_a2) ||
                          (bus_idx == 2 && ci.send_b1) || (bus_idx == 3 && ci.send_b2);
        if (sent && would_create_feedback_loop_locked(
                        c, ci.device_id, channels_[c].pair_group, bus_idx, device_id, bus_pair)) {
            return {false, "feedback_loop_detected", c};
        }
    }

    auto& bus = buses_[bus_idx];
    bus.stream_active.store(false, std::memory_order_release);
    bus.render_stream.close();

    bus.info.device_id = device_id;
    bus.info.device_name = dev_name;
    bus.info.is_virtual = dev_virtual;
    bus.pair_group = bus_pair;
    bus.info.volume = std::clamp(volume, 0.0f, 2.0f);
    bus.info.muted = muted;
    bus.info.active = false;
    dynamic_mixer_.set_bus_mix(bus_idx, bus.info.volume, bus.info.muted);

    if (!device_id.empty()) {
        if (bus.render_stream.open(device_id, audio::StreamDirection::Render)) {
            const uint32_t ren_sr = bus.render_stream.sample_rate();
            bus.pll_l.prepare(48000.0, static_cast<double>(ren_sr));
            bus.output_ring.reset();
            bus.leftover_count = 0;
            bus.last_out_l = 0.0f;
            bus.last_out_r = 0.0f;
            bus.render_sr.store(ren_sr, std::memory_order_relaxed);
            const size_t prefill = std::min<size_t>(ren_sr / 50u, 2048u);
            for (size_t i = 0; i < prefill; ++i) {
                (void)bus.output_ring.try_push(StereoFrame{0.0f, 0.0f});
            }
            if (bus.render_stream.start([this, bus_idx](float* data, uint32_t frames, uint32_t nch) {
                    on_bus_render(bus_idx, data, frames, nch);
                })) {
                bus.stream_active.store(true, std::memory_order_release);
                bus.info.active = true;
            }
        }
    }
    return {true, {}, 0};
}

V2CommandResult AudioEngine::set_channel_send(uint32_t channel_id,
                                              const std::string& bus_id,
                                              bool enabled,
                                              float send_gain) {
    if (channel_id >= dsp::kDynamicMaxChannels) {
        return {false, "invalid_channel_id", channel_id};
    }
    const int b_idx = bus_index_from_id(bus_id);
    if (b_idx < 0) {
        return {false, "invalid_bus_id", channel_id};
    }
    const size_t bus_idx = static_cast<size_t>(b_idx);

    std::lock_guard lock(state_mutex_);
    auto& ch = channels_[channel_id];
    if (!ch.occupied) {
        return {false, "channel_not_found", channel_id};
    }

    if (enabled && ch.info.source_type == "process_loopback" && (bus_idx == 0 || bus_idx == 1)) {
        return {false, "echo_lock_process_loopback", channel_id};
    }

    if (enabled && would_create_feedback_loop_locked(
                       channel_id,
                       ch.info.device_id,
                       ch.pair_group,
                       bus_idx,
                       buses_[bus_idx].info.device_id,
                       buses_[bus_idx].pair_group)) {
        return {false, "feedback_loop_detected", channel_id};
    }

    if (bus_idx == 0) {
        ch.info.send_a1 = enabled;
        ch.send_a1_rt.store(enabled, std::memory_order_release);
    }
    else if (bus_idx == 1) ch.info.send_a2 = enabled;
    else if (bus_idx == 2) ch.info.send_b1 = enabled;
    else if (bus_idx == 3) ch.info.send_b2 = enabled;

    dynamic_mixer_.set_channel_send(channel_id, bus_idx, enabled, send_gain);
    return {true, {}, channel_id};
}

V2CommandResult AudioEngine::set_channel_dsp(const ChannelDspConfig& cfg) {
    if (cfg.channel_id >= dsp::kDynamicMaxChannels) {
        return {false, "invalid_channel_id", cfg.channel_id};
    }
    std::lock_guard lock(state_mutex_);
    if (!channels_[cfg.channel_id].occupied) {
        return {false, "channel_not_found", cfg.channel_id};
    }
    auto& info = channels_[cfg.channel_id].info;
    info.volume = cfg.volume;
    info.muted = cfg.muted;
    info.solo = cfg.solo;
    info.pan = cfg.pan;

    dynamic_mixer_.set_channel_mix(cfg.channel_id, cfg.volume, cfg.muted, cfg.solo, cfg.pan);
    dynamic_mixer_.set_channel_dsp(
        cfg.channel_id,
        cfg.eq_enabled,
        cfg.eq_hpf_hz,
        cfg.eq_low_db,
        cfg.eq_mid1_hz,
        cfg.eq_mid1_db,
        cfg.eq_mid2_hz,
        cfg.eq_mid2_db,
        cfg.eq_high_db,
        cfg.comp_enabled,
        cfg.comp_threshold_db,
        cfg.comp_ratio,
        cfg.comp_makeup_db,
        cfg.duck_source_id,
        cfg.duck_depth_db);
    return {true, {}, cfg.channel_id};
}

V2CommandResult AudioEngine::bind_app_to_channel(const std::string& process_name,
                                                 uint32_t channel_id) {
    if (process_name.empty() || channel_id >= dsp::kDynamicMaxChannels) {
        return {false, "invalid_params", channel_id};
    }
    std::lock_guard lock(state_mutex_);
    if (!channels_[channel_id].occupied) {
        return {false, "channel_not_found", channel_id};
    }

    // Remove from any other channel first
    for (auto& ch : channels_) {
        if (!ch.occupied) continue;
        auto& apps = ch.info.bound_apps;
        apps.erase(
            std::remove_if(apps.begin(), apps.end(), [&](const std::string& a) {
                return _stricmp(a.c_str(), process_name.c_str()) == 0;
            }),
            apps.end());
    }

    channels_[channel_id].info.bound_apps.push_back(process_name);
    (void)routing_rules_.set_rule(
        audio::RoutingRule{process_name, channel_id, channels_[channel_id].info.device_id, false});
    return {true, {}, channel_id};
}

V2CommandResult AudioEngine::unbind_app_from_channel(const std::string& process_name,
                                                     uint32_t channel_id) {
    if (process_name.empty() || channel_id >= dsp::kDynamicMaxChannels) {
        return {false, "invalid_params", channel_id};
    }
    std::lock_guard lock(state_mutex_);
    if (!channels_[channel_id].occupied) {
        return {false, "channel_not_found", channel_id};
    }
    auto& apps = channels_[channel_id].info.bound_apps;
    apps.erase(
        std::remove_if(apps.begin(), apps.end(), [&](const std::string& a) {
            return _stricmp(a.c_str(), process_name.c_str()) == 0;
        }),
        apps.end());
    (void)routing_rules_.delete_rule(process_name);
    return {true, {}, channel_id};
}

V2CommandResult AudioEngine::auto_setup_defaults() {
    const auto devices = audio::WasapiDeviceEnumerator::enumerate_devices();
    std::string default_render;
    std::string default_capture;
    std::string virtual_mic_render;

    for (const auto& d : devices) {
        if (d.is_default && !d.is_virtual) {
            if (d.is_capture && default_capture.empty()) default_capture = d.id;
            if (!d.is_capture && default_render.empty()) default_render = d.id;
        }
        if (!d.is_capture && d.is_virtual && virtual_mic_render.empty()) {
            virtual_mic_render = d.id;
        }
    }

    if (!default_render.empty()) {
        (void)bind_output_bus("A1", default_render, 1.0f, false);
    }
    if (!default_capture.empty()) {
        (void)bind_channel_source(1, "hardware_input", default_capture);
    }
    if (!virtual_mic_render.empty()) {
        (void)bind_output_bus("B1", virtual_mic_render, 1.0f, false);
    }
    return {true, {}, 0};
}

SoundboardPlayResult AudioEngine::play_soundboard(const std::string& path,
                                                  float gain,
                                                  bool send_a1,
                                                  bool send_b1,
                                                  bool send_b2,
                                                  bool loop) {
    dsp::SoundboardEngine::PlayRequest req{};
    req.wav_path = audio::utf8_to_wide(path);
    req.gain = gain;
    req.send_a1 = send_a1;
    req.send_b1 = send_b1;
    req.send_b2 = send_b2;
    req.loop = loop;
    const auto res = soundboard_.play(req);
    return {res.ok, res.slot, res.duration_ms};
}

bool AudioEngine::stop_soundboard(int slot) {
    soundboard_.stop(slot);
    return true;
}

SoundboardStatus AudioEngine::get_soundboard_status() {
    return {soundboard_.active_voices(), static_cast<uint32_t>(dsp::SoundboardEngine::kMaxVoices)};
}

V2CommandResult AudioEngine::set_spectrum_source(uint32_t channel_id) {
    if (channel_id >= dsp::kDynamicMaxChannels) {
        return {false, "invalid_channel_id", channel_id};
    }
    spectrum_source_ch_.store(channel_id, std::memory_order_release);
    spectrum_reset_pending_.store(true, std::memory_order_release);
    return {true, {}, channel_id};
}

void AudioEngine::on_channel_capture(uint32_t ch_idx,
                                     float* data,
                                     uint32_t frames,
                                     uint32_t channels) noexcept {
    if (data == nullptr || frames == 0 || channels == 0 || ch_idx >= dsp::kDynamicMaxChannels) {
        return;
    }
    auto& ring = channels_[ch_idx].input_ring;
    for (uint32_t i = 0; i < frames; ++i) {
        const float l = data[i * channels];
        const float r = (channels > 1) ? data[i * channels + 1] : l;
        if (!ring.try_push(StereoFrame{l, r})) {
            break;
        }
    }
}

void AudioEngine::on_channel_loopback(uint32_t ch_idx,
                                      const float* data,
                                      uint32_t frames,
                                      uint32_t channels) noexcept {
    if (data == nullptr || frames == 0 || channels == 0 || ch_idx >= dsp::kDynamicMaxChannels) {
        return;
    }
    auto& ring = channels_[ch_idx].input_ring;
    for (uint32_t i = 0; i < frames; ++i) {
        const float l = data[i * channels];
        const float r = (channels > 1) ? data[i * channels + 1] : l;
        if (!ring.try_push(StereoFrame{l, r})) {
            break;
        }
    }
}

void AudioEngine::on_bus_render(size_t bus_idx,
                                float* data,
                                uint32_t frames,
                                uint32_t channels) noexcept {
    if (data == nullptr || frames == 0 || channels == 0 || bus_idx >= dsp::kDynamicMaxBuses) {
        return;
    }
    auto& bus = buses_[bus_idx];
    auto& ring = bus.output_ring;
    for (uint32_t i = 0; i < frames; ++i) {
        StereoFrame f{};
        if (ring.try_pop(f)) {
            bus.last_out_l = f.l;
            bus.last_out_r = f.r;
        } else {
            bus.last_out_l *= 0.96f;
            bus.last_out_r *= 0.96f;
            f.l = bus.last_out_l;
            f.r = bus.last_out_r;
        }
        if (channels == 1) {
            data[i] = 0.5f * (f.l + f.r);
        } else {
            data[i * channels] = f.l;
            data[i * channels + 1] = f.r;
            for (uint32_t c = 2; c < channels; ++c) {
                data[i * channels + c] = 0.0f;
            }
        }
    }
}

} // namespace atriswave
