#pragma once

#include <atriswave/ipc_protocol.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace atriswave {

/// Non-RT Win32 Named Pipe control server (`\\.\pipe\AtrisWaveEngine`).
/// Restricted to the current user via an explicit DACL (fails closed if DACL creation fails).
/// Handles newline-delimited UTF-8 JSON commands: `ping`, `get_status`, `shutdown`.
class ControlServer final {
public:
    struct StatusSnapshot {
        uint32_t engine_state{ATRISWAVE_ENGINE_STATE_STOPPED};
        uint32_t sample_rate{48000};
        uint32_t buffer_frames{128};
        uint32_t channel_count{6};
    };

    using StatusProvider = std::function<StatusSnapshot()>;
    using ShutdownCallback = std::function<void()>;

    ControlServer() noexcept = default;
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;
    ControlServer(ControlServer&&) = delete;
    ControlServer& operator=(ControlServer&&) = delete;

    /// Starts the named pipe server on a background thread.
    /// Returns false if the pipe security descriptor cannot be constructed or thread fails to start.
    [[nodiscard]] bool start(const wchar_t* pipe_name = ATRISWAVE_PIPE_NAME_W,
                             StatusProvider status_provider = {},
                             ShutdownCallback on_shutdown = {});

    /// Signals the server to stop and joins the background thread.
    void stop();

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    void server_loop();
    [[nodiscard]] std::string handle_command_line(const std::string& line, bool& should_shutdown);

    std::wstring pipe_name_;
    StatusProvider status_provider_;
    ShutdownCallback on_shutdown_;

    void* stop_event_{nullptr};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace atriswave
