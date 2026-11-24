#include <atriswave/telemetry_publisher.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <atomic>
#include <cstring>

namespace atriswave {

TelemetryPublisher::~TelemetryPublisher() noexcept {
    close();
}

bool TelemetryPublisher::open(const wchar_t* shm_name) noexcept {
    close();
    if (shm_name == nullptr || shm_name[0] == L'\0') {
        return false;
    }

    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(TelemetrySnapshot)),
        shm_name);

    if (mapping == nullptr) {
        return false;
    }

    const bool already_exists = (GetLastError() == ERROR_ALREADY_EXISTS);

    void* view = MapViewOfFile(
        mapping,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(TelemetrySnapshot));

    if (view == nullptr) {
        CloseHandle(mapping);
        return false;
    }

    mapping_handle_ = mapping;
    mapped_ = static_cast<TelemetrySnapshot*>(view);

    if (!already_exists) {
        std::memset(mapped_, 0, sizeof(TelemetrySnapshot));
        mapped_->header.magic = ATRISWAVE_TELEMETRY_MAGIC;
        mapped_->header.version = ATRISWAVE_TELEMETRY_VERSION;
        std::atomic_ref<uint64_t> seq_ref(mapped_->header.sequence);
        seq_ref.store(0, std::memory_order_release);
    }

    return true;
}

void TelemetryPublisher::close() noexcept {
    if (mapped_ != nullptr) {
        UnmapViewOfFile(mapped_);
        mapped_ = nullptr;
    }
    if (mapping_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_handle_));
        mapping_handle_ = nullptr;
    }
}

void TelemetryPublisher::publish(const TelemetrySnapshot& snapshot) noexcept {
    if (mapped_ == nullptr) {
        return;
    }

    std::atomic_ref<uint64_t> seq_ref(mapped_->header.sequence);
    const uint64_t current = seq_ref.load(std::memory_order_relaxed);
    const uint64_t even_seq = (current & 1ULL) != 0ULL ? (current + 1ULL) : current;

    // Enter write section (odd sequence)
    seq_ref.store(even_seq + 1ULL, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acq_rel);

    mapped_->header.magic = ATRISWAVE_TELEMETRY_MAGIC;
    mapped_->header.version = ATRISWAVE_TELEMETRY_VERSION;
    mapped_->header.channel_count = snapshot.header.channel_count;
    mapped_->header.sample_rate = snapshot.header.sample_rate;
    mapped_->header.buffer_frames = snapshot.header.buffer_frames;
    mapped_->header.engine_state = snapshot.header.engine_state;
    mapped_->header.timestamp_qpc = snapshot.header.timestamp_qpc;
    mapped_->header.cpu_load_percent = snapshot.header.cpu_load_percent;
    mapped_->header.xrun_count = snapshot.header.xrun_count;
    mapped_->header.bus_count = snapshot.header.bus_count;
    std::memcpy(mapped_->header.reserved, snapshot.header.reserved, sizeof(mapped_->header.reserved));

    std::memcpy(mapped_->channels, snapshot.channels, sizeof(mapped_->channels));
    std::memcpy(mapped_->buses, snapshot.buses, sizeof(mapped_->buses));

    // Exit write section (even sequence)
    std::atomic_thread_fence(std::memory_order_release);
    seq_ref.store(even_seq + 2ULL, std::memory_order_release);
}

uint64_t TelemetryPublisher::query_qpc() noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart);
}

} // namespace atriswave
