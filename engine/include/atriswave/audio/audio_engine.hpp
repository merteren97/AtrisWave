#pragma once

#include <atriswave/audio/app_router.hpp>
#include <atriswave/audio/driver_bridge.hpp>
#include <atriswave/audio/failsafe_watchdog.hpp>
#include <atriswave/audio/process_loopback.hpp>
#include <atriswave/audio/routing_rules.hpp>
#include <atriswave/audio/wasapi_devices.hpp>
#include <atriswave/audio/wasapi_stream.hpp>
#include <atriswave/control_server.hpp>
#include <atriswave/dsp/dynamic_mixer.hpp>
#include <atriswave/dsp/mic_pipeline.hpp>
#include <atriswave/dsp/pll_resampler.hpp>
#include <atriswave/dsp/soundboard.hpp>
#include <atriswave/dsp/spectrum_analyzer.hpp>
#include <atriswave/spsc_ring_buffer.hpp>
#include <atriswave/telemetry_publisher.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace atriswave {

/// Top-level v2 Audio Engine coordinating:
/// - Dedicated High-Resolution Master Clock Thread (10ms @ 48kHz, MmcssGuard "Pro Audio")
/// - Dynamic 16-channel x 4-bus (A1, A2, B1, B2) mixer with per-channel EQ/Comp/Ducking
/// - Feedback-loop blocker (direct ID + virtual_pair_group) & ProcessLoopback A1/A2 echo lock
/// - Dual-Path Mic DSP, AdaptivePllResampler per non-master stream, Telemetry V2 (704B), ControlServer
class AudioEngine final : public ControlServer::IControlHandler {
public:
    AudioEngine() noexcept = default;
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] bool start(void* shutdown_event, bool headless_test = false);
    void stop();

    void publish_telemetry_tick() noexcept;

    // ControlServer::IControlHandler (Phase 0-2)
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

    [[nodiscard]] audio::DriverStatus get_driver_status() override;
    [[nodiscard]] std::vector<audio::RoutingRule> list_routing_rules() override;
    [[nodiscard]] ControlServer::RoutingRuleApplyResult set_routing_rule(const audio::RoutingRule& rule) override;
    [[nodiscard]] bool delete_routing_rule(const std::string& process_name) override;
    [[nodiscard]] uint32_t apply_routing_rules() override;

    // v2 Dynamic Multi-Channel & Output Bus Matrix
    [[nodiscard]] std::vector<DynamicChannelInfo> list_channels() override;
    [[nodiscard]] V2CommandResult create_channel(const std::string& name,
                                                 const std::string& source_type,
                                                 const std::string& color) override;
    [[nodiscard]] V2CommandResult delete_channel(uint32_t channel_id) override;
    [[nodiscard]] V2CommandResult rename_channel(uint32_t channel_id,
                                                 const std::string& name) override;
    [[nodiscard]] V2CommandResult bind_channel_source(uint32_t channel_id,
                                                      const std::string& source_type,
                                                      const std::string& device_id) override;
    [[nodiscard]] std::vector<OutputBusInfo> list_output_buses() override;
    [[nodiscard]] V2CommandResult bind_output_bus(const std::string& bus_id,
                                                  const std::string& device_id,
                                                  float volume,
                                                  bool muted) override;
    [[nodiscard]] OutputBusMixResult set_output_bus_mix(const std::string& bus_id,
                                                        float volume,
                                                        bool muted) override;
    [[nodiscard]] V2CommandResult set_channel_send(uint32_t channel_id,
                                                   const std::string& bus_id,
                                                   bool enabled,
                                                   float send_gain) override;
    [[nodiscard]] V2CommandResult set_channel_dsp(const ChannelDspConfig& cfg) override;
    [[nodiscard]] V2CommandResult bind_app_to_channel(const std::string& process_name,
                                                      uint32_t channel_id) override;
    [[nodiscard]] V2CommandResult unbind_app_from_channel(const std::string& process_name,
                                                          uint32_t channel_id) override;
    [[nodiscard]] V2CommandResult auto_setup_defaults() override;
    [[nodiscard]] V2CommandResult set_system_default_virtual(bool enabled) override;
    [[nodiscard]] V2CommandResult set_chatmix(float balance) override;

    // v2.1 Soundboard & Spectrum Analyzer
    [[nodiscard]] SoundboardPlayResult play_soundboard(const std::string& path,
                                                       float gain,
                                                       bool send_a1,
                                                       bool send_b1,
                                                       bool send_b2,
                                                       bool loop) override;
    [[nodiscard]] bool stop_soundboard(int slot) override;
    [[nodiscard]] SoundboardStatus get_soundboard_status() override;
    [[nodiscard]] V2CommandResult set_spectrum_source(uint32_t channel_id) override;

private:
    struct StereoFrame {
        float l{0.0f};
        float r{0.0f};
    };

    struct ChannelRuntime {
        bool occupied{false};
        DynamicChannelInfo info{};
        std::string pair_group;
        SpscRingBuffer<StereoFrame, 4096> input_ring;
        dsp::AdaptivePllResampler pll_l;
        std::array<float, 64> leftover_l{};
        std::array<float, 64> leftover_r{};
        size_t leftover_count{0};
        std::atomic<bool> primed{false};
        std::atomic<bool> send_a1_rt{false};
        std::atomic<uint32_t> capture_sr{48000};
        float last_in_l{0.0f};
        float last_in_r{0.0f};
        std::atomic<bool> stream_active{false};
        std::atomic<int32_t> vad_endpoint_id{-1};
        audio::WasapiStream capture_stream;
        audio::ProcessLoopbackCapture loopback_capture;
    };

    struct OutputBusRuntime {
        OutputBusInfo info{};
        std::string pair_group;
        SpscRingBuffer<StereoFrame, 4096> output_ring;
        dsp::AdaptivePllResampler pll_l;
        std::array<float, 64> leftover_l{};
        std::array<float, 64> leftover_r{};
        size_t leftover_count{0};
        std::atomic<uint32_t> render_sr{48000};
        float last_out_l{0.0f};
        float last_out_r{0.0f};
        std::atomic<bool> stream_active{false};
        audio::WasapiStream render_stream;
    };

    void init_default_slots();
    void setup_vad_bridge_if_installed();
    [[nodiscard]] int32_t allocate_vad_render_slot_locked(const std::string& channel_name) const;
    [[nodiscard]] static std::string find_vad_mmdevice_id(uint32_t endpoint_id);
    static void route_process_instances_to_device(const std::string& process_name, const std::string& target_device_id);
    void master_clock_loop();
    void process_master_tick(size_t frames) noexcept;
    void wait_for_master_tick_quiesce() noexcept;
    void check_invalidated_streams();
    void auto_setup_missing_defaults(bool fill_a1, bool fill_mic, bool fill_b1);
    void mark_state_dirty() noexcept;
    void save_persistent_state() const;
    bool load_persistent_state();

    [[nodiscard]] bool would_create_feedback_loop_locked(
        uint32_t ch_idx,
        const std::string& ch_device_id,
        const std::string& ch_pair_group,
        size_t bus_idx,
        const std::string& bus_device_id,
        const std::string& bus_pair_group) const;

    [[nodiscard]] static int bus_index_from_id(const std::string& bus_id) noexcept;
    [[nodiscard]] std::string resolve_pair_group(const std::string& device_id) const;

    void on_channel_capture(uint32_t ch_idx, float* data, uint32_t frames, uint32_t channels) noexcept;
    void on_channel_loopback(uint32_t ch_idx, const float* data, uint32_t frames, uint32_t channels) noexcept;
    void on_bus_render(size_t bus_idx, float* data, uint32_t frames, uint32_t channels) noexcept;

    void launch_watchdog_if_present() noexcept;

    void* shutdown_event_{nullptr};
    void* master_stop_event_{nullptr};
    void* watchdog_process_{nullptr};
    std::atomic<bool> master_running_{false};
    std::thread master_thread_;

    TelemetryPublisher telemetry_;
    ControlServer control_server_;

    dsp::MicPipeline mic_pipeline_;
    dsp::DynamicMixer dynamic_mixer_;
    dsp::SpectrumAnalyzer spectrum_analyzer_;
    dsp::SoundboardEngine soundboard_;
    std::atomic<uint32_t> spectrum_source_ch_{1};
    std::atomic<bool> spectrum_reset_pending_{false};

    static constexpr size_t kMasterTickFrames = 480; // 10ms @ 48kHz
    std::array<std::vector<float>, dsp::kDynamicMaxChannels> ch_in_l_;
    std::array<std::vector<float>, dsp::kDynamicMaxChannels> ch_in_r_;
    std::vector<float> pll_in_l_;
    std::vector<float> pll_in_r_;
    std::vector<float> pll_out_l_;
    std::vector<float> pll_out_r_;
    std::vector<float> mic_mono_scratch_;
    std::vector<float> mic_mon_l_;
    std::vector<float> mic_mon_r_;
    std::vector<float> mic_bcast_l_;
    std::vector<float> mic_bcast_r_;
    std::array<std::vector<float>, dsp::kDynamicMaxBuses> bus_out_l_;
    std::array<std::vector<float>, dsp::kDynamicMaxBuses> bus_out_r_;

    mutable std::mutex state_mutex_;
    std::mutex stream_config_mutex_;
    std::array<ChannelRuntime, dsp::kDynamicMaxChannels> channels_{};
    std::array<OutputBusRuntime, dsp::kDynamicMaxBuses> buses_{};

    mutable std::mutex config_mutex_;
    MicDspConfig current_mic_cfg_{};
    audio::RoutingRulesStore routing_rules_;
    audio::FailsafeWatchdog failsafe_;
    audio::DriverBridgeClient vad_client_;
    std::atomic<bool> vad_b1_active_{false};
    std::atomic<bool> vad_b2_active_{false};
    std::array<float, kMasterTickFrames * 2> vad_pcm_scratch_{};

    std::atomic<uint32_t> engine_state_{ATRISWAVE_ENGINE_STATE_STOPPED};
    std::atomic<uint32_t> sample_rate_{48000};
    std::atomic<uint32_t> buffer_frames_{480};
    std::atomic<uint32_t> xrun_count_{0};
    std::atomic<float> cpu_load_percent_{0.0f};
    std::atomic<float> chatmix_balance_{0.0f};
    std::atomic<uint64_t> master_tick_epoch_{0};
    std::atomic<bool> state_dirty_{false};
    std::atomic<bool> loading_state_{false};
    bool headless_mode_{false};
    uint64_t last_recovery_check_ms_{0};
    uint64_t last_state_save_ms_{0};
};

} // namespace atriswave
