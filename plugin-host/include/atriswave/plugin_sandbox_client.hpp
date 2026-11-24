#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <sddl.h>

#include "atriswave/plugin_ipc.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace atriswave {

class PluginSandboxClient final {
public:
    PluginSandboxClient() noexcept = default;

    ~PluginSandboxClient() noexcept {
        stop();
    }

    PluginSandboxClient(const PluginSandboxClient&) = delete;
    PluginSandboxClient& operator=(const PluginSandboxClient&) = delete;
    PluginSandboxClient(PluginSandboxClient&&) = delete;
    PluginSandboxClient& operator=(PluginSandboxClient&&) = delete;

    bool start(const std::wstring& host_exe_path,
               const std::wstring& plugin_dll_path,
               uint32_t sample_rate = 48000u,
               uint32_t timeout_ms = 5u,
               uint32_t startup_timeout_ms = 5000u) {
        stop();

        if (host_exe_path.empty() || plugin_dll_path.empty() || sample_rate == 0u) {
            return false;
        }

        sample_rate_ = sample_rate;
        timeout_ms_ = std::max(1u, timeout_ms);
        crossfade_total_samples_ = std::max<uint32_t>(1u, (sample_rate_ * 5u) / 1000u);
        crossfade_pos_ = 0u;
        crossfade_active_ = false;
        is_bypassed_ = false;
        last_wet_left_ = 0.0f;
        last_wet_right_ = 0.0f;

        PSECURITY_DESCRIPTOR raw_sd = nullptr;
        if (!create_current_user_sd(raw_sd)) {
            return false;
        }
        ScopedLocalAlloc sd_guard(raw_sd);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.lpSecurityDescriptor = raw_sd;
        sa.bInheritHandle = FALSE;

        static std::atomic<uint32_t> slot_counter{1u};
        const uint32_t id = slot_counter.fetch_add(1u, std::memory_order_relaxed);
        slot_name_ = L"AtrisWavePluginSlot_" +
                     std::to_wstring(GetCurrentProcessId()) + L"_" +
                     std::to_wstring(id);

        const std::wstring shm_name = L"Local\\" + slot_name_;
        const std::wstring req_name = shm_name + L"_req";
        const std::wstring done_name = shm_name + L"_done";

        shm_handle_.reset(CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            &sa,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(sizeof(AtrisWavePluginSlotLayout)),
            shm_name.c_str()));
        if (!shm_handle_.valid()) {
            stop();
            return false;
        }

        slot_ = static_cast<AtrisWavePluginSlotLayout*>(
            MapViewOfFile(shm_handle_.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AtrisWavePluginSlotLayout)));
        if (slot_ == nullptr) {
            stop();
            return false;
        }

        std::memset(slot_, 0, sizeof(AtrisWavePluginSlotLayout));
        slot_->magic = ATRISWAVE_PLUGIN_IPC_MAGIC;
        slot_->abi_version = ATRISWAVE_PLUGIN_IPC_ABI_VERSION;
        slot_->max_frames = ATRISWAVE_PLUGIN_MAX_FRAMES;
        slot_->sample_rate = sample_rate_;
        slot_->command = ATRISWAVE_PLUGIN_CMD_NONE;
        slot_->status = ATRISWAVE_PLUGIN_STATUS_IDLE;

        req_event_.reset(CreateEventW(&sa, FALSE, FALSE, req_name.c_str()));
        done_event_.reset(CreateEventW(&sa, FALSE, FALSE, done_name.c_str()));
        if (!req_event_.valid() || !done_event_.valid()) {
            stop();
            return false;
        }

        std::wstring cmd_line = L"\"" + host_exe_path + L"\"" +
                                L" --slot \"" + slot_name_ + L"\"" +
                                L" --plugin \"" + plugin_dll_path + L"\"" +
                                L" --sample-rate " + std::to_wstring(sample_rate_);

        STARTUPINFOW si{};
        si.cb = sizeof(STARTUPINFOW);
        PROCESS_INFORMATION pi{};

        if (!CreateProcessW(
                host_exe_path.c_str(),
                cmd_line.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &si,
                &pi)) {
            stop();
            return false;
        }

        process_handle_.reset(pi.hProcess);
        thread_handle_.reset(pi.hThread);

        HANDLE wait_handles[2] = {done_event_.get(), process_handle_.get()};
        const DWORD wait_rc = WaitForMultipleObjects(2, wait_handles, FALSE, startup_timeout_ms);
        if (wait_rc != WAIT_OBJECT_0 || slot_->status != ATRISWAVE_PLUGIN_STATUS_READY) {
            stop();
            return false;
        }

        return true;
    }

    void stop() noexcept {
        if (process_handle_.valid() && slot_ != nullptr && req_event_.valid() && done_event_.valid()) {
            DWORD exit_code = 0;
            if (GetExitCodeProcess(process_handle_.get(), &exit_code) && exit_code == STILL_ACTIVE) {
                slot_->command = ATRISWAVE_PLUGIN_CMD_SHUTDOWN;
                slot_->status = ATRISWAVE_PLUGIN_STATUS_IDLE;
                SetEvent(req_event_.get());

                HANDLE wait_handles[2] = {done_event_.get(), process_handle_.get()};
                WaitForMultipleObjects(2, wait_handles, FALSE, 250);
                if (WaitForSingleObject(process_handle_.get(), 250) == WAIT_TIMEOUT) {
                    TerminateProcess(process_handle_.get(), 1);
                    WaitForSingleObject(process_handle_.get(), 250);
                }
            }
        }

        thread_handle_.reset();
        process_handle_.reset();
        done_event_.reset();
        req_event_.reset();

        if (slot_ != nullptr) {
            UnmapViewOfFile(slot_);
            slot_ = nullptr;
        }
        shm_handle_.reset();
        slot_name_.clear();
    }

    /// Updates a plugin parameter. Callers must serialize access to the slot
    /// with process() (e.g., audio thread owns slot_->command).
    bool set_param(uint32_t param_index, float value) noexcept {
        if (is_bypassed_ || slot_ == nullptr || !req_event_.valid() || !done_event_.valid() ||
            !process_handle_.valid()) {
            return false;
        }

        slot_->param_index = param_index;
        slot_->param_value = value;
        slot_->command = ATRISWAVE_PLUGIN_CMD_SET_PARAM;
        slot_->status = ATRISWAVE_PLUGIN_STATUS_IDLE;
        SetEvent(req_event_.get());

        HANDLE wait_handles[2] = {done_event_.get(), process_handle_.get()};
        const DWORD wait_rc = WaitForMultipleObjects(2, wait_handles, FALSE, std::max(timeout_ms_, 50u));
        if (wait_rc == WAIT_OBJECT_0 && slot_->status == ATRISWAVE_PLUGIN_STATUS_READY) {
            return true;
        }

        engage_bypass();
        return false;
    }

    bool simulate_crash() noexcept {
        if (slot_ == nullptr || !req_event_.valid() || !process_handle_.valid()) {
            return false;
        }

        slot_->command = ATRISWAVE_PLUGIN_CMD_SIMULATE_CRASH;
        slot_->status = ATRISWAVE_PLUGIN_STATUS_IDLE;
        SetEvent(req_event_.get());

        const DWORD wait_rc = WaitForSingleObject(process_handle_.get(), 2000);
        if (wait_rc == WAIT_OBJECT_0) {
            engage_bypass();
            return true;
        }
        return false;
    }

    /// Processes a stereo audio block through the sandboxed plugin, or crossfades
    /// smoothly to dry bypass if crashed/timed out. Callers must serialize
    /// access to the slot with set_param() (e.g., audio thread owns slot_->command).
    void process(const float* in_left,
                 const float* in_right,
                 float* out_left,
                 float* out_right,
                 uint32_t num_frames) noexcept {
        if (in_left == nullptr || in_right == nullptr || out_left == nullptr || out_right == nullptr ||
            num_frames == 0u) {
            return;
        }

        uint32_t offset = 0u;
        while (offset < num_frames) {
            const uint32_t chunk = std::min(num_frames - offset, ATRISWAVE_PLUGIN_MAX_FRAMES);
            const float* chunk_in_l = in_left + offset;
            const float* chunk_in_r = in_right + offset;
            float* chunk_out_l = out_left + offset;
            float* chunk_out_r = out_right + offset;

            if (!is_bypassed_ && process_remote_chunk(chunk_in_l, chunk_in_r, chunk_out_l, chunk_out_r, chunk)) {
                last_wet_left_ = chunk_out_l[chunk - 1u];
                last_wet_right_ = chunk_out_r[chunk - 1u];
            } else {
                process_bypass_chunk(chunk_in_l, chunk_in_r, chunk_out_l, chunk_out_r, chunk);
            }

            offset += chunk;
        }
    }

    [[nodiscard]] bool is_bypassed() const noexcept {
        return is_bypassed_;
    }

    [[nodiscard]] uint32_t crash_count() const noexcept {
        return crash_count_;
    }

    [[nodiscard]] uint32_t sample_rate() const noexcept {
        return sample_rate_;
    }

    [[nodiscard]] uint32_t crossfade_samples() const noexcept {
        return crossfade_total_samples_;
    }

private:
    class ScopedHandle final {
    public:
        ScopedHandle() noexcept = default;
        explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
        ~ScopedHandle() noexcept {
            reset();
        }
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;
        [[nodiscard]] HANDLE get() const noexcept {
            return handle_;
        }
        [[nodiscard]] bool valid() const noexcept {
            return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
        }
        void reset(HANDLE next = nullptr) noexcept {
            if (valid()) {
                CloseHandle(handle_);
            }
            handle_ = next;
        }

    private:
        HANDLE handle_{nullptr};
    };

    class ScopedLocalAlloc final {
    public:
        explicit ScopedLocalAlloc(void* ptr) noexcept : ptr_(ptr) {}
        ~ScopedLocalAlloc() noexcept {
            if (ptr_ != nullptr) {
                LocalFree(ptr_);
            }
        }
        ScopedLocalAlloc(const ScopedLocalAlloc&) = delete;
        ScopedLocalAlloc& operator=(const ScopedLocalAlloc&) = delete;

    private:
        void* ptr_{nullptr};
    };

    static bool create_current_user_sd(PSECURITY_DESCRIPTOR& sd_out) noexcept {
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

    bool process_remote_chunk(const float* in_left,
                              const float* in_right,
                              float* out_left,
                              float* out_right,
                              uint32_t chunk) noexcept {
        if (slot_ == nullptr || !req_event_.valid() || !done_event_.valid() || !process_handle_.valid()) {
            engage_bypass();
            return false;
        }

        std::memcpy(slot_->in_left, in_left, chunk * sizeof(float));
        std::memcpy(slot_->in_right, in_right, chunk * sizeof(float));
        slot_->num_frames = chunk;
        slot_->command = ATRISWAVE_PLUGIN_CMD_PROCESS;
        slot_->status = ATRISWAVE_PLUGIN_STATUS_IDLE;
        SetEvent(req_event_.get());

        HANDLE wait_handles[2] = {done_event_.get(), process_handle_.get()};
        const DWORD wait_rc = WaitForMultipleObjects(2, wait_handles, FALSE, timeout_ms_);
        if (wait_rc == WAIT_OBJECT_0 && slot_->status == ATRISWAVE_PLUGIN_STATUS_READY) {
            for (uint32_t i = 0u; i < chunk; ++i) {
                const float l = slot_->out_left[i];
                const float r = slot_->out_right[i];
                out_left[i] = std::isfinite(l) ? l : 0.0f;
                out_right[i] = std::isfinite(r) ? r : 0.0f;
            }
            return true;
        }

        engage_bypass();
        return false;
    }

    void engage_bypass() noexcept {
        if (!is_bypassed_) {
            is_bypassed_ = true;
            ++crash_count_;
            crossfade_pos_ = 0u;
            crossfade_active_ = true;
        }
    }

    void process_bypass_chunk(const float* in_left,
                              const float* in_right,
                              float* out_left,
                              float* out_right,
                              uint32_t chunk) noexcept {
        if (!crossfade_active_) {
            std::memcpy(out_left, in_left, chunk * sizeof(float));
            std::memcpy(out_right, in_right, chunk * sizeof(float));
            return;
        }

        for (uint32_t i = 0u; i < chunk; ++i) {
            if (crossfade_pos_ < crossfade_total_samples_) {
                const float alpha =
                    static_cast<float>(crossfade_pos_) / static_cast<float>(crossfade_total_samples_);
                const float one_minus_alpha = 1.0f - alpha;
                out_left[i] = last_wet_left_ * one_minus_alpha + in_left[i] * alpha;
                out_right[i] = last_wet_right_ * one_minus_alpha + in_right[i] * alpha;
                ++crossfade_pos_;
            } else {
                out_left[i] = in_left[i];
                out_right[i] = in_right[i];
            }
        }

        if (crossfade_pos_ >= crossfade_total_samples_) {
            crossfade_active_ = false;
        }
    }

    std::wstring slot_name_;
    ScopedHandle shm_handle_;
    ScopedHandle req_event_;
    ScopedHandle done_event_;
    ScopedHandle process_handle_;
    ScopedHandle thread_handle_;
    AtrisWavePluginSlotLayout* slot_{nullptr};

    uint32_t sample_rate_{48000u};
    uint32_t timeout_ms_{5u};
    uint32_t crossfade_total_samples_{240u};
    uint32_t crossfade_pos_{0u};
    bool crossfade_active_{false};
    bool is_bypassed_{false};
    uint32_t crash_count_{0u};
    float last_wet_left_{0.0f};
    float last_wet_right_{0.0f};
};

} // namespace atriswave
