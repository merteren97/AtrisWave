#pragma once

#include <atriswave/audio/app_router.hpp>
#include <atriswave/audio/process_loopback.hpp>
#include <atriswave/audio/wasapi_devices.hpp>
#include <atriswave/audio/wasapi_stream.hpp>
#include <atriswave/control_server.hpp>
#include <atriswave/dsp/mic_pipeline.hpp>
#include <atriswave/dsp/mixer_bus.hpp>
#include <atriswave/spsc_ring_buffer.hpp>
#include <atriswave/telemetry_publisher.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace atriswave {

/// Top-level Phase 1 Audio Engine coordinating WASAPI streams, Dual-Path Mic DSP,
/// 6-channel MixerBus, Process Loopback capture, TelemetryPublisher, and ControlServer.
class AudioEngine final : public ControlServer::IControlHandler {
public:
    AudioEngine() noexcept = default;
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] bool start(void* shutdown_event, bool headless_test = false);
    void stop();

    void publish_telemetry_tick() noexcept;

    // ControlServer::IControlHandler
    [[nodiscard]] ControlServer::StatusSnapshot get_status() override;
    void on_shutdown() override;
    [[nodiscard]] std::vector<audio::AudioDeviceInfo> list_devices() override;
    [[nodiscard]] std::vector<audio::AudioSessionInfo> list_sessions() override;
    [[nodiscard]] bool set_channel(const ChannelConfig& cfg) override;
    [[nodiscard]] bool set_mic_dsp(const MicDspConfig& cfg) override;
    [[nodiscard]] bool select_devices(const std::string& input_id,
                                      const std::string& monitor_id,
                                      const std::string& output_id) override;
    [[nodiscard]] audio::RouteResult route_app(uint32_t pid,
                                               const std::string& target_device_id,
                                               bool loopback_capture) override;
    [[nodiscard]] bool save_preset(const std::string& name) override;
    [[nodiscard]] std::optional<MicDspConfig> load_preset(const std::string& name) override;
    [[nodiscard]] std::vector<std::string> list_presets() override;

private:
    void on_mic_capture(float* data, uint32_t frames, uint32_t channels) noexcept;
    void on_monitor_render(float* data, uint32_t frames, uint32_t channels) noexcept;
    void on_output_render(float* data, uint32_t frames, uint32_t channels) noexcept;
    void on_loopback_capture(const float* data, uint32_t frames, uint32_t channels) noexcept;

    void* shutdown_event_{nullptr};
    TelemetryPublisher telemetry_;
    ControlServer control_server_;

    dsp::MicPipeline mic_pipeline_;
    dsp::MixerBus mixer_bus_;

    struct StereoFrame {
        float l{0.0f};
        float r{0.0f};
    };

    // Lock-free SPSC stereo ring buffers between capture/loopback and render threads
    SpscRingBuffer<StereoFrame, 16384> monitor_ring_;
    SpscRingBuffer<StereoFrame, 16384> output_ring_;
    SpscRingBuffer<StereoFrame, 16384> loopback_ring_;

    std::atomic<bool> monitor_active_{false};
    std::atomic<bool> output_active_{false};
    std::atomic<bool> loopback_active_{false};

    // Pre-allocated RT scratch buffers (sized for up to 4096 frames)
    static constexpr size_t kMaxScratchFrames = 4096;
    std::vector<float> mono_in_scratch_;
    std::vector<float> mon_l_scratch_;
    std::vector<float> mon_r_scratch_;
    std::vector<float> bcast_l_scratch_;
    std::vector<float> bcast_r_scratch_;
    std::vector<float> loop_l_scratch_;
    std::vector<float> loop_r_scratch_;
    std::vector<float> mix_l_scratch_;
    std::vector<float> mix_r_scratch_;

    std::mutex device_mutex_;
    audio::WasapiStream capture_stream_;
    audio::WasapiStream monitor_stream_;
    audio::WasapiStream output_stream_;
    audio::ProcessLoopbackCapture loopback_capture_;

    std::mutex config_mutex_;
    MicDspConfig current_mic_cfg_{};

    std::atomic<uint32_t> engine_state_{ATRISWAVE_ENGINE_STATE_STOPPED};
    std::atomic<uint32_t> sample_rate_{48000};
    std::atomic<uint32_t> buffer_frames_{128};
    std::atomic<uint32_t> xrun_count_{0};
};

} // namespace atriswave
