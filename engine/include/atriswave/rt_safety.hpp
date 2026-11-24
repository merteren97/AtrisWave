#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <avrt.h>
#include <pmmintrin.h>
#include <xmmintrin.h>

#include <cstdint>
#include <utility>

namespace atriswave {

/// RAII guard that registers the current thread with Windows MMCSS ("Pro Audio")
/// and reverts registration on scope exit.
class MmcssGuard final {
public:
    explicit MmcssGuard(const wchar_t* task_name = L"Pro Audio") noexcept {
        if (task_name != nullptr) {
            handle_ = AvSetMmThreadCharacteristicsW(task_name, &task_index_);
        }
    }

    ~MmcssGuard() noexcept {
        reset();
    }

    MmcssGuard(const MmcssGuard&) = delete;
    MmcssGuard& operator=(const MmcssGuard&) = delete;

    MmcssGuard(MmcssGuard&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
        , task_index_(std::exchange(other.task_index_, 0)) {}

    MmcssGuard& operator=(MmcssGuard&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
            task_index_ = std::exchange(other.task_index_, 0);
        }
        return *this;
    }

    [[nodiscard]] bool is_active() const noexcept {
        return handle_ != nullptr;
    }

    [[nodiscard]] DWORD task_index() const noexcept {
        return task_index_;
    }

    bool set_priority(AVRT_PRIORITY priority) noexcept {
        if (handle_ == nullptr) {
            return false;
        }
        return AvSetMmThreadPriority(handle_, priority) != FALSE;
    }

private:
    void reset() noexcept {
        if (handle_ != nullptr) {
            AvRevertMmThreadCharacteristics(handle_);
            handle_ = nullptr;
            task_index_ = 0;
        }
    }

    HANDLE handle_{nullptr};
    DWORD task_index_{0};
};

/// Enables SSE/SSE2 Flush-To-Zero (FTZ) and Denormals-Are-Zero (DAZ)
/// on the calling thread to prevent denormal CPU spikes in DSP loops.
inline void enable_ftz_daz() noexcept {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}

/// RAII guard that enables FTZ/DAZ on construction and restores MXCSR on exit.
class DenormalGuard final {
public:
    DenormalGuard() noexcept : prev_csr_(_mm_getcsr()) {
        enable_ftz_daz();
    }

    ~DenormalGuard() noexcept {
        _mm_setcsr(prev_csr_);
    }

    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;
    DenormalGuard(DenormalGuard&&) = delete;
    DenormalGuard& operator=(DenormalGuard&&) = delete;

private:
    unsigned int prev_csr_{0};
};

} // namespace atriswave
