#include <atriswave/audio/driver_bridge.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace atriswave::audio {

bool VadRingSession::attach(AtrisWaveVadRingHeader* header, float* pcm_buffer) noexcept {
    detach();
    if (header == nullptr || pcm_buffer == nullptr) {
        return false;
    }
    if (!atriswave_vad_validate_header(header)) {
        return false;
    }
    header_ = header;
    pcm_buffer_ = pcm_buffer;
    return true;
}

void VadRingSession::detach() noexcept {
    header_ = nullptr;
    pcm_buffer_ = nullptr;
}

uint32_t VadRingSession::available_read() const noexcept {
    if (!is_attached()) {
        return 0u;
    }
    return atriswave_vad_available_read(header_, nullptr, nullptr);
}

uint32_t VadRingSession::available_write() const noexcept {
    if (!is_attached()) {
        return 0u;
    }
    return atriswave_vad_available_write(header_, nullptr, nullptr);
}

uint32_t VadRingSession::write_frames(const float* src, uint32_t frames) noexcept {
    if (!is_attached()) {
        return 0u;
    }
    return atriswave_vad_write_frames(header_, pcm_buffer_, src, frames);
}

uint32_t VadRingSession::read_frames(float* dst, uint32_t frames) noexcept {
    if (!is_attached()) {
        return 0u;
    }
    return atriswave_vad_read_frames(header_, pcm_buffer_, dst, frames);
}

DriverStatus DriverBridge::probe_driver() noexcept {
    DriverStatus status{};

    HANDLE device = CreateFileW(
        ATRISWAVE_VAD_WIN32_DEVICE_NAME_W,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (device == INVALID_HANDLE_VALUE || device == nullptr) {
        return status;
    }

    AtrisWaveVadVersionInfo info{};
    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(
        device,
        IOCTL_ATRISWAVE_GET_VERSION,
        nullptr,
        0,
        &info,
        static_cast<DWORD>(sizeof(info)),
        &bytes_returned,
        nullptr);

    CloseHandle(device);

    if (ok && bytes_returned == sizeof(info) && info.abi_version == ATRISWAVE_DRIVER_ABI_VERSION) {
        status.driver_installed = true;
        status.abi_version = info.abi_version;
        status.driver_version = (info.major << 16u) | (info.minor << 8u) | info.patch;
        status.endpoint_count = info.endpoint_count;
        status.fallback_mode = "atriswave_vad";
    }

    return status;
}

} // namespace atriswave::audio
