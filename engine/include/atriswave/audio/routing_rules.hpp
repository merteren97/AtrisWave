#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atriswave::audio {

struct RoutingRule {
    std::string process_name;
    uint32_t channel_id{1};
    std::string target_device_id;
    bool loopback_capture{false};
};

/// Persistent per-app routing rules store (`%LOCALAPPDATA%\AtrisWave\routing_rules.json` by default).
/// Uses case-insensitive matching on Windows executable names and atomic temp-file replacement.
class RoutingRulesStore final {
public:
    RoutingRulesStore();
    explicit RoutingRulesStore(std::filesystem::path storage_path);

    [[nodiscard]] static std::optional<std::filesystem::path> default_storage_path();

    [[nodiscard]] bool set_rule(const RoutingRule& rule);
    [[nodiscard]] bool delete_rule(std::string_view process_name);
    [[nodiscard]] std::vector<RoutingRule> list_rules() const;
    [[nodiscard]] std::optional<RoutingRule> find_rule(std::string_view process_name) const;

    [[nodiscard]] const std::filesystem::path& storage_path() const noexcept { return storage_path_; }

private:
    void load_locked();
    [[nodiscard]] bool save_locked() const;
    [[nodiscard]] static bool equals_ignore_case(std::string_view a, std::string_view b) noexcept;

    std::filesystem::path storage_path_;
    mutable std::mutex mutex_;
    std::vector<RoutingRule> rules_;
};

} // namespace atriswave::audio
