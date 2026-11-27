#include <atriswave/control_server.hpp>
#include <atriswave/audio/com_utils.hpp>
#include <atriswave/audio/config_store.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <sddl.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atriswave {
namespace {

class DefaultControlHandler final : public ControlServer::IControlHandler {
public:
    DefaultControlHandler(ControlServer::StatusProvider status_provider,
                          ControlServer::ShutdownCallback on_shutdown)
        : status_provider_(std::move(status_provider))
        , on_shutdown_(std::move(on_shutdown)) {}

    ControlServer::StatusSnapshot get_status() override {
        if (status_provider_) {
            return status_provider_();
        }
        return {};
    }

    void on_shutdown() override {
        if (on_shutdown_) {
            on_shutdown_();
        }
    }

    std::vector<audio::AudioDeviceInfo> list_devices() override {
        return audio::WasapiDeviceEnumerator::enumerate_devices(false);
    }

    std::vector<audio::AudioSessionInfo> list_sessions() override {
        return audio::AppRouter::list_sessions();
    }

    bool set_channel(const ChannelConfig& /*cfg*/) override { return true; }
    bool set_mic_dsp(const MicDspConfig& /*cfg*/) override { return true; }
    bool select_devices(const std::string&, const std::string&, const std::string&) override {
        return true;
    }

    audio::RouteResult route_app(uint32_t pid,
                                 const std::string& target_device_id,
                                 bool /*loopback_capture*/) override {
        return audio::AppRouter::route_app_to_device(pid, target_device_id);
    }

    bool save_preset(const std::string& name) override {
        return audio::ConfigStore::save_preset(name, "{}");
    }

    std::optional<MicDspConfig> load_preset(const std::string& name) override {
        const auto data = audio::ConfigStore::load_preset(name);
        if (!data.has_value()) {
            return std::nullopt;
        }
        return MicDspConfig{};
    }

    std::vector<std::string> list_presets() override {
        return audio::ConfigStore::list_presets();
    }

    audio::DriverStatus get_driver_status() override {
        return audio::DriverBridge::probe_driver();
    }

    std::vector<audio::RoutingRule> list_routing_rules() override {
        return {};
    }

    ControlServer::RoutingRuleApplyResult set_routing_rule(const audio::RoutingRule&) override {
        return {};
    }

    bool delete_routing_rule(const std::string&) override {
        return false;
    }

    uint32_t apply_routing_rules() override {
        return 0;
    }

    std::vector<DynamicChannelInfo> list_channels() override { return {}; }
    V2CommandResult create_channel(const std::string&, const std::string&, const std::string&) override {
        return {false, "unsupported", 0};
    }
    V2CommandResult delete_channel(uint32_t) override { return {false, "unsupported", 0}; }
    V2CommandResult rename_channel(uint32_t, const std::string&) override { return {false, "unsupported", 0}; }
    V2CommandResult bind_channel_source(uint32_t, const std::string&, const std::string&) override {
        return {false, "unsupported", 0};
    }
    std::vector<OutputBusInfo> list_output_buses() override { return {}; }
    V2CommandResult bind_output_bus(const std::string&, const std::string&, float, bool) override {
        return {false, "unsupported", 0};
    }
    OutputBusMixResult set_output_bus_mix(const std::string& bus_id, float volume, bool muted) override {
        return {true, bus_id, volume, muted, {}};
    }
    V2CommandResult set_channel_send(uint32_t, const std::string&, bool, float) override {
        return {false, "unsupported", 0};
    }
    V2CommandResult set_channel_dsp(const ChannelDspConfig&) override { return {false, "unsupported", 0}; }
    V2CommandResult bind_app_to_channel(const std::string&, uint32_t) override {
        return {false, "unsupported", 0};
    }
    V2CommandResult unbind_app_from_channel(const std::string&, uint32_t) override {
        return {false, "unsupported", 0};
    }
    V2CommandResult auto_setup_defaults() override { return {false, "unsupported", 0}; }

    SoundboardPlayResult play_soundboard(const std::string&, float, bool, bool, bool, bool) override {
        return {};
    }
    bool stop_soundboard(int) override { return false; }
    SoundboardStatus get_soundboard_status() override { return {}; }
    V2CommandResult set_spectrum_source(uint32_t) override { return {false, "unsupported", 0}; }

private:
    ControlServer::StatusProvider status_provider_;
    ControlServer::ShutdownCallback on_shutdown_;
};

class ScopedHandle final {
public:
    explicit ScopedHandle(HANDLE h = nullptr) noexcept : handle_(h) {}
    ~ScopedHandle() noexcept { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    void reset(HANDLE h = nullptr) noexcept {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = h;
    }

private:
    HANDLE handle_{nullptr};
};

class ScopedLocalAlloc final {
public:
    explicit ScopedLocalAlloc(void* ptr = nullptr) noexcept : ptr_(ptr) {}
    ~ScopedLocalAlloc() noexcept { reset(); }

    ScopedLocalAlloc(const ScopedLocalAlloc&) = delete;
    ScopedLocalAlloc& operator=(const ScopedLocalAlloc&) = delete;

    [[nodiscard]] void* get() const noexcept { return ptr_; }

    void reset() noexcept {
        if (ptr_ != nullptr) {
            LocalFree(ptr_);
            ptr_ = nullptr;
        }
    }

private:
    void* ptr_{nullptr};
};

/// Builds a security descriptor granting Generic All ONLY to the current process user SID.
/// Fails closed (returns false) on any error.
bool create_current_user_sd(PSECURITY_DESCRIPTOR& sd_out) noexcept {
    sd_out = nullptr;

    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return false;
    }
    ScopedHandle token(raw_token);

    DWORD needed = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0) {
        return false;
    }

    std::vector<BYTE> buffer(needed);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), needed, &needed)) {
        return false;
    }

    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    if (token_user == nullptr || token_user->User.Sid == nullptr) {
        return false;
    }

    LPWSTR sid_string = nullptr;
    if (!ConvertSidToStringSidW(token_user->User.Sid, &sid_string)) {
        return false;
    }
    ScopedLocalAlloc sid_guard(sid_string);

    std::wstring sddl = L"D:(A;;GA;;;";
    sddl += sid_string;
    sddl += L")";

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &sd_out,
            nullptr)) {
        sd_out = nullptr;
        return false;
    }

    return sd_out != nullptr;
}

void append_utf8_codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

bool parse_hex4(std::string_view s, size_t pos, uint32_t& out) noexcept {
    if (pos + 4 > s.size()) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) {
        const char ch = s[pos + i];
        v <<= 4;
        if (ch >= '0' && ch <= '9') {
            v |= static_cast<uint32_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            v |= static_cast<uint32_t>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            v |= static_cast<uint32_t>(ch - 'A' + 10);
        } else {
            return false;
        }
    }
    out = v;
    return true;
}

std::optional<size_t> find_json_value_start(std::string_view json, std::string_view key) {
    size_t i = 0;
    while (i < json.size()) {
        if (json[i] != '"') {
            ++i;
            continue;
        }
        const size_t str_start = i + 1;
        size_t j = str_start;
        bool has_escape = false;
        while (j < json.size() && json[j] != '"') {
            if (json[j] == '\\' && (j + 1) < json.size()) {
                has_escape = true;
                j += 2;
            } else {
                ++j;
            }
        }
        if (j >= json.size()) {
            return std::nullopt;
        }
        const std::string_view token = json.substr(str_start, j - str_start);
        size_t after = j + 1;
        while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after]))) {
            ++after;
        }
        if (!has_escape && token == key && after < json.size() && json[after] == ':') {
            size_t val_pos = after + 1;
            while (val_pos < json.size() && std::isspace(static_cast<unsigned char>(json[val_pos]))) {
                ++val_pos;
            }
            return val_pos;
        }
        i = j + 1;
    }
    return std::nullopt;
}

std::optional<std::string> extract_json_string(std::string_view json, std::string_view key) {
    auto pos_opt = find_json_value_start(json, key);
    if (!pos_opt.has_value()) {
        return std::nullopt;
    }
    size_t pos = *pos_opt;
    if (pos >= json.size() || json[pos] != '"') {
        return std::nullopt;
    }
    ++pos;

    std::string value;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && (pos + 1) < json.size()) {
            ++pos;
            const char esc = json[pos];
            switch (esc) {
            case '"':  value.push_back('"'); ++pos; break;
            case '\\': value.push_back('\\'); ++pos; break;
            case '/':  value.push_back('/'); ++pos; break;
            case 'b':  value.push_back('\b'); ++pos; break;
            case 'f':  value.push_back('\f'); ++pos; break;
            case 'n':  value.push_back('\n'); ++pos; break;
            case 'r':  value.push_back('\r'); ++pos; break;
            case 't':  value.push_back('\t'); ++pos; break;
            case 'u': {
                uint32_t cp1 = 0;
                if (parse_hex4(json, pos + 1, cp1)) {
                    pos += 5;
                    if (cp1 >= 0xD800u && cp1 <= 0xDBFFu &&
                        (pos + 6) <= json.size() && json[pos] == '\\' && json[pos + 1] == 'u') {
                        uint32_t cp2 = 0;
                        if (parse_hex4(json, pos + 2, cp2) && cp2 >= 0xDC00u && cp2 <= 0xDFFFu) {
                            const uint32_t combined = 0x10000u + ((cp1 - 0xD800u) << 10) + (cp2 - 0xDC00u);
                            append_utf8_codepoint(value, combined);
                            pos += 6;
                            break;
                        }
                    }
                    append_utf8_codepoint(value, cp1);
                } else {
                    value.push_back('u');
                    ++pos;
                }
                break;
            }
            default:
                value.push_back(esc);
                ++pos;
                break;
            }
        } else {
            value.push_back(json[pos]);
            ++pos;
        }
    }
    if (pos >= json.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<uint64_t> extract_json_uint(std::string_view json, std::string_view key) {
    auto pos_opt = find_json_value_start(json, key);
    if (!pos_opt.has_value()) {
        return std::nullopt;
    }
    size_t pos = *pos_opt;
    if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) {
        return std::nullopt;
    }

    uint64_t value = 0;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10ULL + static_cast<uint64_t>(json[pos] - '0');
        ++pos;
    }
    return value;
}

std::optional<double> extract_json_number(std::string_view json, std::string_view key) {
    auto pos_opt = find_json_value_start(json, key);
    if (!pos_opt.has_value()) {
        return std::nullopt;
    }
    const size_t pos = *pos_opt;
    if (pos >= json.size()) {
        return std::nullopt;
    }
    const char* begin = json.data() + pos;
    char* end = nullptr;
    const double val = std::strtod(begin, &end);
    if (end == begin) {
        return std::nullopt;
    }
    return val;
}

std::optional<bool> extract_json_bool(std::string_view json, std::string_view key) {
    auto pos_opt = find_json_value_start(json, key);
    if (!pos_opt.has_value()) {
        return std::nullopt;
    }
    const std::string_view tail = json.substr(*pos_opt);
    if (tail.starts_with("true")) {
        return true;
    }
    if (tail.starts_with("false")) {
        return false;
    }
    return std::nullopt;
}

bool wait_overlapped(HANDLE pipe, OVERLAPPED& ov, HANDLE stop_event, DWORD& bytes_transferred) noexcept {
    HANDLE wait_handles[2] = {stop_event, ov.hEvent};
    const DWORD wait_res = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
    if (wait_res == WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        return false;
    }
    if (wait_res != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &ov);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &bytes_transferred, FALSE) != FALSE;
}

bool write_all_overlapped(HANDLE pipe, std::string_view data, HANDLE stop_event) noexcept {
    ScopedHandle write_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!write_event.valid()) {
        return false;
    }

    size_t total_written = 0;
    while (total_written < data.size()) {
        ResetEvent(write_event.get());
        OVERLAPPED ov{};
        ov.hEvent = write_event.get();

        const DWORD remaining = static_cast<DWORD>(data.size() - total_written);
        DWORD written = 0;
        const BOOL ok = WriteFile(
            pipe,
            data.data() + total_written,
            remaining,
            &written,
            &ov);

        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) {
                return false;
            }
            if (!wait_overlapped(pipe, ov, stop_event, written)) {
                return false;
            }
        }
        if (written == 0) {
            return false;
        }
        total_written += static_cast<size_t>(written);
    }

    return true;
}

std::string make_ok_response(uint64_t id, std::string_view result_json) {
    return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"result\":" + std::string(result_json) + "}\n";
}

std::string make_err_response(uint64_t id, std::string_view error_code) {
    return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"" +
           audio::json_escape(error_code) + "\"}\n";
}

} // namespace

ControlServer::ControlServer() noexcept = default;

ControlServer::~ControlServer() {
    stop();
}

bool ControlServer::start(const wchar_t* pipe_name, IControlHandler* handler) {
    stop();

    if (pipe_name == nullptr || pipe_name[0] == L'\0' || handler == nullptr) {
        return false;
    }

    // Fail closed: verify current-user DACL can be constructed before starting.
    PSECURITY_DESCRIPTOR test_sd = nullptr;
    if (!create_current_user_sd(test_sd)) {
        return false;
    }
    LocalFree(test_sd);

    HANDLE raw_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (raw_stop == nullptr) {
        return false;
    }

    pipe_name_ = pipe_name;
    handler_ = handler;
    stop_event_ = raw_stop;
    running_.store(true, std::memory_order_release);

    worker_ = std::thread(&ControlServer::server_loop, this);
    return true;
}

bool ControlServer::start(const wchar_t* pipe_name,
                          StatusProvider status_provider,
                          ShutdownCallback on_shutdown) {
    auto default_handler = std::make_unique<DefaultControlHandler>(
        std::move(status_provider), std::move(on_shutdown));
    if (!start(pipe_name, default_handler.get())) {
        return false;
    }
    owned_default_handler_ = std::move(default_handler);
    return true;
}

struct ControlServer::ClientSession {
    std::mutex pipe_mutex;
    HANDLE pipe{INVALID_HANDLE_VALUE};
    std::atomic<bool> done{false};
    std::thread thread;
};

void ControlServer::reap_finished_clients_locked() {
    auto it = clients_.begin();
    while (it != clients_.end()) {
        if ((*it)->done.load(std::memory_order_acquire)) {
            if ((*it)->thread.joinable()) {
                (*it)->thread.join();
            }
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

void ControlServer::stop() {
    running_.store(false, std::memory_order_release);
    if (stop_event_ != nullptr) {
        SetEvent(static_cast<HANDLE>(stop_event_));
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    std::vector<std::unique_ptr<ClientSession>> active_clients;
    {
        std::lock_guard lock(clients_mutex_);
        active_clients = std::move(clients_);
    }
    for (auto& client : active_clients) {
        {
            std::lock_guard pipe_lock(client->pipe_mutex);
            if (client->pipe != INVALID_HANDLE_VALUE && client->pipe != nullptr) {
                CancelIoEx(client->pipe, nullptr);
            }
        }
        if (client->thread.joinable()) {
            client->thread.join();
        }
    }

    if (stop_event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }
    owned_default_handler_.reset();
    handler_ = nullptr;
}

std::string ControlServer::handle_command_line(const std::string& line, bool& should_shutdown) {
    should_shutdown = false;
    const uint64_t id = extract_json_uint(line, "id").value_or(0);
    const auto cmd = extract_json_string(line, "cmd");

    if (!cmd.has_value()) {
        return make_err_response(id, "invalid_json");
    }

    if (*cmd == "ping") {
        return make_ok_response(id, "{\"pong\":true}");
    }

    if (*cmd == "get_status") {
        const StatusSnapshot status = handler_ ? handler_->get_status() : StatusSnapshot{};
        char buf[256]{};
        std::snprintf(
            buf,
            sizeof(buf),
            "{\"engine_state\":%u,\"sample_rate\":%u,\"buffer_frames\":%u,\"channel_count\":%u,\"version\":\"0.1.4\"}",
            status.engine_state,
            status.sample_rate,
            status.buffer_frames,
            status.channel_count);
        return make_ok_response(id, buf);
    }

    if (*cmd == "shutdown") {
        should_shutdown = true;
        return make_ok_response(id, "{\"shutting_down\":true}");
    }

    if (*cmd == "list_devices") {
        const auto devices = handler_->list_devices();
        std::string json = "{\"devices\":[";
        for (size_t i = 0; i < devices.size(); ++i) {
            if (i > 0) json.push_back(',');
            const auto& d = devices[i];
            json += "{\"id\":\"" + audio::json_escape(d.id) +
                    "\",\"name\":\"" + audio::json_escape(d.name) +
                    "\",\"is_capture\":" + (d.is_capture ? "true" : "false") +
                    ",\"is_default\":" + (d.is_default ? "true" : "false") +
                    ",\"is_virtual\":" + (d.is_virtual ? "true" : "false") +
                    ",\"sample_rate\":" + std::to_string(d.sample_rate) +
                    ",\"channels\":" + std::to_string(d.channels) + "}";
        }
        json += "]}";
        return make_ok_response(id, json);
    }

    if (*cmd == "list_sessions") {
        const auto sessions = handler_->list_sessions();
        std::string json = "{\"sessions\":[";
        for (size_t i = 0; i < sessions.size(); ++i) {
            if (i > 0) json.push_back(',');
            const auto& s = sessions[i];
            char num_buf[160]{};
            std::snprintf(num_buf, sizeof(num_buf),
                          ",\"volume\":%.4f,\"muted\":%s,\"peak\":%.4f,\"is_capture\":%s,\"is_active\":%s}",
                          s.volume,
                          s.muted ? "true" : "false",
                          s.peak,
                          s.is_capture ? "true" : "false",
                          s.is_active ? "true" : "false");
            json += "{\"pid\":" + std::to_string(s.pid) +
                    ",\"process_name\":\"" + audio::json_escape(s.process_name) +
                    "\",\"display_name\":\"" + audio::json_escape(s.display_name) +
                    "\",\"device_id\":\"" + audio::json_escape(s.device_id) + "\"" +
                    num_buf;
        }
        json += "]}";
        return make_ok_response(id, json);
    }

    if (*cmd == "set_channel") {
        ChannelConfig cfg{};
        cfg.channel_id = static_cast<uint32_t>(extract_json_uint(line, "channel_id").value_or(0));
        cfg.volume = static_cast<float>(extract_json_number(line, "volume").value_or(1.0));
        cfg.muted = extract_json_bool(line, "muted").value_or(false);
        cfg.solo = extract_json_bool(line, "solo").value_or(false);
        const bool ok = handler_->set_channel(cfg);
        return ok ? make_ok_response(id, "{\"applied\":true}") : make_err_response(id, "invalid_channel");
    }

    if (*cmd == "set_mic_dsp") {
        MicDspConfig cfg{};
        if (auto v = extract_json_bool(line, "bypass_all")) cfg.bypass_all = *v;
        if (auto v = extract_json_bool(line, "direct_monitor")) cfg.direct_monitor = *v;
        if (auto v = extract_json_bool(line, "monitor_processed")) cfg.monitor_processed = *v;
        if (auto v = extract_json_number(line, "monitor_gain")) cfg.monitor_gain = static_cast<float>(*v);
        if (auto v = extract_json_bool(line, "denoise_enabled")) cfg.denoise_enabled = *v;
        if (auto v = extract_json_number(line, "denoise_strength")) cfg.denoise_strength = static_cast<float>(*v);
        if (auto v = extract_json_bool(line, "gate_enabled")) cfg.gate_enabled = *v;
        if (auto v = extract_json_number(line, "gate_threshold_db")) cfg.gate_threshold_db = static_cast<float>(*v);
        if (auto v = extract_json_bool(line, "eq_enabled")) cfg.eq_enabled = *v;
        if (auto v = extract_json_number(line, "eq_hpf_hz")) cfg.eq_hpf_hz = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_low_db")) cfg.eq_low_db = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_mid1_hz")) cfg.eq_mid1_hz = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_mid1_db")) cfg.eq_mid1_db = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_mid2_hz")) cfg.eq_mid2_hz = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_mid2_db")) cfg.eq_mid2_db = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_high_db")) cfg.eq_high_db = static_cast<float>(*v);
        if (auto v = extract_json_bool(line, "comp_enabled")) cfg.comp_enabled = *v;
        if (auto v = extract_json_number(line, "comp_threshold_db")) cfg.comp_threshold_db = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "comp_ratio")) cfg.comp_ratio = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "comp_makeup_db")) cfg.comp_makeup_db = static_cast<float>(*v);
        if (auto v = extract_json_bool(line, "ducking_enabled")) cfg.ducking_enabled = *v;
        if (auto v = extract_json_number(line, "ducking_depth_db")) cfg.ducking_depth_db = static_cast<float>(*v);
        if (auto v = extract_json_bool(line, "reverb_enabled")) cfg.reverb_enabled = *v;
        if (auto v = extract_json_number(line, "reverb_wet")) cfg.reverb_wet = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "reverb_room")) cfg.reverb_room = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "reverb_damp")) cfg.reverb_damp = static_cast<float>(*v);
        if (auto v = extract_json_uint(line, "reverb_mode")) cfg.reverb_mode = static_cast<uint32_t>(*v);
        if (auto v = extract_json_bool(line, "pitch_enabled")) cfg.pitch_enabled = *v;
        if (auto v = extract_json_number(line, "pitch_semitones")) cfg.pitch_semitones = static_cast<float>(*v);
        if (auto v = extract_json_bool(line, "limiter_enabled")) cfg.limiter_enabled = *v;
        if (auto v = extract_json_number(line, "limiter_ceiling_db")) cfg.limiter_ceiling_db = static_cast<float>(*v);

        const bool ok = handler_->set_mic_dsp(cfg);
        return ok ? make_ok_response(id, "{\"applied\":true}") : make_err_response(id, "dsp_update_failed");
    }

    if (*cmd == "select_devices") {
        const std::string input_id = extract_json_string(line, "input_id").value_or("");
        const std::string monitor_id = extract_json_string(line, "monitor_id").value_or("");
        const std::string output_id = extract_json_string(line, "output_id").value_or("");
        const bool ok = handler_->select_devices(input_id, monitor_id, output_id);
        return ok ? make_ok_response(id, "{\"selected\":true}") : make_err_response(id, "device_select_failed");
    }

    if (*cmd == "route_app") {
        const uint32_t pid = static_cast<uint32_t>(extract_json_uint(line, "pid").value_or(0));
        const std::string target_id = extract_json_string(line, "target_device_id").value_or("");
        const bool loopback = extract_json_bool(line, "loopback_capture").value_or(false);
        const auto res = handler_->route_app(pid, target_id, loopback);
        const std::string json = "{\"routed\":" + std::string(res.ok ? "true" : "false") +
                                 ",\"method\":\"" + audio::json_escape(res.method) +
                                 "\",\"message\":\"" + audio::json_escape(res.message) + "\"}";
        return make_ok_response(id, json);
    }

    if (*cmd == "save_preset") {
        const auto name = extract_json_string(line, "name");
        if (!name.has_value() || !handler_->save_preset(*name)) {
            return make_err_response(id, "save_preset_failed");
        }
        return make_ok_response(id, "{\"saved\":true}");
    }

    if (*cmd == "load_preset") {
        const auto name = extract_json_string(line, "name");
        if (!name.has_value()) {
            return make_err_response(id, "invalid_preset_name");
        }
        const auto loaded = handler_->load_preset(*name);
        if (!loaded.has_value()) {
            return make_err_response(id, "preset_not_found");
        }
        return make_ok_response(id, "{\"loaded\":true,\"name\":\"" + audio::json_escape(*name) + "\"}");
    }

    if (*cmd == "list_presets") {
        const auto presets = handler_->list_presets();
        std::string json = "{\"presets\":[";
        for (size_t i = 0; i < presets.size(); ++i) {
            if (i > 0) json.push_back(',');
            json += "\"" + audio::json_escape(presets[i]) + "\"";
        }
        json += "]}";
        return make_ok_response(id, json);
    }

    if (*cmd == "get_driver_status") {
        const auto ds = handler_->get_driver_status();
        const std::string json =
            "{\"driver_installed\":" + std::string(ds.driver_installed ? "true" : "false") +
            ",\"driver_version\":" + std::to_string(ds.driver_version) +
            ",\"endpoint_count\":" + std::to_string(ds.endpoint_count) +
            ",\"fallback_mode\":\"" + audio::json_escape(ds.fallback_mode) + "\"}";
        return make_ok_response(id, json);
    }

    if (*cmd == "list_routing_rules") {
        const auto rules = handler_->list_routing_rules();
        std::string json = "{\"rules\":[";
        for (size_t i = 0; i < rules.size(); ++i) {
            if (i > 0) json.push_back(',');
            const auto& r = rules[i];
            json += "{\"process_name\":\"" + audio::json_escape(r.process_name) +
                    "\",\"channel_id\":" + std::to_string(r.channel_id) +
                    ",\"target_device_id\":\"" + audio::json_escape(r.target_device_id) +
                    "\",\"loopback_capture\":" + (r.loopback_capture ? "true" : "false") + "}";
        }
        json += "]}";
        return make_ok_response(id, json);
    }

    if (*cmd == "set_routing_rule") {
        const auto proc = extract_json_string(line, "process_name");
        if (!proc.has_value() || proc->empty()) {
            return make_err_response(id, "invalid_process_name");
        }
        audio::RoutingRule rule{};
        rule.process_name = *proc;
        rule.channel_id = static_cast<uint32_t>(extract_json_uint(line, "channel_id").value_or(1));
        rule.target_device_id = extract_json_string(line, "target_device_id").value_or("");
        rule.loopback_capture = extract_json_bool(line, "loopback_capture").value_or(false);

        const auto res = handler_->set_routing_rule(rule);
        if (!res.saved) {
            return make_err_response(id, "save_routing_rule_failed");
        }
        const std::string json =
            "{\"saved\":true,\"applied\":" + std::string(res.applied ? "true" : "false") +
            ",\"method\":\"" + audio::json_escape(res.method) + "\"}";
        return make_ok_response(id, json);
    }

    if (*cmd == "delete_routing_rule") {
        const auto proc = extract_json_string(line, "process_name");
        if (!proc.has_value() || proc->empty()) {
            return make_err_response(id, "invalid_process_name");
        }
        const bool deleted = handler_->delete_routing_rule(*proc);
        return make_ok_response(id, std::string("{\"deleted\":") + (deleted ? "true}" : "false}"));
    }

    if (*cmd == "apply_routing_rules") {
        const uint32_t count = handler_->apply_routing_rules();
        return make_ok_response(id, "{\"applied_count\":" + std::to_string(count) + "}");
    }

    if (*cmd == "list_channels") {
        const auto channels = handler_->list_channels();
        std::string json = "{\"channels\":[";
        for (size_t i = 0; i < channels.size(); ++i) {
            if (i > 0) json.push_back(',');
            const auto& c = channels[i];
            char num_buf[256]{};
            std::snprintf(
                num_buf,
                sizeof(num_buf),
                ",\"is_system\":%s,\"volume\":%.4f,\"muted\":%s,\"solo\":%s,\"pan\":%.4f,"
                "\"send_a1\":%s,\"send_a2\":%s,\"send_b1\":%s,\"send_b2\":%s,\"echo_locked_a1_a2\":%s",
                c.is_system ? "true" : "false",
                c.volume,
                c.muted ? "true" : "false",
                c.solo ? "true" : "false",
                c.pan,
                c.send_a1 ? "true" : "false",
                c.send_a2 ? "true" : "false",
                c.send_b1 ? "true" : "false",
                c.send_b2 ? "true" : "false",
                c.echo_locked_a1_a2 ? "true" : "false");

            json += "{\"channel_id\":" + std::to_string(c.channel_id) +
                    ",\"name\":\"" + audio::json_escape(c.name) +
                    "\",\"source_type\":\"" + audio::json_escape(c.source_type) +
                    "\",\"device_id\":\"" + audio::json_escape(c.device_id) +
                    "\",\"color\":\"" + audio::json_escape(c.color) + "\"" +
                    num_buf + ",\"bound_apps\":[";
            for (size_t a = 0; a < c.bound_apps.size(); ++a) {
                if (a > 0) json.push_back(',');
                json += "\"" + audio::json_escape(c.bound_apps[a]) + "\"";
            }
            json += "]}";
        }
        json += "]}";
        return make_ok_response(id, json);
    }

    if (*cmd == "create_channel") {
        const std::string name = extract_json_string(line, "name").value_or("");
        const std::string source_type = extract_json_string(line, "source_type").value_or("app_group");
        const std::string color = extract_json_string(line, "color").value_or("#6366F1");
        const auto res = handler_->create_channel(name, source_type, color);
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "create_channel_failed" : res.error);
        }
        return make_ok_response(id, "{\"created\":true,\"channel_id\":" + std::to_string(res.channel_id) + "}");
    }

    if (*cmd == "delete_channel") {
        const auto ch_id = extract_json_uint(line, "channel_id");
        if (!ch_id.has_value()) {
            return make_err_response(id, "missing_channel_id");
        }
        const auto res = handler_->delete_channel(static_cast<uint32_t>(*ch_id));
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "delete_channel_failed" : res.error);
        }
        return make_ok_response(id, "{\"deleted\":true}");
    }

    if (*cmd == "rename_channel") {
        const auto ch_id = extract_json_uint(line, "channel_id");
        const auto name = extract_json_string(line, "name");
        if (!ch_id.has_value() || !name.has_value()) {
            return make_err_response(id, "invalid_params");
        }
        const auto res = handler_->rename_channel(static_cast<uint32_t>(*ch_id), *name);
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "rename_channel_failed" : res.error);
        }
        return make_ok_response(id, "{\"renamed\":true}");
    }

    if (*cmd == "bind_channel_source") {
        const auto ch_id = extract_json_uint(line, "channel_id");
        if (!ch_id.has_value()) {
            return make_err_response(id, "missing_channel_id");
        }
        const std::string source_type = extract_json_string(line, "source_type").value_or("app_group");
        const std::string device_id = extract_json_string(line, "device_id").value_or("");
        const auto res = handler_->bind_channel_source(static_cast<uint32_t>(*ch_id), source_type, device_id);
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "bind_channel_source_failed" : res.error);
        }
        return make_ok_response(id, "{\"bound\":true}");
    }

    if (*cmd == "list_output_buses") {
        const auto buses = handler_->list_output_buses();
        std::string json = "{\"buses\":[";
        for (size_t i = 0; i < buses.size(); ++i) {
            if (i > 0) json.push_back(',');
            const auto& b = buses[i];
            char buf[128]{};
            std::snprintf(
                buf,
                sizeof(buf),
                ",\"is_virtual\":%s,\"volume\":%.4f,\"muted\":%s,\"active\":%s}",
                b.is_virtual ? "true" : "false",
                b.volume,
                b.muted ? "true" : "false",
                b.active ? "true" : "false");
            json += "{\"bus_id\":\"" + audio::json_escape(b.bus_id) +
                    "\",\"role\":\"" + audio::json_escape(b.role) +
                    "\",\"name\":\"" + audio::json_escape(b.name) +
                    "\",\"device_id\":\"" + audio::json_escape(b.device_id) +
                    "\",\"device_name\":\"" + audio::json_escape(b.device_name) + "\"" + buf;
        }
        json += "]}";
        return make_ok_response(id, json);
    }

    if (*cmd == "bind_output_bus") {
        const auto bus_id = extract_json_string(line, "bus_id");
        if (!bus_id.has_value()) {
            return make_err_response(id, "missing_bus_id");
        }
        const std::string device_id = extract_json_string(line, "device_id").value_or("");
        const float vol = static_cast<float>(extract_json_number(line, "volume").value_or(1.0));
        const bool muted = extract_json_bool(line, "muted").value_or(false);
        const auto res = handler_->bind_output_bus(*bus_id, device_id, vol, muted);
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "bind_output_bus_failed" : res.error);
        }
        return make_ok_response(id, "{\"bound\":true}");
    }

    if (*cmd == "set_output_bus_mix") {
        const auto bus_id = extract_json_string(line, "bus_id");
        if (!bus_id.has_value() || bus_id->empty()) {
            return make_err_response(id, "missing_bus_id");
        }
        const float vol = static_cast<float>(extract_json_number(line, "volume").value_or(1.0));
        const bool muted = extract_json_bool(line, "muted").value_or(false);
        const auto res = handler_->set_output_bus_mix(*bus_id, vol, muted);
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "invalid_bus_id" : res.error);
        }
        char buf[128]{};
        std::snprintf(buf, sizeof(buf), ",\"volume\":%.4f,\"muted\":%s}", res.volume, res.muted ? "true" : "false");
        return make_ok_response(id, "{\"bus_id\":\"" + audio::json_escape(res.bus_id) + "\"" + buf);
    }

    if (*cmd == "set_channel_send") {
        const auto ch_id = extract_json_uint(line, "channel_id");
        const auto bus_id = extract_json_string(line, "bus_id");
        if (!ch_id.has_value() || !bus_id.has_value()) {
            return make_err_response(id, "invalid_params");
        }
        const bool enabled = extract_json_bool(line, "enabled").value_or(false);
        const float send_gain = static_cast<float>(extract_json_number(line, "send_gain").value_or(1.0));
        const auto res = handler_->set_channel_send(static_cast<uint32_t>(*ch_id), *bus_id, enabled, send_gain);
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "set_channel_send_failed" : res.error);
        }
        return make_ok_response(id, "{\"updated\":true}");
    }

    if (*cmd == "set_channel_dsp") {
        ChannelDspConfig cfg{};
        cfg.channel_id = static_cast<uint32_t>(extract_json_uint(line, "channel_id").value_or(0));
        if (auto v = extract_json_number(line, "volume")) cfg.volume = static_cast<float>(*v);
        if (auto v = extract_json_bool(line, "muted")) cfg.muted = *v;
        if (auto v = extract_json_bool(line, "solo")) cfg.solo = *v;
        if (auto v = extract_json_number(line, "pan")) cfg.pan = static_cast<float>(*v);
        if (auto v = extract_json_bool(line, "eq_enabled")) cfg.eq_enabled = *v;
        if (auto v = extract_json_number(line, "eq_hpf_hz")) cfg.eq_hpf_hz = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_low_db")) cfg.eq_low_db = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_mid1_hz")) cfg.eq_mid1_hz = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_mid1_db")) cfg.eq_mid1_db = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_mid2_hz")) cfg.eq_mid2_hz = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_mid2_db")) cfg.eq_mid2_db = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "eq_high_db")) cfg.eq_high_db = static_cast<float>(*v);
        if (auto v = extract_json_bool(line, "comp_enabled")) cfg.comp_enabled = *v;
        if (auto v = extract_json_number(line, "comp_threshold_db")) cfg.comp_threshold_db = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "comp_ratio")) cfg.comp_ratio = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "comp_makeup_db")) cfg.comp_makeup_db = static_cast<float>(*v);
        if (auto v = extract_json_number(line, "duck_source_id")) cfg.duck_source_id = static_cast<int32_t>(*v);
        if (auto v = extract_json_number(line, "duck_depth_db")) cfg.duck_depth_db = static_cast<float>(*v);
        const auto res = handler_->set_channel_dsp(cfg);
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "set_channel_dsp_failed" : res.error);
        }
        return make_ok_response(id, "{\"updated\":true}");
    }

    if (*cmd == "bind_app_to_channel") {
        const auto proc = extract_json_string(line, "process_name");
        const auto ch_id = extract_json_uint(line, "channel_id");
        if (!proc.has_value() || !ch_id.has_value()) {
            return make_err_response(id, "invalid_params");
        }
        const auto res = handler_->bind_app_to_channel(*proc, static_cast<uint32_t>(*ch_id));
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "bind_app_failed" : res.error);
        }
        return make_ok_response(id, "{\"bound\":true}");
    }

    if (*cmd == "unbind_app_from_channel") {
        const auto proc = extract_json_string(line, "process_name");
        const auto ch_id = extract_json_uint(line, "channel_id");
        if (!proc.has_value() || !ch_id.has_value()) {
            return make_err_response(id, "invalid_params");
        }
        const auto res = handler_->unbind_app_from_channel(*proc, static_cast<uint32_t>(*ch_id));
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "unbind_app_failed" : res.error);
        }
        return make_ok_response(id, "{\"unbound\":true}");
    }

    if (*cmd == "auto_setup_defaults") {
        const auto res = handler_->auto_setup_defaults();
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "auto_setup_failed" : res.error);
        }
        return make_ok_response(id, "{\"configured\":true}");
    }

    if (*cmd == "set_system_default_virtual") {
        const bool enabled = extract_json_bool(line, "enabled").value_or(true);
        const auto res = handler_->set_system_default_virtual(enabled);
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "set_system_default_virtual_failed" : res.error);
        }
        return make_ok_response(id, std::string("{\"enabled\":") + (enabled ? "true}" : "false}"));
    }

    if (*cmd == "set_chatmix") {
        const float balance = static_cast<float>(extract_json_number(line, "balance").value_or(0.0));
        const auto res = handler_->set_chatmix(balance);
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "set_chatmix_failed" : res.error);
        }
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "{\"balance\":%.4f}", balance);
        return make_ok_response(id, buf);
    }

    if (*cmd == "play_soundboard") {
        const auto path = extract_json_string(line, "path");
        if (!path.has_value() || path->empty()) {
            return make_err_response(id, "invalid_wav");
        }
        const float gain = static_cast<float>(extract_json_number(line, "gain").value_or(1.0));
        const bool send_a1 = extract_json_bool(line, "send_a1").value_or(true);
        const bool send_b1 = extract_json_bool(line, "send_b1").value_or(true);
        const bool send_b2 = extract_json_bool(line, "send_b2").value_or(true);
        const bool loop = extract_json_bool(line, "loop").value_or(false);
        const auto res = handler_->play_soundboard(*path, gain, send_a1, send_b1, send_b2, loop);
        if (!res.playing) {
            return make_err_response(id, "invalid_wav");
        }
        return make_ok_response(
            id,
            "{\"playing\":true,\"slot\":" + std::to_string(res.slot) +
                ",\"duration_ms\":" + std::to_string(res.duration_ms) + "}");
    }

    if (*cmd == "stop_soundboard") {
        const int slot = static_cast<int>(extract_json_number(line, "slot").value_or(-1.0));
        const bool stopped = handler_->stop_soundboard(slot);
        return make_ok_response(id, std::string("{\"stopped\":") + (stopped ? "true}" : "false}"));
    }

    if (*cmd == "get_soundboard_status") {
        const auto st = handler_->get_soundboard_status();
        return make_ok_response(
            id,
            "{\"active_voices\":" + std::to_string(st.active_voices) +
                ",\"max_voices\":" + std::to_string(st.max_voices) + "}");
    }

    if (*cmd == "set_spectrum_source") {
        const auto ch_id = extract_json_uint(line, "channel_id");
        if (!ch_id.has_value()) {
            return make_err_response(id, "missing_channel_id");
        }
        const auto res = handler_->set_spectrum_source(static_cast<uint32_t>(*ch_id));
        if (!res.ok) {
            return make_err_response(id, res.error.empty() ? "invalid_channel_id" : res.error);
        }
        return make_ok_response(id, "{\"channel_id\":" + std::to_string(res.channel_id) + "}");
    }

    return make_err_response(id, "unknown_command");
}

void ControlServer::handle_client_pipe(void* raw_session) {
    auto* session = static_cast<ClientSession*>(raw_session);
    HANDLE raw_pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard pipe_lock(session->pipe_mutex);
        raw_pipe = session->pipe;
    }
    HANDLE stop_evt = static_cast<HANDLE>(stop_event_);
    audio::ScopedComInit com(COINIT_MULTITHREADED);

    ScopedHandle io_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    std::string pending;
    char read_buf[1024]{};
    bool client_active = io_event.valid() && raw_pipe != INVALID_HANDLE_VALUE && raw_pipe != nullptr;
    bool shutdown_requested = false;

    while (client_active && running_.load(std::memory_order_acquire)) {
        ResetEvent(io_event.get());
        OVERLAPPED read_ov{};
        read_ov.hEvent = io_event.get();

        DWORD bytes_read = 0;
        const BOOL read_ok = ReadFile(
            raw_pipe,
            read_buf,
            static_cast<DWORD>(sizeof(read_buf)),
            &bytes_read,
            &read_ov);

        if (!read_ok) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                if (!wait_overlapped(raw_pipe, read_ov, stop_evt, bytes_read)) {
                    client_active = false;
                    break;
                }
            } else {
                client_active = false;
                break;
            }
        }

        if (bytes_read == 0) {
            client_active = false;
            break;
        }

        constexpr size_t kMaxPendingBytes = 64 * 1024;
        if (pending.size() + bytes_read > kMaxPendingBytes) {
            client_active = false;
            break;
        }
        pending.append(read_buf, bytes_read);

        size_t nl_pos = std::string::npos;
        while ((nl_pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl_pos);
            pending.erase(0, nl_pos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }

            bool cmd_shutdown = false;
            const std::string reply = handle_command_line(line, cmd_shutdown);
            if (!write_all_overlapped(raw_pipe, reply, stop_evt)) {
                client_active = false;
                break;
            }
            if (cmd_shutdown) {
                shutdown_requested = true;
                client_active = false;
                break;
            }
        }
    }

    if (raw_pipe != INVALID_HANDLE_VALUE && raw_pipe != nullptr) {
        if (running_.load(std::memory_order_acquire)) {
            FlushFileBuffers(raw_pipe);
        }
        DisconnectNamedPipe(raw_pipe);
        std::lock_guard pipe_lock(session->pipe_mutex);
        CloseHandle(raw_pipe);
        session->pipe = INVALID_HANDLE_VALUE;
    }
    session->done.store(true, std::memory_order_release);

    if (shutdown_requested) {
        running_.store(false, std::memory_order_release);
        if (stop_evt != nullptr) {
            SetEvent(stop_evt);
        }
        if (handler_) {
            handler_->on_shutdown();
        }
    }
}

void ControlServer::server_loop() {
    HANDLE stop_evt = static_cast<HANDLE>(stop_event_);

    PSECURITY_DESCRIPTOR raw_sd = nullptr;
    if (!create_current_user_sd(raw_sd)) {
        running_.store(false, std::memory_order_release);
        return;
    }
    ScopedLocalAlloc sd_guard(raw_sd);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = sd_guard.get();
    sa.bInheritHandle = FALSE;

    ScopedHandle io_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!io_event.valid()) {
        running_.store(false, std::memory_order_release);
        return;
    }

    auto create_listening_pipe = [&]() -> HANDLE {
        return CreateNamedPipeW(
            pipe_name_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            65536,
            65536,
            0,
            &sa);
    };

    HANDLE next_pipe = create_listening_pipe();
    if (next_pipe == INVALID_HANDLE_VALUE) {
        running_.store(false, std::memory_order_release);
        return;
    }

    while (running_.load(std::memory_order_acquire)) {
        if (next_pipe == INVALID_HANDLE_VALUE) {
            next_pipe = create_listening_pipe();
            if (next_pipe == INVALID_HANDLE_VALUE) {
                break;
            }
        }

        ResetEvent(io_event.get());
        OVERLAPPED connect_ov{};
        connect_ov.hEvent = io_event.get();

        const BOOL connected_now = ConnectNamedPipe(next_pipe, &connect_ov);
        if (!connected_now) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD dummy = 0;
                if (!wait_overlapped(next_pipe, connect_ov, stop_evt, dummy)) {
                    CloseHandle(next_pipe);
                    next_pipe = INVALID_HANDLE_VALUE;
                    break;
                }
            } else if (err != ERROR_PIPE_CONNECTED) {
                CloseHandle(next_pipe);
                next_pipe = INVALID_HANDLE_VALUE;
                continue;
            }
        }

        // Immediately arm the next standby pipe instance before dispatching the client thread
        HANDLE standby_pipe = running_.load(std::memory_order_acquire)
            ? create_listening_pipe()
            : INVALID_HANDLE_VALUE;

        auto session = std::make_unique<ClientSession>();
        session->pipe = next_pipe;
        next_pipe = standby_pipe;

        ClientSession* session_ptr = session.get();
        session->thread = std::thread(&ControlServer::handle_client_pipe, this, session_ptr);

        std::lock_guard lock(clients_mutex_);
        reap_finished_clients_locked();
        clients_.push_back(std::move(session));
    }

    if (next_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(next_pipe);
    }

    running_.store(false, std::memory_order_release);
}

} // namespace atriswave
