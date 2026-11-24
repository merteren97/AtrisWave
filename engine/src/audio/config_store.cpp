#include <atriswave/audio/config_store.hpp>
#include <atriswave/audio/com_utils.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shlobj.h>

#include <cctype>
#include <fstream>
#include <sstream>

namespace atriswave::audio {

std::optional<std::filesystem::path> ConfigStore::presets_dir() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data)) ||
        local_app_data == nullptr) {
        return std::nullopt;
    }

    std::filesystem::path dir(local_app_data);
    CoTaskMemFree(local_app_data);
    dir /= L"AtrisWave";
    dir /= L"presets";
    return dir;
}

bool ConfigStore::is_valid_preset_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    if (name == "." || name == "..") {
        return false;
    }
    for (const char ch : name) {
        const auto uch = static_cast<unsigned char>(ch);
        const bool allowed = std::isalnum(uch) != 0 || ch == '_' || ch == '-' || ch == ' ';
        if (!allowed) {
            return false;
        }
    }
    // Disallow leading/trailing whitespace
    if (name.front() == ' ' || name.back() == ' ') {
        return false;
    }
    return true;
}

bool ConfigStore::save_preset(std::string_view name, std::string_view json_payload) {
    if (!is_valid_preset_name(name)) {
        return false;
    }
    const auto dir = presets_dir();
    if (!dir.has_value()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(*dir, ec);
    if (ec) {
        return false;
    }

    const std::filesystem::path file_path = *dir / (std::string(name) + ".json");
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(json_payload.data(), static_cast<std::streamsize>(json_payload.size()));
    return out.good();
}

std::optional<std::string> ConfigStore::load_preset(std::string_view name) {
    if (!is_valid_preset_name(name)) {
        return std::nullopt;
    }
    const auto dir = presets_dir();
    if (!dir.has_value()) {
        return std::nullopt;
    }

    const std::filesystem::path file_path = *dir / (std::string(name) + ".json");
    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> ConfigStore::list_presets() {
    std::vector<std::string> names;
    const auto dir = presets_dir();
    if (!dir.has_value()) {
        return names;
    }

    std::error_code ec;
    if (!std::filesystem::exists(*dir, ec)) {
        return names;
    }

    for (const auto& entry : std::filesystem::directory_iterator(*dir, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            const std::string stem = entry.path().stem().string();
            if (is_valid_preset_name(stem)) {
                names.push_back(stem);
            }
        }
    }
    return names;
}

} // namespace atriswave::audio
