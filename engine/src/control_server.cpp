#include <atriswave/control_server.hpp>

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
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atriswave {
namespace {

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

    void** out_ptr() noexcept {
        reset();
        return &ptr_;
    }

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

std::optional<std::string> extract_json_string(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }

    size_t pos = json.find(':', key_pos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;

    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
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
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t key_pos = json.find(needle);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }

    size_t pos = json.find(':', key_pos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;

    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
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

} // namespace

ControlServer::~ControlServer() {
    stop();
}

bool ControlServer::start(const wchar_t* pipe_name,
                          StatusProvider status_provider,
                          ShutdownCallback on_shutdown) {
    stop();

    if (pipe_name == nullptr || pipe_name[0] == L'\0') {
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
    status_provider_ = std::move(status_provider);
    on_shutdown_ = std::move(on_shutdown);
    stop_event_ = raw_stop;
    running_.store(true, std::memory_order_release);

    worker_ = std::thread(&ControlServer::server_loop, this);
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
}

std::string ControlServer::handle_command_line(const std::string& line, bool& should_shutdown) {
    should_shutdown = false;
    const uint64_t id = extract_json_uint(line, "id").value_or(0);
    const auto cmd = extract_json_string(line, "cmd");

    if (!cmd.has_value()) {
        return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"invalid_json\"}\n";
    }

    if (*cmd == "ping") {
        return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"result\":{\"pong\":true}}\n";
    }

    if (*cmd == "get_status") {
        StatusSnapshot status{};
        if (status_provider_) {
            status = status_provider_();
        }
        char buf[256]{};
        std::snprintf(
            buf,
            sizeof(buf),
            "{\"id\":%llu,\"ok\":true,\"result\":{\"engine_state\":%u,\"sample_rate\":%u,\"buffer_frames\":%u,\"channel_count\":%u,\"version\":\"0.1.0\"}}\n",
            static_cast<unsigned long long>(id),
            status.engine_state,
            status.sample_rate,
            status.buffer_frames,
            status.channel_count);
        return std::string(buf);
    }

    if (*cmd == "shutdown") {
        should_shutdown = true;
        return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"result\":{\"shutting_down\":true}}\n";
    }

    return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":\"unknown_command\"}\n";
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
            if (on_shutdown_) {
                on_shutdown_();
            }
            break;
        }
    }

    running_.store(false, std::memory_order_release);
}

} // namespace atriswave
