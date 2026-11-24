#include <atriswave/audio/audio_engine.hpp>
#include <atriswave/audio/config_store.hpp>
#include <atriswave/ipc_protocol.h>

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
#include <string_view>

namespace atriswave {
namespace {

dsp::MicDspParams to_dsp_params(const MicDspConfig& cfg) noexcept {
    dsp::MicDspParams p{};
    p.direct_monitor = cfg.direct_monitor;
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
        "{\"direct_monitor\":%s,\"monitor_gain\":%.4f,\"denoise_enabled\":%s,\"denoise_strength\":%.4f,"
        "\"gate_enabled\":%s,\"gate_threshold_db\":%.4f,\"eq_enabled\":%s,\"eq_hpf_hz\":%.4f,"
        "\"eq_low_db\":%.4f,\"eq_mid1_hz\":%.4f,\"eq_mid1_db\":%.4f,\"eq_mid2_hz\":%.4f,"
        "\"eq_mid2_db\":%.4f,\"eq_high_db\":%.4f,\"comp_enabled\":%s,\"comp_threshold_db\":%.4f,"
        "\"comp_ratio\":%.4f,\"comp_makeup_db\":%.4f,\"ducking_enabled\":%s,\"ducking_depth_db\":%.4f,"
        "\"reverb_enabled\":%s,\"reverb_wet\":%.4f,\"reverb_room\":%.4f,\"reverb_damp\":%.4f,"
        "\"reverb_mode\":%u,\"pitch_enabled\":%s,\"pitch_semitones\":%.4f,"
        "\"limiter_enabled\":%s,\"limiter_ceiling_db\":%.4f}",
        c.direct_monitor ? "true" : "false", c.monitor_gain,
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
    if (auto v = find_bool(json, "direct_monitor")) c.direct_monitor = *v;
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

bool AudioEngine::start(void* shutdown_event, bool headless_test) {
    stop();
    shutdown_event_ = shutdown_event;

    // Pre-allocate all RT scratch buffers before starting any audio thread
    mono_in_scratch_.assign(kMaxScratchFrames, 0.0f);
    mon_l_scratch_.assign(kMaxScratchFrames, 0.0f);
    mon_r_scratch_.assign(kMaxScratchFrames, 0.0f);
    bcast_l_scratch_.assign(kMaxScratchFrames, 0.0f);
    bcast_r_scratch_.assign(kMaxScratchFrames, 0.0f);
    loop_l_scratch_.assign(kMaxScratchFrames, 0.0f);
    loop_r_scratch_.assign(kMaxScratchFrames, 0.0f);
    mix_l_scratch_.assign(kMaxScratchFrames, 0.0f);
    mix_r_scratch_.assign(kMaxScratchFrames, 0.0f);

    mic_pipeline_.prepare(48000.0, kMaxScratchFrames);
    mixer_bus_.prepare(48000.0);
    mic_pipeline_.set_params(to_dsp_params(current_mic_cfg_));

    if (!telemetry_.open(ATRISWAVE_TELEMETRY_SHM_NAME_W)) {
        return false;
    }

    engine_state_.store(ATRISWAVE_ENGINE_STATE_RUNNING, std::memory_order_release);

    if (!headless_test) {
        // Attempt to open default capture device gracefully; stay running if unavailable.
        const auto devices = audio::WasapiDeviceEnumerator::enumerate_devices();
        for (const auto& d : devices) {
            if (d.is_capture && d.is_default) {
                (void)select_devices(d.id, "", "");
                break;
            }
        }
    }

    if (!control_server_.start(ATRISWAVE_PIPE_NAME_W, this)) {
        stop();
        return false;
    }

    return true;
}

void AudioEngine::stop() {
    engine_state_.store(ATRISWAVE_ENGINE_STATE_STOPPED, std::memory_order_release);
    monitor_active_.store(false, std::memory_order_release);
    output_active_.store(false, std::memory_order_release);
    loopback_active_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(device_mutex_);
        loopback_capture_.stop();
        capture_stream_.close();
        monitor_stream_.close();
        output_stream_.close();
    }
    publish_telemetry_tick();
    control_server_.stop();
    telemetry_.close();
    shutdown_event_ = nullptr;
}

void AudioEngine::publish_telemetry_tick() noexcept {
    if (!telemetry_.is_open()) {
        return;
    }

    TelemetrySnapshot snap{};
    snap.header.channel_count = static_cast<uint32_t>(dsp::kMixerChannelCount);
    snap.header.sample_rate = sample_rate_.load(std::memory_order_relaxed);
    snap.header.buffer_frames = buffer_frames_.load(std::memory_order_relaxed);
    snap.header.engine_state = engine_state_.load(std::memory_order_acquire);
    snap.header.timestamp_qpc = TelemetryPublisher::query_qpc();
    snap.header.xrun_count = xrun_count_.load(std::memory_order_relaxed);

    mixer_bus_.fill_telemetry(snap.channels, dsp::kMixerChannelCount);
    snap.channels[static_cast<size_t>(dsp::ChannelId::Mic)].gain_reduction_db =
        mic_pipeline_.gain_reduction_db();

    telemetry_.publish(snap);
}

ControlServer::StatusSnapshot AudioEngine::get_status() {
    ControlServer::StatusSnapshot s{};
    s.engine_state = engine_state_.load(std::memory_order_acquire);
    s.sample_rate = sample_rate_.load(std::memory_order_relaxed);
    s.buffer_frames = buffer_frames_.load(std::memory_order_relaxed);
    s.channel_count = static_cast<uint32_t>(dsp::kMixerChannelCount);
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
    if (cfg.channel_id >= dsp::kMixerChannelCount) {
        return false;
    }
    mixer_bus_.set_channel(static_cast<dsp::ChannelId>(cfg.channel_id), cfg.volume, cfg.muted, cfg.solo);
    return true;
}

bool AudioEngine::set_mic_dsp(const MicDspConfig& cfg) {
    {
        std::lock_guard lock(config_mutex_);
        current_mic_cfg_ = cfg;
    }
    mic_pipeline_.set_params(to_dsp_params(cfg));
    mixer_bus_.set_ducking(cfg.ducking_enabled, cfg.ducking_depth_db);
    return true;
}

bool AudioEngine::select_devices(const std::string& input_id,
                                 const std::string& monitor_id,
                                 const std::string& output_id) {
    std::lock_guard lock(device_mutex_);
    monitor_active_.store(false, std::memory_order_release);
    output_active_.store(false, std::memory_order_release);
    capture_stream_.close();
    monitor_stream_.close();
    output_stream_.close();
    monitor_ring_.reset();
    output_ring_.reset();

    bool any_ok = false;

    if (!input_id.empty()) {
        if (capture_stream_.open(input_id, audio::StreamDirection::Capture)) {
            sample_rate_.store(capture_stream_.sample_rate(), std::memory_order_relaxed);
            buffer_frames_.store(capture_stream_.buffer_frames(), std::memory_order_relaxed);
            mic_pipeline_.prepare(static_cast<double>(capture_stream_.sample_rate()), kMaxScratchFrames);
            mixer_bus_.prepare(static_cast<double>(capture_stream_.sample_rate()));
            any_ok = capture_stream_.start([this](float* data, uint32_t frames, uint32_t ch) {
                on_mic_capture(data, frames, ch);
            });
        }
    }

    if (!monitor_id.empty()) {
        if (monitor_stream_.open(monitor_id, audio::StreamDirection::Render)) {
            if (monitor_stream_.start([this](float* data, uint32_t frames, uint32_t ch) {
                    on_monitor_render(data, frames, ch);
                })) {
                monitor_active_.store(true, std::memory_order_release);
                any_ok = true;
            }
        }
    }

    if (!output_id.empty()) {
        if (output_stream_.open(output_id, audio::StreamDirection::Render)) {
            if (output_stream_.start([this](float* data, uint32_t frames, uint32_t ch) {
                    on_output_render(data, frames, ch);
                })) {
                output_active_.store(true, std::memory_order_release);
                any_ok = true;
            }
        }
    }

    return any_ok || (input_id.empty() && monitor_id.empty() && output_id.empty());
}

audio::RouteResult AudioEngine::route_app(uint32_t pid,
                                          const std::string& target_device_id,
                                          bool loopback_capture) {
    if (loopback_capture) {
        std::lock_guard lock(device_mutex_);
        loopback_active_.store(false, std::memory_order_release);
        loopback_capture_.stop();
        const bool ok = loopback_capture_.start(
            pid,
            true,
            [this](const float* data, uint32_t frames, uint32_t ch) {
                on_loopback_capture(data, frames, ch);
            });
        loopback_active_.store(ok, std::memory_order_release);
        return {
            ok,
            ok ? "loopback_capture" : "unsupported",
            ok ? "Process loopback capture active" : "Process loopback capture unavailable or blocked"
        };
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

void AudioEngine::on_mic_capture(float* data, uint32_t frames, uint32_t channels) noexcept {
    if (data == nullptr || frames == 0 || channels == 0) {
        return;
    }
    const size_t n = std::min(static_cast<size_t>(frames), kMaxScratchFrames);
    for (size_t i = 0; i < n; ++i) {
        mono_in_scratch_[i] = data[i * channels];
    }

    mic_pipeline_.process(
        mono_in_scratch_.data(),
        mon_l_scratch_.data(),
        mon_r_scratch_.data(),
        bcast_l_scratch_.data(),
        bcast_r_scratch_.data(),
        n);

    // Pull any captured app loopback audio for ChannelId::Game (channel 1)
    const bool use_loopback = loopback_active_.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        StereoFrame frame{};
        (void)loopback_ring_.try_pop(frame);
        loop_l_scratch_[i] = use_loopback ? frame.l : 0.0f;
        loop_r_scratch_[i] = use_loopback ? frame.r : 0.0f;
    }

    std::array<const float*, dsp::kMixerChannelCount> in_l{};
    std::array<const float*, dsp::kMixerChannelCount> in_r{};
    in_l[static_cast<size_t>(dsp::ChannelId::Game)] = loop_l_scratch_.data();
    in_r[static_cast<size_t>(dsp::ChannelId::Game)] = loop_r_scratch_.data();
    in_l[static_cast<size_t>(dsp::ChannelId::Mic)] = bcast_l_scratch_.data();
    in_r[static_cast<size_t>(dsp::ChannelId::Mic)] = bcast_r_scratch_.data();

    mixer_bus_.process(
        in_l,
        in_r,
        mix_l_scratch_.data(),
        mix_r_scratch_.data(),
        n,
        mic_pipeline_.mic_envelope());

    if (monitor_active_.load(std::memory_order_acquire)) {
        for (size_t i = 0; i < n; ++i) {
            if (!monitor_ring_.try_push(StereoFrame{mon_l_scratch_[i], mon_r_scratch_[i]})) {
                xrun_count_.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }

    if (output_active_.load(std::memory_order_acquire)) {
        for (size_t i = 0; i < n; ++i) {
            if (!output_ring_.try_push(StereoFrame{mix_l_scratch_[i], mix_r_scratch_[i]})) {
                xrun_count_.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
}

void AudioEngine::on_monitor_render(float* data, uint32_t frames, uint32_t channels) noexcept {
    if (data == nullptr || frames == 0 || channels == 0) {
        return;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        StereoFrame frame{};
        (void)monitor_ring_.try_pop(frame);
        if (channels == 1) {
            data[i] = 0.5f * (frame.l + frame.r);
        } else {
            data[i * channels] = frame.l;
            data[i * channels + 1] = frame.r;
            for (uint32_t c = 2; c < channels; ++c) {
                data[i * channels + c] = 0.0f;
            }
        }
    }
}

void AudioEngine::on_output_render(float* data, uint32_t frames, uint32_t channels) noexcept {
    if (data == nullptr || frames == 0 || channels == 0) {
        return;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        StereoFrame frame{};
        (void)output_ring_.try_pop(frame);
        if (channels == 1) {
            data[i] = 0.5f * (frame.l + frame.r);
        } else {
            data[i * channels] = frame.l;
            data[i * channels + 1] = frame.r;
            for (uint32_t c = 2; c < channels; ++c) {
                data[i * channels + c] = 0.0f;
            }
        }
    }
}

void AudioEngine::on_loopback_capture(const float* data, uint32_t frames, uint32_t channels) noexcept {
    if (data == nullptr || frames == 0 || channels == 0) {
        return;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        const float l = data[i * channels];
        const float r = (channels > 1) ? data[i * channels + 1] : l;
        if (!loopback_ring_.try_push(StereoFrame{l, r})) {
            break;
        }
    }
}

} // namespace atriswave
