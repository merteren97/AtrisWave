#pragma once

#include "atriswave/driver_ioctl.h"

#if defined(__cplusplus) && !defined(_KERNEL_MODE)
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#else
#include <stddef.h>
#ifndef _KERNEL_MODE
#include <stdint.h>
#endif
#include <string.h>
#endif

#ifdef __cplusplus
#define ATRISWAVE_INLINE_CONSTEXPR inline constexpr
#define ATRISWAVE_INLINE inline
#else
#define ATRISWAVE_INLINE_CONSTEXPR static __inline
#define ATRISWAVE_INLINE static __inline
#endif

ATRISWAVE_INLINE_CONSTEXPR bool atriswave_vad_is_power_of_two(uint32_t v)
{
    return (v != 0u) && ((v & (v - 1u)) == 0u);
}

ATRISWAVE_INLINE void atriswave_vad_memory_barrier(void)
{
#ifdef _KERNEL_MODE
    KeMemoryBarrier();
#elif defined(__cplusplus)
    std::atomic_thread_fence(std::memory_order_acq_rel);
#else
    MemoryBarrier();
#endif
}

ATRISWAVE_INLINE uint32_t atriswave_vad_load_u32_once(const volatile uint32_t* ptr)
{
    return *ptr;
}

ATRISWAVE_INLINE uint64_t atriswave_vad_load_u64_once(const volatile uint64_t* ptr)
{
    return *ptr;
}

ATRISWAVE_INLINE void atriswave_vad_store_u64_once(volatile uint64_t* ptr, uint64_t value)
{
    *ptr = value;
}

ATRISWAVE_INLINE bool atriswave_vad_snapshot_header_fields(
    const AtrisWaveVadRingHeader* hdr,
    uint32_t* out_channels,
    uint32_t* out_capacity_frames)
{
    if (hdr == nullptr) {
        return false;
    }

    /* Snapshot untrusted header fields ONCE to local stack variables (anti-TOCTOU). */
    const uint32_t magic = atriswave_vad_load_u32_once(&hdr->magic);
    const uint32_t abi_version = atriswave_vad_load_u32_once(&hdr->abi_version);
    const uint32_t endpoint_id = atriswave_vad_load_u32_once(&hdr->endpoint_id);
    const uint32_t sample_rate = atriswave_vad_load_u32_once(&hdr->sample_rate);
    const uint32_t channels = atriswave_vad_load_u32_once(&hdr->channels);
    const uint32_t capacity_frames = atriswave_vad_load_u32_once(&hdr->capacity_frames);

    if (magic != ATRISWAVE_VAD_RING_MAGIC) {
        return false;
    }
    if (abi_version != ATRISWAVE_DRIVER_ABI_VERSION) {
        return false;
    }
    if (endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
        return false;
    }
    if (channels < ATRISWAVE_VAD_MIN_CHANNELS || channels > ATRISWAVE_VAD_MAX_CHANNELS) {
        return false;
    }
    if (sample_rate < ATRISWAVE_VAD_MIN_SAMPLE_RATE || sample_rate > ATRISWAVE_VAD_MAX_SAMPLE_RATE) {
        return false;
    }
    if (capacity_frames < ATRISWAVE_VAD_MIN_RING_FRAMES ||
        capacity_frames > ATRISWAVE_VAD_MAX_RING_FRAMES ||
        !atriswave_vad_is_power_of_two(capacity_frames)) {
        return false;
    }

    if (out_channels != nullptr) {
        *out_channels = channels;
    }
    if (out_capacity_frames != nullptr) {
        *out_capacity_frames = capacity_frames;
    }
    return true;
}

ATRISWAVE_INLINE bool atriswave_vad_validate_header(const AtrisWaveVadRingHeader* hdr)
{
    return atriswave_vad_snapshot_header_fields(hdr, nullptr, nullptr);
}

ATRISWAVE_INLINE uint32_t atriswave_vad_available_read_snapshot(
    const AtrisWaveVadRingHeader* hdr,
    uint64_t* out_write_idx,
    uint64_t* out_read_idx,
    uint32_t* out_channels,
    uint32_t* out_capacity_frames)
{
    uint32_t channels = 0u;
    uint32_t capacity_frames = 0u;
    if (!atriswave_vad_snapshot_header_fields(hdr, &channels, &capacity_frames)) {
        return 0u;
    }

    /* Snapshot untrusted shared indices ONCE to local stack variables (anti-TOCTOU). */
    const uint64_t write_idx = atriswave_vad_load_u64_once(&hdr->write_frame_idx);
    atriswave_vad_memory_barrier();
    const uint64_t read_idx = atriswave_vad_load_u64_once(&hdr->read_frame_idx);

    if (write_idx < read_idx) {
        return 0u;
    }

    const uint64_t used = write_idx - read_idx;
    if (used > static_cast<uint64_t>(capacity_frames)) {
        return 0u;
    }

    if (out_write_idx != nullptr) {
        *out_write_idx = write_idx;
    }
    if (out_read_idx != nullptr) {
        *out_read_idx = read_idx;
    }
    if (out_channels != nullptr) {
        *out_channels = channels;
    }
    if (out_capacity_frames != nullptr) {
        *out_capacity_frames = capacity_frames;
    }

    return static_cast<uint32_t>(used);
}

ATRISWAVE_INLINE uint32_t atriswave_vad_available_write_snapshot(
    const AtrisWaveVadRingHeader* hdr,
    uint64_t* out_write_idx,
    uint64_t* out_read_idx,
    uint32_t* out_channels,
    uint32_t* out_capacity_frames)
{
    uint32_t channels = 0u;
    uint32_t capacity_frames = 0u;
    if (!atriswave_vad_snapshot_header_fields(hdr, &channels, &capacity_frames)) {
        return 0u;
    }

    /* Snapshot untrusted shared indices ONCE to local stack variables (anti-TOCTOU). */
    const uint64_t read_idx = atriswave_vad_load_u64_once(&hdr->read_frame_idx);
    atriswave_vad_memory_barrier();
    const uint64_t write_idx = atriswave_vad_load_u64_once(&hdr->write_frame_idx);

    if (write_idx < read_idx) {
        return 0u;
    }

    const uint64_t used = write_idx - read_idx;
    if (used > static_cast<uint64_t>(capacity_frames)) {
        return 0u;
    }

    if (out_write_idx != nullptr) {
        *out_write_idx = write_idx;
    }
    if (out_read_idx != nullptr) {
        *out_read_idx = read_idx;
    }
    if (out_channels != nullptr) {
        *out_channels = channels;
    }
    if (out_capacity_frames != nullptr) {
        *out_capacity_frames = capacity_frames;
    }

    return capacity_frames - static_cast<uint32_t>(used);
}

ATRISWAVE_INLINE uint32_t atriswave_vad_available_read(
    const AtrisWaveVadRingHeader* hdr,
    uint64_t* out_write_idx,
    uint64_t* out_read_idx)
{
    return atriswave_vad_available_read_snapshot(
        hdr,
        out_write_idx,
        out_read_idx,
        nullptr,
        nullptr);
}

ATRISWAVE_INLINE uint32_t atriswave_vad_available_write(
    const AtrisWaveVadRingHeader* hdr,
    uint64_t* out_write_idx,
    uint64_t* out_read_idx)
{
    return atriswave_vad_available_write_snapshot(
        hdr,
        out_write_idx,
        out_read_idx,
        nullptr,
        nullptr);
}

ATRISWAVE_INLINE uint32_t atriswave_vad_write_frames(
    AtrisWaveVadRingHeader* hdr,
    float* pcm_buf,
    const float* src,
    uint32_t frames)
{
    if (hdr == nullptr || pcm_buf == nullptr || src == nullptr || frames == 0u) {
        return 0u;
    }

    uint64_t write_idx = 0u;
    uint64_t read_idx = 0u;
    uint32_t channels_u32 = 0u;
    uint32_t capacity = 0u;
    const uint32_t avail = atriswave_vad_available_write_snapshot(
        hdr,
        &write_idx,
        &read_idx,
        &channels_u32,
        &capacity);
    if (avail == 0u) {
        return 0u;
    }

    uint32_t to_write = (frames < avail) ? frames : avail;
    const uint64_t max_until_u64_wrap = UINT64_MAX - write_idx;
    if (static_cast<uint64_t>(to_write) > max_until_u64_wrap) {
        to_write = static_cast<uint32_t>(max_until_u64_wrap);
    }
    if (to_write == 0u) {
        return 0u;
    }

    /* Use ONLY validated local stack copies (capacity, channels_u32, write_idx). */
    const uint32_t mask = capacity - 1u;
    const uint32_t start_frame = static_cast<uint32_t>(write_idx & static_cast<uint64_t>(mask));
    const uint32_t tail_space = capacity - start_frame;
    const uint32_t first_chunk = (tail_space < to_write) ? tail_space : to_write;
    const uint32_t second_chunk = to_write - first_chunk;
    const size_t channels = static_cast<size_t>(channels_u32);

    memcpy(
        pcm_buf + (static_cast<size_t>(start_frame) * channels),
        src,
        static_cast<size_t>(first_chunk) * channels * sizeof(float));

    if (second_chunk > 0u) {
        memcpy(
            pcm_buf,
            src + (static_cast<size_t>(first_chunk) * channels),
            static_cast<size_t>(second_chunk) * channels * sizeof(float));
    }

    atriswave_vad_memory_barrier();
    atriswave_vad_store_u64_once(&hdr->write_frame_idx, write_idx + static_cast<uint64_t>(to_write));
    return to_write;
}

ATRISWAVE_INLINE uint32_t atriswave_vad_read_frames(
    AtrisWaveVadRingHeader* hdr,
    const float* pcm_buf,
    float* dst,
    uint32_t frames)
{
    if (hdr == nullptr || pcm_buf == nullptr || dst == nullptr || frames == 0u) {
        return 0u;
    }

    uint64_t write_idx = 0u;
    uint64_t read_idx = 0u;
    uint32_t channels_u32 = 0u;
    uint32_t capacity = 0u;
    const uint32_t avail = atriswave_vad_available_read_snapshot(
        hdr,
        &write_idx,
        &read_idx,
        &channels_u32,
        &capacity);
    if (avail == 0u) {
        return 0u;
    }

    const uint32_t to_read = (frames < avail) ? frames : avail;

    /* Use ONLY validated local stack copies (capacity, channels_u32, read_idx). */
    const uint32_t mask = capacity - 1u;
    const uint32_t start_frame = static_cast<uint32_t>(read_idx & static_cast<uint64_t>(mask));
    const uint32_t tail_avail = capacity - start_frame;
    const uint32_t first_chunk = (tail_avail < to_read) ? tail_avail : to_read;
    const uint32_t second_chunk = to_read - first_chunk;
    const size_t channels = static_cast<size_t>(channels_u32);

    memcpy(
        dst,
        pcm_buf + (static_cast<size_t>(start_frame) * channels),
        static_cast<size_t>(first_chunk) * channels * sizeof(float));

    if (second_chunk > 0u) {
        memcpy(
            dst + (static_cast<size_t>(first_chunk) * channels),
            pcm_buf,
            static_cast<size_t>(second_chunk) * channels * sizeof(float));
    }

    atriswave_vad_memory_barrier();
    atriswave_vad_store_u64_once(&hdr->read_frame_idx, read_idx + static_cast<uint64_t>(to_read));
    return to_read;
}
