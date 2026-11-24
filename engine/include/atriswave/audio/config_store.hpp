#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atriswave::audio {

/// Persistent JSON preset store under `%LOCALAPPDATA%\AtrisWave\presets\`.
/// Validates preset names to prevent path traversal.
class ConfigStore final {
public:
    [[nodiscard]] static std::optional<std::filesystem::path> presets_dir();
    [[nodiscard]] static bool is_valid_preset_name(std::string_view name) noexcept;

    [[nodiscard]] static bool save_preset(std::string_view name, std::string_view json_payload);
    [[nodiscard]] static std::optional<std::string> load_preset(std::string_view name);
    [[nodiscard]] static std::vector<std::string> list_presets();
};

} // namespace atriswave::audio
