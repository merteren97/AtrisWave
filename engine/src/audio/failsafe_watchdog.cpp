#include <atriswave/audio/failsafe_watchdog.hpp>
#include <atriswave/audio/com_utils.hpp>
#include <atriswave/audio/wasapi_devices.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace atriswave::audio {
namespace {

bool is_atriswave_name(std::string_view name) noexcept {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.find("atriswave") != std::string::npos;
}

std::optional<std::string> parse_json_str(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t kp = json.find(needle);
    if (kp == std::string_view::npos) return std::nullopt;
    size_t pos = json.find(':', kp + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && (pos + 1) < json.size()) ++pos;
        out.push_back(json[pos]);
        ++pos;
    }
    return (pos < json.size()) ? std::optional<std::string>(out) : std::nullopt;
}

std::optional<bool> parse_json_bool(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t kp = json.find(needle);
    if (kp == std::string_view::npos) return std::nullopt;
    size_t pos = json.find(':', kp + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    const auto tail = json.substr(pos);
    if (tail.starts_with("true")) return true;
    if (tail.starts_with("false")) return false;
    return std::nullopt;
}

// Undocumented IPolicyConfig interface for restoring default Windows audio endpoints
struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
};

constexpr GUID kClsidPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
constexpr GUID kIidPolicyConfig = {
    0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

} // namespace

FailsafeWatchdog::FailsafeWatchdog() {
    if (const auto def = default_snapshot_path(); def.has_value()) {
        snapshot_path_ = *def;
    }
}

FailsafeWatchdog::FailsafeWatchdog(std::filesystem::path snapshot_path)
    : snapshot_path_(std::move(snapshot_path)) {}

std::optional<std::filesystem::path> FailsafeWatchdog::default_snapshot_path() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data)) ||
        local_app_data == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path p(local_app_data);
    CoTaskMemFree(local_app_data);
    p /= L"AtrisWave";
    p /= L"failsafe_defaults.json";
    return p;
}

bool FailsafeWatchdog::arm_snapshot(const FailsafeDefaultsSnapshot& snap) const {
    if (snapshot_path_.empty()) {
        return false;
    }
    std::error_code ec;
    if (snapshot_path_.has_parent_path()) {
        std::filesystem::create_directories(snapshot_path_.parent_path(), ec);
        if (ec) {
            return false;
        }
    }

    std::filesystem::path tmp = snapshot_path_;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << "{\"armed\":" << (snap.armed ? "true" : "false")
            << ",\"render_console_id\":\"" << json_escape(snap.render_console_id)
            << "\",\"render_multimedia_id\":\"" << json_escape(snap.render_multimedia_id)
            << "\",\"render_comm_id\":\"" << json_escape(snap.render_comm_id)
            << "\",\"capture_console_id\":\"" << json_escape(snap.capture_console_id)
            << "\",\"capture_multimedia_id\":\"" << json_escape(snap.capture_multimedia_id)
            << "\",\"capture_comm_id\":\"" << json_escape(snap.capture_comm_id)
            << "\"}\n";
        out.flush();
        if (!out.good()) {
            return false;
        }
    }

    if (MoveFileExW(tmp.c_str(), snapshot_path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool FailsafeWatchdog::arm_current_system_defaults() const {
    const auto devices = WasapiDeviceEnumerator::enumerate_devices();
    FailsafeDefaultsSnapshot snap{};
    snap.armed = true;
    for (const auto& d : devices) {
        if (!d.is_default || is_atriswave_name(d.name)) {
            continue;
        }
        if (d.is_capture) {
            snap.capture_console_id = d.id;
            snap.capture_multimedia_id = d.id;
            snap.capture_comm_id = d.id;
        } else {
            snap.render_console_id = d.id;
            snap.render_multimedia_id = d.id;
            snap.render_comm_id = d.id;
        }
    }
    return arm_snapshot(snap);
}

bool FailsafeWatchdog::disarm() const {
    if (snapshot_path_.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(snapshot_path_, ec);
    return !ec;
}

std::optional<FailsafeDefaultsSnapshot> FailsafeWatchdog::load_snapshot() const {
    if (snapshot_path_.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::exists(snapshot_path_, ec)) {
        return std::nullopt;
    }
    std::ifstream in(snapshot_path_, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();

    FailsafeDefaultsSnapshot snap{};
    snap.armed = parse_json_bool(json, "armed").value_or(false);
    snap.render_console_id = parse_json_str(json, "render_console_id").value_or("");
    snap.render_multimedia_id = parse_json_str(json, "render_multimedia_id").value_or("");
    snap.render_comm_id = parse_json_str(json, "render_comm_id").value_or("");
    snap.capture_console_id = parse_json_str(json, "capture_console_id").value_or("");
    snap.capture_multimedia_id = parse_json_str(json, "capture_multimedia_id").value_or("");
    snap.capture_comm_id = parse_json_str(json, "capture_comm_id").value_or("");
    return snap;
}

bool FailsafeWatchdog::restore_if_needed() const {
    const auto snap = load_snapshot();
    if (!snap.has_value() || !snap->armed) {
        return true;
    }

    const auto devices = WasapiDeviceEnumerator::enumerate_devices();
    auto is_active = [&](const std::string& id) {
        return !id.empty() && std::any_of(devices.begin(), devices.end(), [&](const AudioDeviceInfo& d) {
            return d.id == id;
        });
    };
    auto first_physical = [&](bool capture) -> std::string {
        for (const auto& d : devices) {
            if (d.is_capture == capture && !is_atriswave_name(d.name)) {
                return d.id;
            }
        }
        return {};
    };

    bool current_render_is_atris = false;
    bool current_capture_is_atris = false;
    for (const auto& d : devices) {
        if (d.is_default && is_atriswave_name(d.name)) {
            if (d.is_capture) current_capture_is_atris = true;
            else current_render_is_atris = true;
        }
    }

    bool ok = true;
    auto resolve_target = [&](const std::string& saved, bool capture) -> std::string {
        if (is_active(saved)) {
            return saved;
        }
        return first_physical(capture);
    };

    if (current_render_is_atris) {
        const std::string c_target = resolve_target(snap->render_console_id, false);
        const std::string m_target = resolve_target(snap->render_multimedia_id, false);
        const std::string k_target = resolve_target(snap->render_comm_id, false);
        if (!c_target.empty()) ok = set_system_default_endpoint_role(c_target, static_cast<uint32_t>(eConsole)) && ok;
        if (!m_target.empty()) ok = set_system_default_endpoint_role(m_target, static_cast<uint32_t>(eMultimedia)) && ok;
        if (!k_target.empty()) ok = set_system_default_endpoint_role(k_target, static_cast<uint32_t>(eCommunications)) && ok;
    }
    if (current_capture_is_atris) {
        const std::string c_target = resolve_target(snap->capture_console_id, true);
        const std::string m_target = resolve_target(snap->capture_multimedia_id, true);
        const std::string k_target = resolve_target(snap->capture_comm_id, true);
        if (!c_target.empty()) ok = set_system_default_endpoint_role(c_target, static_cast<uint32_t>(eConsole)) && ok;
        if (!m_target.empty()) ok = set_system_default_endpoint_role(m_target, static_cast<uint32_t>(eMultimedia)) && ok;
        if (!k_target.empty()) ok = set_system_default_endpoint_role(k_target, static_cast<uint32_t>(eCommunications)) && ok;
    }

    (void)disarm();
    return ok;
}

bool FailsafeWatchdog::set_system_default_endpoint_role(const std::string& device_id, uint32_t role) {
    if (device_id.empty()) {
        return false;
    }
    ScopedComInit com;
    if (!com.ok()) {
        return false;
    }
    ComPtr<IPolicyConfig> cfg;
    if (FAILED(CoCreateInstance(
            kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, kIidPolicyConfig, cfg.put_void())) ||
        !cfg) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(device_id);
    return SUCCEEDED(cfg->SetDefaultEndpoint(wide.c_str(), static_cast<ERole>(role)));
}

bool FailsafeWatchdog::set_system_default_endpoint(const std::string& device_id) {
    if (device_id.empty()) {
        return false;
    }
    ScopedComInit com;
    if (!com.ok()) {
        return false;
    }
    ComPtr<IPolicyConfig> cfg;
    if (FAILED(CoCreateInstance(
            kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, kIidPolicyConfig, cfg.put_void())) ||
        !cfg) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(device_id);
    const HRESULT hr1 = cfg->SetDefaultEndpoint(wide.c_str(), eConsole);
    const HRESULT hr2 = cfg->SetDefaultEndpoint(wide.c_str(), eMultimedia);
    const HRESULT hr3 = cfg->SetDefaultEndpoint(wide.c_str(), eCommunications);
    return SUCCEEDED(hr1) && SUCCEEDED(hr2) && SUCCEEDED(hr3);
}

} // namespace atriswave::audio
