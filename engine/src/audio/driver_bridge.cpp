#include <atriswave/audio/driver_bridge.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>

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

DriverBridgeClient::~DriverBridgeClient() {
    close();
}

bool DriverBridgeClient::open() noexcept {
    close();

    HANDLE dev = CreateFileW(
        ATRISWAVE_VAD_WIN32_DEVICE_NAME_W,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (dev == INVALID_HANDLE_VALUE || dev == nullptr) {
        return false;
    }

    AtrisWaveVadVersionInfo info{};
    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(
        dev,
        IOCTL_ATRISWAVE_GET_VERSION,
        nullptr,
        0,
        &info,
        static_cast<DWORD>(sizeof(info)),
        &bytes_returned,
        nullptr);

    if (!ok || bytes_returned != sizeof(info) || info.abi_version != ATRISWAVE_DRIVER_ABI_VERSION) {
        CloseHandle(dev);
        return false;
    }

    handle_ = dev;
    open_.store(true, std::memory_order_release);
    return true;
}

void DriverBridgeClient::close() noexcept {
    open_.store(false, std::memory_order_release);
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

bool DriverBridgeClient::set_endpoint_connected(uint32_t endpoint_id, bool connected) noexcept {
    if (!is_open() || handle_ == nullptr || endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
        return false;
    }
    AtrisWaveVadSetConnectedRequest req{};
    req.endpoint_id = endpoint_id;
    req.connected = connected ? 1u : 0u;
    DWORD bytes = 0;
    return DeviceIoControl(
               static_cast<HANDLE>(handle_),
               IOCTL_ATRISWAVE_SET_ENDPOINT_CONNECTED,
               &req,
               static_cast<DWORD>(sizeof(req)),
               nullptr,
               0,
               &bytes,
               nullptr) != FALSE;
}

bool DriverBridgeClient::get_endpoint_states(AtrisWaveVadEndpointStateMask& out_mask) noexcept {
    if (!is_open() || handle_ == nullptr) {
        return false;
    }
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(
        static_cast<HANDLE>(handle_),
        IOCTL_ATRISWAVE_GET_ENDPOINT_STATES,
        nullptr,
        0,
        &out_mask,
        static_cast<DWORD>(sizeof(out_mask)),
        &bytes,
        nullptr);
    return ok != FALSE && bytes == sizeof(out_mask);
}

bool DriverBridgeClient::write_capture_pcm(uint32_t endpoint_id,
                                           const float* interleaved_stereo,
                                           uint32_t frames) noexcept {
    if (!is_open() || handle_ == nullptr || interleaved_stereo == nullptr || frames == 0u ||
        endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
        return false;
    }

    constexpr uint32_t kMaxChunk = 1024u;
    const uint32_t clamped = std::min(frames, kMaxChunk);
    const size_t pcm_bytes = static_cast<size_t>(clamped) * 2u * sizeof(float);

    alignas(8) std::array<uint8_t, sizeof(AtrisWaveVadPcmWriteHeader) + kMaxChunk * 2u * sizeof(float)> buf{};
    auto* hdr = reinterpret_cast<AtrisWaveVadPcmWriteHeader*>(buf.data());
    hdr->endpoint_id = endpoint_id;
    hdr->frames = clamped;
    hdr->channels = 2u;
    hdr->reserved = 0u;
    std::memcpy(buf.data() + sizeof(AtrisWaveVadPcmWriteHeader), interleaved_stereo, pcm_bytes);

    DWORD bytes = 0;
    return DeviceIoControl(
               static_cast<HANDLE>(handle_),
               IOCTL_ATRISWAVE_WRITE_CAPTURE_PCM,
               buf.data(),
               static_cast<DWORD>(sizeof(AtrisWaveVadPcmWriteHeader) + pcm_bytes),
               nullptr,
               0,
               &bytes,
               nullptr) != FALSE;
}

bool DriverBridgeClient::read_render_pcm(uint32_t endpoint_id,
                                         float* interleaved_stereo,
                                         uint32_t max_frames,
                                         uint32_t& out_frames) noexcept {
    out_frames = 0u;
    if (!is_open() || handle_ == nullptr || interleaved_stereo == nullptr || max_frames == 0u ||
        endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
        return false;
    }

    constexpr uint32_t kMaxChunk = 1024u;
    const uint32_t clamped = std::min(max_frames, kMaxChunk);
    const size_t out_buf_bytes = sizeof(AtrisWaveVadPcmReadHeader) + static_cast<size_t>(clamped) * 2u * sizeof(float);

    AtrisWaveVadPcmReadRequest req{};
    req.endpoint_id = endpoint_id;
    req.max_frames = clamped;

    alignas(8) std::array<uint8_t, sizeof(AtrisWaveVadPcmReadHeader) + kMaxChunk * 2u * sizeof(float)> buf{};
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(
        static_cast<HANDLE>(handle_),
        IOCTL_ATRISWAVE_READ_RENDER_PCM,
        &req,
        static_cast<DWORD>(sizeof(req)),
        buf.data(),
        static_cast<DWORD>(out_buf_bytes),
        &bytes,
        nullptr);

    if (!ok || bytes < sizeof(AtrisWaveVadPcmReadHeader)) {
        return false;
    }

    const auto* hdr = reinterpret_cast<const AtrisWaveVadPcmReadHeader*>(buf.data());
    const uint32_t got = std::min(hdr->frames_read, clamped);
    const size_t pcm_bytes = static_cast<size_t>(got) * 2u * sizeof(float);
    if (got > 0u && bytes >= sizeof(AtrisWaveVadPcmReadHeader) + pcm_bytes) {
        std::memcpy(interleaved_stereo, buf.data() + sizeof(AtrisWaveVadPcmReadHeader), pcm_bytes);
        out_frames = got;
    }
    return true;
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
