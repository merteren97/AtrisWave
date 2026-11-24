#pragma once

#include <atriswave/ipc_protocol.h>

#include <cstdint>

namespace atriswave {

/// Lock-free / wait-free Win32 shared memory telemetry writer using a seqlock.
/// After `open()`, `publish()` performs zero heap allocations and zero locks,
/// making it safe to call from real-time audio threads.
class TelemetryPublisher final {
public:
    TelemetryPublisher() noexcept = default;
    ~TelemetryPublisher() noexcept;

    TelemetryPublisher(const TelemetryPublisher&) = delete;
    TelemetryPublisher& operator=(const TelemetryPublisher&) = delete;
    TelemetryPublisher(TelemetryPublisher&&) = delete;
    TelemetryPublisher& operator=(TelemetryPublisher&&) = delete;

    /// Creates or opens the named shared memory region and maps TelemetrySnapshot.
    [[nodiscard]] bool open(const wchar_t* shm_name = ATRISWAVE_TELEMETRY_SHM_NAME_W) noexcept;

    /// Unmaps the view and closes the file mapping handle.
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return mapped_ != nullptr; }

    /// Publishes a telemetry snapshot using the seqlock protocol.
    /// Real-time safe (no locks, no allocations, no syscalls).
    void publish(const TelemetrySnapshot& snapshot) noexcept;

    /// Returns the current QueryPerformanceCounter timestamp.
    [[nodiscard]] static uint64_t query_qpc() noexcept;

private:
    void* mapping_handle_{nullptr};
    TelemetrySnapshot* mapped_{nullptr};
};

} // namespace atriswave
