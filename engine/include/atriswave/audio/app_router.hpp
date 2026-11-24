#pragma once

#include <cstdint>
#include <string>
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
};

} // namespace atriswave::audio
