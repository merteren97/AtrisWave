#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace atriswave::audio {

struct FailsafeDefaultsSnapshot {
    bool armed{false};
    std::string render_console_id;
    std::string render_multimedia_id;
    std::string render_comm_id;
    std::string capture_console_id;
    std::string capture_multimedia_id;
    std::string capture_comm_id;
};

/// 3-Layer Failsafe Default-Device Recovery:
/// - Persists pre-hijack physical Windows default devices to `%LOCALAPPDATA%\AtrisWave\failsafe_defaults.json`
/// - Respects manual user overrides (only restores if current default is still an AtrisWave endpoint)
/// - Verifies `DEVICE_STATE_ACTIVE` and falls back to first active non-AtrisWave physical device if unplugged
/// - Disarms cleanly on graceful shutdown
class FailsafeWatchdog final {
public:
    FailsafeWatchdog();
    explicit FailsafeWatchdog(std::filesystem::path snapshot_path);

    [[nodiscard]] static std::optional<std::filesystem::path> default_snapshot_path();

    [[nodiscard]] bool arm_snapshot(const FailsafeDefaultsSnapshot& snap) const;
    [[nodiscard]] bool arm_current_system_defaults() const;
    [[nodiscard]] bool disarm() const;
    [[nodiscard]] std::optional<FailsafeDefaultsSnapshot> load_snapshot() const;

    /// Checks whether an armed snapshot exists and restores physical defaults if needed.
    [[nodiscard]] bool restore_if_needed() const;

    [[nodiscard]] const std::filesystem::path& snapshot_path() const noexcept { return snapshot_path_; }

    /// Sets the system default audio endpoint for all 3 ERole values via IPolicyConfig.
    [[nodiscard]] static bool set_system_default_endpoint(const std::string& device_id);

    /// Sets the system default audio endpoint for a specific ERole (0=eConsole, 1=eMultimedia, 2=eCommunications).
    [[nodiscard]] static bool set_system_default_endpoint_role(const std::string& device_id, uint32_t role);

private:
    std::filesystem::path snapshot_path_;
};

} // namespace atriswave::audio
