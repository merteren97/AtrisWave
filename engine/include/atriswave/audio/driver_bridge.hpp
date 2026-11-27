#pragma once

#include <atriswave/driver_ioctl.h>
#include <atriswave/ring_buffer_validate.h>

#include <atomic>
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

/// Persistent Win32 IOCTL client to `\\.\AtrisWaveVAD` for real-time stereo PCM I/O
/// and dynamic virtual jack presence management.
class DriverBridgeClient final {
public:
    DriverBridgeClient() noexcept = default;
    ~DriverBridgeClient();

    DriverBridgeClient(const DriverBridgeClient&) = delete;
    DriverBridgeClient& operator=(const DriverBridgeClient&) = delete;

    [[nodiscard]] bool open() noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept {
        return open_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool set_endpoint_connected(uint32_t endpoint_id, bool connected) noexcept;
    [[nodiscard]] bool get_endpoint_states(AtrisWaveVadEndpointStateMask& out_mask) noexcept;

    /// Zero-allocation RT-safe IOCTL write into virtual capture endpoint ring (B1 Mic / B2 Stream Mix).
    [[nodiscard]] bool write_capture_pcm(uint32_t endpoint_id,
                                         const float* interleaved_stereo,
                                         uint32_t frames) noexcept;

    /// Zero-allocation RT-safe IOCTL read from virtual render endpoint ring (System / Game / Chat / ...).
    [[nodiscard]] bool read_render_pcm(uint32_t endpoint_id,
                                       float* interleaved_stereo,
                                       uint32_t max_frames,
                                       uint32_t& out_frames) noexcept;

private:
    void* handle_{nullptr};
    std::atomic<bool> open_{false};
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
