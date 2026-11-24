#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atriswave::audio {

struct AudioSessionInfo {
    uint32_t pid{0};
    std::string process_name;
    std::string display_name;
    float volume{1.0f};
    bool muted{false};
    float peak{0.0f};
};

struct RouteResult {
    bool ok{false};
    std::string method{"unsupported"};
    std::string message;
};

class AppRouter final {
public:
    /// Enumerates active Windows render audio sessions via `IAudioSessionManager2`.
    [[nodiscard]] static std::vector<AudioSessionInfo> list_sessions();

    /// Attempts to redirect a process's audio output to `target_device_id` via
    /// the versioned `IAudioPolicyConfigFactory` COM shim.
    /// Returns `method = "policy_config"` on success or `method = "unsupported"` cleanly on failure.
    [[nodiscard]] static RouteResult route_app_to_device(uint32_t pid,
                                                           const std::string& target_device_id);

    /// Sets volume (0.0..1.0) and mute state on all active Windows audio sessions matching `process_name`
    /// via `IAudioSessionManager2` + `ISimpleAudioVolume` (zero-echo driverless control).
    [[nodiscard]] static bool set_app_session_volume_mute(std::string_view process_name,
                                                          float volume,
                                                          bool muted);
};

} // namespace atriswave::audio
