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
        return audio::WasapiDeviceEnumerator::enumerate_devices();
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

std::optional<size_t> find_json_value_start(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t search_from = 0;
    while (true) {
        const size_t key_pos = json.find(needle, search_from);
        if (key_pos == std::string_view::npos) {
            return std::nullopt;
        }
        size_t pos = key_pos + needle.size();
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
            ++pos;
        }
        if (pos < json.size() && json[pos] == ':') {
            ++pos;
            while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
                ++pos;
            }
            return pos;
        }
        search_from = key_pos + needle.size();
    }
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
        }
        value.push_back(json[pos]);
        ++pos;
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

    OVERLAPPED ov{};
    ov.hEvent = write_event.get();

    DWORD written = 0;
    const BOOL ok = WriteFile(
        pipe,
        data.data(),
        static_cast<DWORD>(data.size()),
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

    return written == data.size();
}

std::string make_ok_response(uint64_t id, std::string_view result_json) {
    return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"result\":" + std::string(result_json) + "}\n";
}

std::string make_err_response(uint64_t id, std::string_view error_code) {
    return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"" +
           audio::json_escape(error_code) + "\"}\n";
}

} // namespace

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

void ControlServer::stop() {
    running_.store(false, std::memory_order_release);
    if (stop_event_ != nullptr) {
        SetEvent(static_cast<HANDLE>(stop_event_));
    }
    if (worker_.joinable()) {
        worker_.join();
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
            "{\"engine_state\":%u,\"sample_rate\":%u,\"buffer_frames\":%u,\"channel_count\":%u,\"version\":\"0.2.0\"}",
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
            char num_buf[96]{};
            std::snprintf(num_buf, sizeof(num_buf), ",\"volume\":%.4f,\"muted\":%s,\"peak\":%.4f}",
                          s.volume, s.muted ? "true" : "false", s.peak);
            json += "{\"pid\":" + std::to_string(s.pid) +
                    ",\"process_name\":\"" + audio::json_escape(s.process_name) +
                    "\",\"display_name\":\"" + audio::json_escape(s.display_name) + "\"" +
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
        if (auto v = extract_json_bool(line, "direct_monitor")) cfg.direct_monitor = *v;
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

    return make_err_response(id, "unknown_command");
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

    while (running_.load(std::memory_order_acquire)) {
        ScopedHandle pipe(CreateNamedPipeW(
            pipe_name_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            4096,
            4096,
            0,
            &sa));

        if (!pipe.valid()) {
            break;
        }

        ScopedHandle io_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!io_event.valid()) {
            break;
        }

        OVERLAPPED connect_ov{};
        connect_ov.hEvent = io_event.get();

        const BOOL connected_now = ConnectNamedPipe(pipe.get(), &connect_ov);
        if (!connected_now) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD dummy = 0;
                if (!wait_overlapped(pipe.get(), connect_ov, stop_evt, dummy)) {
                    break;
                }
            } else if (err != ERROR_PIPE_CONNECTED) {
                continue;
            }
        }

        std::string pending;
        char read_buf[1024]{};
        bool client_active = true;
        bool shutdown_requested = false;

        while (client_active && running_.load(std::memory_order_acquire)) {
            ResetEvent(io_event.get());
            OVERLAPPED read_ov{};
            read_ov.hEvent = io_event.get();

            DWORD bytes_read = 0;
            const BOOL read_ok = ReadFile(
                pipe.get(),
                read_buf,
                static_cast<DWORD>(sizeof(read_buf)),
                &bytes_read,
                &read_ov);

            if (!read_ok) {
                const DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    if (!wait_overlapped(pipe.get(), read_ov, stop_evt, bytes_read)) {
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
                if (!write_all_overlapped(pipe.get(), reply, stop_evt)) {
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

        FlushFileBuffers(pipe.get());
        DisconnectNamedPipe(pipe.get());

        if (shutdown_requested) {
            running_.store(false, std::memory_order_release);
            if (handler_) {
                handler_->on_shutdown();
            }
            break;
        }
    }

    running_.store(false, std::memory_order_release);
}

} // namespace atriswave
