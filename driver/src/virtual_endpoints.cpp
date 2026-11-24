#include "driver_internal.h"

static constexpr uint32_t kDefaultRingFrames = 2048u;
static constexpr uint32_t kDefaultSampleRate = 48000u;
static constexpr uint32_t kDefaultChannels = 2u;

static const AtrisWaveEndpointDescriptor kEndpointDescriptors[ATRISWAVE_VAD_ENDPOINT_COUNT] = {
    { 0u, ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Game",       L"Game",       kDefaultSampleRate, kDefaultChannels },
    { 1u, ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Chat",       L"Chat",       kDefaultSampleRate, kDefaultChannels },
    { 2u, ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Media",      L"Media",      kDefaultSampleRate, kDefaultChannels },
    { 3u, ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Aux",        L"Aux",        kDefaultSampleRate, kDefaultChannels },
    { 4u, ATRISWAVE_ENDPOINT_CAPTURE, L"AtrisWave Mic",        L"MicOut",     kDefaultSampleRate, kDefaultChannels },
    { 5u, ATRISWAVE_ENDPOINT_CAPTURE, L"AtrisWave Stream Mix", L"StreamMix",  kDefaultSampleRate, kDefaultChannels }
};

const AtrisWaveEndpointDescriptor* AtrisWaveGetEndpointDescriptors(uint32_t* out_count)
{
    if (out_count != nullptr) {
        *out_count = ATRISWAVE_VAD_ENDPOINT_COUNT;
    }
    return kEndpointDescriptors;
}

NTSTATUS AtrisWaveInitializeEndpoints(PATRISWAVE_DEVICE_CONTEXT ctx)
{
    if (ctx == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < ATRISWAVE_VAD_ENDPOINT_COUNT; ++i) {
        AtrisWaveEndpointRuntime* ep = &ctx->endpoints[i];
        const AtrisWaveEndpointDescriptor* desc = &kEndpointDescriptors[i];

        RtlZeroMemory(ep, sizeof(*ep));
        ep->ring_header.magic = ATRISWAVE_VAD_RING_MAGIC;
        ep->ring_header.abi_version = ATRISWAVE_DRIVER_ABI_VERSION;
        ep->ring_header.endpoint_id = desc->endpoint_id;
        ep->ring_header.sample_rate = desc->default_sample_rate;
        ep->ring_header.channels = desc->default_channels;
        ep->ring_header.capacity_frames = kDefaultRingFrames;
        ep->ring_header.state = ATRISWAVE_VAD_RING_STATE_STOPPED;

        if (!atriswave_vad_validate_header(&ep->ring_header)) {
            AtrisWaveCleanupEndpoints(ctx);
            return STATUS_INTERNAL_ERROR;
        }

        const size_t bytes =
            static_cast<size_t>(kDefaultRingFrames) *
            static_cast<size_t>(ATRISWAVE_VAD_MAX_CHANNELS) *
            sizeof(float);

        ep->pcm_buffer = static_cast<float*>(
            ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, ATRISWAVE_POOL_TAG));
        if (ep->pcm_buffer == nullptr) {
            AtrisWaveCleanupEndpoints(ctx);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ep->pcm_buffer_bytes = bytes;
        RtlZeroMemory(ep->pcm_buffer, bytes);
    }

    return STATUS_SUCCESS;
}

void AtrisWaveCleanupEndpoints(PATRISWAVE_DEVICE_CONTEXT ctx)
{
    if (ctx == nullptr) {
        return;
    }

    for (uint32_t i = 0; i < ATRISWAVE_VAD_ENDPOINT_COUNT; ++i) {
        AtrisWaveEndpointRuntime* ep = &ctx->endpoints[i];
        if (ep->pcm_buffer != nullptr) {
            ExFreePoolWithTag(ep->pcm_buffer, ATRISWAVE_POOL_TAG);
            ep->pcm_buffer = nullptr;
            ep->pcm_buffer_bytes = 0;
        }
    }
}

NTSTATUS AtrisWaveGetEndpointStats(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id,
    AtrisWaveVadEndpointStats* out_stats)
{
    if (ctx == nullptr || out_stats == nullptr || endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfSpinLockAcquire(ctx->state_lock);
    const AtrisWaveEndpointRuntime* ep = &ctx->endpoints[endpoint_id];
    out_stats->endpoint_id = endpoint_id;
    out_stats->sample_rate = ep->ring_header.sample_rate;
    out_stats->channels = ep->ring_header.channels;
    out_stats->active = ep->active;
    out_stats->xrun_count = ep->ring_header.xrun_count;
    out_stats->reserved = 0u;
    out_stats->frames_transferred = ep->frames_transferred;
    WdfSpinLockRelease(ctx->state_lock);

    return STATUS_SUCCESS;
}

NTSTATUS AtrisWaveSetEndpointFormat(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id,
    uint32_t sample_rate,
    uint32_t channels)
{
    if (ctx == nullptr || endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    if (sample_rate < ATRISWAVE_VAD_MIN_SAMPLE_RATE ||
        sample_rate > ATRISWAVE_VAD_MAX_SAMPLE_RATE ||
        channels < ATRISWAVE_VAD_MIN_CHANNELS ||
        channels > ATRISWAVE_VAD_MAX_CHANNELS) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfSpinLockAcquire(ctx->state_lock);
    AtrisWaveEndpointRuntime* ep = &ctx->endpoints[endpoint_id];
    if (ep->active != 0u) {
        WdfSpinLockRelease(ctx->state_lock);
        return STATUS_DEVICE_BUSY;
    }
    ep->ring_header.sample_rate = sample_rate;
    ep->ring_header.channels = channels;
    ep->ring_header.write_frame_idx = 0u;
    ep->ring_header.read_frame_idx = 0u;
    WdfSpinLockRelease(ctx->state_lock);

    return STATUS_SUCCESS;
}

NTSTATUS AtrisWaveMapEndpointRing(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id,
    AtrisWaveVadRingMapInfo* out_info)
{
    if (ctx == nullptr || out_info == nullptr || endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfSpinLockAcquire(ctx->state_lock);
    AtrisWaveEndpointRuntime* ep = &ctx->endpoints[endpoint_id];
    ep->ring_header.state = ATRISWAVE_VAD_RING_STATE_RUNNING;
    ep->active = 1u;

    out_info->endpoint_id = endpoint_id;
    out_info->capacity_frames = ep->ring_header.capacity_frames;
    out_info->sample_rate = ep->ring_header.sample_rate;
    out_info->channels = ep->ring_header.channels;
    out_info->ring_bytes =
        static_cast<uint64_t>(ep->ring_header.capacity_frames) *
        static_cast<uint64_t>(ep->ring_header.channels) *
        sizeof(float);
    out_info->state = ep->ring_header.state;
    out_info->reserved = 0u;
    WdfSpinLockRelease(ctx->state_lock);

    return STATUS_SUCCESS;
}

/*
 * DISPATCH_LEVEL-safe zero-DSP WaveRT/ACX cyclic ring transfer:
 * - Zero heap allocations
 * - Zero locks (single-producer/single-consumer ring indices)
 * - Zero DSP math in kernel mode
 */
uint32_t AtrisWaveRtPushFramesDispatch(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id,
    const float* src_pcm,
    uint32_t frames)
{
    if (ctx == nullptr || endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT || src_pcm == nullptr || frames == 0u) {
        return 0u;
    }

    AtrisWaveEndpointRuntime* ep = &ctx->endpoints[endpoint_id];
    if (ep->pcm_buffer == nullptr) {
        return 0u;
    }

    const uint32_t written = atriswave_vad_write_frames(&ep->ring_header, ep->pcm_buffer, src_pcm, frames);
    if (written < frames) {
        ep->ring_header.xrun_count += 1u;
    }
    ep->frames_transferred += static_cast<uint64_t>(written);
    return written;
}

uint32_t AtrisWaveRtPullFramesDispatch(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id,
    float* dst_pcm,
    uint32_t frames)
{
    if (ctx == nullptr || endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT || dst_pcm == nullptr || frames == 0u) {
        return 0u;
    }

    AtrisWaveEndpointRuntime* ep = &ctx->endpoints[endpoint_id];
    if (ep->pcm_buffer == nullptr) {
        return 0u;
    }

    const uint32_t read = atriswave_vad_read_frames(&ep->ring_header, ep->pcm_buffer, dst_pcm, frames);
    if (read < frames) {
        ep->ring_header.xrun_count += 1u;
    }
    ep->frames_transferred += static_cast<uint64_t>(read);
    return read;
}
