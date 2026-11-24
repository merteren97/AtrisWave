#pragma once

#include <atriswave/driver_ioctl.h>
#include <atriswave/ring_buffer_validate.h>

#include <cstdint>
#include <string>

namespace atriswave::audio {

struct DriverStatus {
    bool driver_installed{false};
    uint32_t abi_version{0};
    uint32_t driver_version{0};
    uint32_t endpoint_count{ATRISWAVE_VAD_ENDPOINT_COUNT};
    std::string fallback_mode{"wasapi_vbcable"};
};

/// Safe user-mode wrapper over `atriswave/ring_buffer_validate.h` for zero-copy
/// PCM ring-buffer exchange with `AtrisWaveVAD.sys` (or mock user-mode buffers in tests).
class VadRingSession final {
public:
    VadRingSession() noexcept = default;

    [[nodiscard]] bool attach(AtrisWaveVadRingHeader* header, float* pcm_buffer) noexcept;
    void detach() noexcept;

    [[nodiscard]] bool is_attached() const noexcept {
        return header_ != nullptr && pcm_buffer_ != nullptr;
    }

    [[nodiscard]] uint32_t available_read() const noexcept;
    [[nodiscard]] uint32_t available_write() const noexcept;
    [[nodiscard]] uint32_t write_frames(const float* src, uint32_t frames) noexcept;
    [[nodiscard]] uint32_t read_frames(float* dst, uint32_t frames) noexcept;

private:
    AtrisWaveVadRingHeader* header_{nullptr};
    float* pcm_buffer_{nullptr};
};

/// User-mode Win32 bridge to `\\.\AtrisWaveVAD`.
class DriverBridge final {
public:
    /// Probes `\\.\AtrisWaveVAD` via `IOCTL_ATRISWAVE_GET_VERSION`.
    /// Gracefully returns `driver_installed = false` with `fallback_mode = "wasapi_vbcable"`
    /// when the kernel driver is not installed.
    [[nodiscard]] static DriverStatus probe_driver() noexcept;
};

} // namespace atriswave::audio
