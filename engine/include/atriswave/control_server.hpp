#pragma once

#include <atriswave/audio/app_router.hpp>
#include <atriswave/audio/driver_bridge.hpp>
#include <atriswave/audio/routing_rules.hpp>
#include <atriswave/audio/wasapi_devices.hpp>
#include <atriswave/ipc_protocol.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace atriswave {

struct ChannelConfig {
    uint32_t channel_id{0};
    float volume{1.0f};
    bool muted{false};
    bool solo{false};
};

struct MicDspConfig {
    bool bypass_all{false};
    bool direct_monitor{false};
    bool monitor_processed{true};
    float monitor_gain{1.0f};
    bool denoise_enabled{false};
    float denoise_strength{0.35f};
    bool gate_enabled{false};
    float gate_threshold_db{-45.0f};
    bool eq_enabled{true};
    float eq_hpf_hz{75.0f};
    float eq_low_db{0.0f};
    float eq_mid1_hz{500.0f};
    float eq_mid1_db{0.0f};
    float eq_mid2_hz{2500.0f};
    float eq_mid2_db{0.0f};
    float eq_high_db{0.0f};
    bool comp_enabled{false};
    float comp_threshold_db{-20.0f};
    float comp_ratio{2.5f};
    float comp_makeup_db{1.5f};
    bool ducking_enabled{false};
    float ducking_depth_db{-6.0f};
    bool reverb_enabled{false};
    float reverb_wet{0.15f};
    float reverb_room{0.4f};
    float reverb_damp{0.5f};
    uint32_t reverb_mode{0};
    bool pitch_enabled{false};
    float pitch_semitones{0.0f};
    bool limiter_enabled{true};
    float limiter_ceiling_db{-1.0f};
};

struct DynamicChannelInfo {
    uint32_t channel_id{0};
    std::string name;
    std::string source_type{"app_group"};
    std::string device_id;
    std::string color{"#6366F1"};
    bool is_system{false};
    float volume{1.0f};
    bool muted{false};
    bool solo{false};
    float pan{0.0f};
    bool send_a1{false};
    bool send_a2{false};
    bool send_b1{false};
    bool send_b2{false};
    std::vector<std::string> bound_apps;
    bool echo_locked_a1_a2{false};
};

struct OutputBusInfo {
    std::string bus_id;
    std::string role;
    std::string name;
    std::string device_id;
    std::string device_name;
    bool is_virtual{false};
    float volume{1.0f};
    bool muted{false};
    bool active{false};
};

struct ChannelDspConfig {
    uint32_t channel_id{0};
    float volume{1.0f};
    bool muted{false};
    bool solo{false};
    float pan{0.0f};
    bool eq_enabled{false};
    float eq_hpf_hz{30.0f};
    float eq_low_db{0.0f};
    float eq_mid1_hz{500.0f};
    float eq_mid1_db{0.0f};
    float eq_mid2_hz{2500.0f};
    float eq_mid2_db{0.0f};
    float eq_high_db{0.0f};
    bool comp_enabled{false};
    float comp_threshold_db{-18.0f};
    float comp_ratio{3.0f};
    float comp_makeup_db{0.0f};
    int32_t duck_source_id{-1};
    float duck_depth_db{-12.0f};
};

struct V2CommandResult {
    bool ok{false};
    std::string error;
    uint32_t channel_id{0};
};

struct SoundboardPlayResult {
    bool playing{false};
    int slot{-1};
    uint32_t duration_ms{0};
};

struct SoundboardStatus {
    uint32_t active_voices{0};
    uint32_t max_voices{8};
};

struct OutputBusMixResult {
    bool ok{false};
    std::string bus_id;
    float volume{1.0f};
    bool muted{false};
    std::string error;
};

/// Non-RT Win32 Named Pipe control server (`\\.\pipe\AtrisWaveEngine`).
/// Restricted to the current user via an explicit DACL (fails closed if DACL creation fails).
/// Rejects remote SMB clients (`PIPE_REJECT_REMOTE_CLIENTS`) and caps pending lines at 64 KiB.
class ControlServer final {
public:
    struct StatusSnapshot {
        uint32_t engine_state{ATRISWAVE_ENGINE_STATE_STOPPED};
        uint32_t sample_rate{48000};
        uint32_t buffer_frames{128};
        uint32_t channel_count{6};
    };

    struct RoutingRuleApplyResult {
        bool saved{false};
        bool applied{false};
        std::string method{"none"};
    };

    class IControlHandler {
    public:
        virtual ~IControlHandler() = default;
        [[nodiscard]] virtual StatusSnapshot get_status() = 0;
        virtual void on_shutdown() = 0;
        [[nodiscard]] virtual std::vector<audio::AudioDeviceInfo> list_devices() = 0;
        [[nodiscard]] virtual std::vector<audio::AudioSessionInfo> list_sessions() = 0;
        [[nodiscard]] virtual bool set_channel(const ChannelConfig& cfg) = 0;
        [[nodiscard]] virtual bool set_mic_dsp(const MicDspConfig& cfg) = 0;
        [[nodiscard]] virtual bool select_devices(const std::string& input_id,
                                                  const std::string& monitor_id,
                                                  const std::string& output_id) = 0;
        [[nodiscard]] virtual audio::RouteResult route_app(uint32_t pid,
                                                           const std::string& target_device_id,
                                                           bool loopback_capture) = 0;
        [[nodiscard]] virtual bool save_preset(const std::string& name) = 0;
        [[nodiscard]] virtual std::optional<MicDspConfig> load_preset(const std::string& name) = 0;
        [[nodiscard]] virtual std::vector<std::string> list_presets() = 0;

        // Phase 2: Virtual Audio Driver & Persistent Routing Rules
        [[nodiscard]] virtual audio::DriverStatus get_driver_status() = 0;
        [[nodiscard]] virtual std::vector<audio::RoutingRule> list_routing_rules() = 0;
        [[nodiscard]] virtual RoutingRuleApplyResult set_routing_rule(const audio::RoutingRule& rule) = 0;
        [[nodiscard]] virtual bool delete_routing_rule(const std::string& process_name) = 0;
        [[nodiscard]] virtual uint32_t apply_routing_rules() = 0;

        // v2 Dynamic Multi-Channel & Output Bus Matrix
        [[nodiscard]] virtual std::vector<DynamicChannelInfo> list_channels() = 0;
        [[nodiscard]] virtual V2CommandResult create_channel(const std::string& name,
                                                             const std::string& source_type,
                                                             const std::string& color) = 0;
        [[nodiscard]] virtual V2CommandResult delete_channel(uint32_t channel_id) = 0;
        [[nodiscard]] virtual V2CommandResult rename_channel(uint32_t channel_id,
                                                             const std::string& name) = 0;
        [[nodiscard]] virtual V2CommandResult bind_channel_source(uint32_t channel_id,
                                                                  const std::string& source_type,
                                                                  const std::string& device_id) = 0;
        [[nodiscard]] virtual std::vector<OutputBusInfo> list_output_buses() = 0;
        [[nodiscard]] virtual V2CommandResult bind_output_bus(const std::string& bus_id,
                                                              const std::string& device_id,
                                                              float volume,
                                                              bool muted) = 0;
        [[nodiscard]] virtual OutputBusMixResult set_output_bus_mix(const std::string& bus_id,
                                                                    float volume,
                                                                    bool muted) = 0;
        [[nodiscard]] virtual V2CommandResult set_channel_send(uint32_t channel_id,
                                                               const std::string& bus_id,
                                                               bool enabled,
                                                               float send_gain) = 0;
        [[nodiscard]] virtual V2CommandResult set_channel_dsp(const ChannelDspConfig& cfg) = 0;
        [[nodiscard]] virtual V2CommandResult bind_app_to_channel(const std::string& process_name,
                                                                  uint32_t channel_id) = 0;
        [[nodiscard]] virtual V2CommandResult unbind_app_from_channel(const std::string& process_name,
                                                                      uint32_t channel_id) = 0;
        [[nodiscard]] virtual V2CommandResult auto_setup_defaults() = 0;
        [[nodiscard]] virtual V2CommandResult set_system_default_virtual(bool /*enabled*/) {
            return {false, "unsupported", 0};
        }

        // v2.1 Soundboard & Spectrum Analyzer
        [[nodiscard]] virtual SoundboardPlayResult play_soundboard(const std::string& path,
                                                                   float gain,
                                                                   bool send_a1,
                                                                   bool send_b1,
                                                                   bool send_b2,
                                                                   bool loop) = 0;
        [[nodiscard]] virtual bool stop_soundboard(int slot) = 0;
        [[nodiscard]] virtual SoundboardStatus get_soundboard_status() = 0;
        [[nodiscard]] virtual V2CommandResult set_spectrum_source(uint32_t channel_id) = 0;
    };

    using StatusProvider = std::function<StatusSnapshot()>;
    using ShutdownCallback = std::function<void()>;

    ControlServer() noexcept;
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;
    ControlServer(ControlServer&&) = delete;
    ControlServer& operator=(ControlServer&&) = delete;

    /// Starts the named pipe server with a full Phase 1 `IControlHandler`.
    [[nodiscard]] bool start(const wchar_t* pipe_name, IControlHandler* handler);

    /// Legacy/Phase 0 overload with built-in ping/status/shutdown handler.
    [[nodiscard]] bool start(const wchar_t* pipe_name = ATRISWAVE_PIPE_NAME_W,
                             StatusProvider status_provider = {},
                             ShutdownCallback on_shutdown = {});

    void stop();

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    struct ClientSession;

    void server_loop();
    void handle_client_pipe(void* raw_pipe);
    void reap_finished_clients_locked();
    [[nodiscard]] std::string handle_command_line(const std::string& line, bool& should_shutdown);

    std::wstring pipe_name_;
    std::unique_ptr<IControlHandler> owned_default_handler_;
    IControlHandler* handler_{nullptr};

    void* stop_event_{nullptr};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex clients_mutex_;
    std::vector<std::unique_ptr<ClientSession>> clients_;
};

} // namespace atriswave
