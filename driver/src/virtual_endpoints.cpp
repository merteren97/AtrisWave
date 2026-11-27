#include "driver_internal.h"

static constexpr uint32_t kDefaultRingFrames = 4096u;
static constexpr uint32_t kDefaultSampleRate = 48000u;
static constexpr uint32_t kDefaultChannels = 2u;

/*
 * 16 pre-provisioned pooled virtual audio endpoints:
 * - Slot 0 (System): always connected, catches default Windows audio
 * - Slot 1 (Mic): default connected, processed microphone output (B1 -> Discord / OBS)
 * - Slot 2 (Stream Mix): default connected, broadcast / OBS mix output (B2 -> OBS)
 * - Slot 3 (Game): default connected, dedicated Games playback channel
 * - Slot 4 (Chat): default connected, dedicated Chat / Discord playback channel
 * - Slots 5..15 (Media, Aux, Ch 8..16): dynamic jack-presence pool toggled via IOCTL
 */
static const AtrisWaveEndpointDescriptor kEndpointDescriptors[ATRISWAVE_VAD_ENDPOINT_COUNT] = {
    { 0u,  ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave System",     L"WaveSystem",    L"TopoSystem",    kDefaultSampleRate, kDefaultChannels, 1u, 0u },
    { 1u,  ATRISWAVE_ENDPOINT_CAPTURE, L"AtrisWave Mic",        L"WaveMic",       L"TopoMic",       kDefaultSampleRate, kDefaultChannels, 1u, 1u },
    { 2u,  ATRISWAVE_ENDPOINT_CAPTURE, L"AtrisWave Stream Mix", L"WaveStreamMix", L"TopoStreamMix", kDefaultSampleRate, kDefaultChannels, 1u, 1u },
    { 3u,  ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Game",       L"WaveGame",      L"TopoGame",      kDefaultSampleRate, kDefaultChannels, 1u, 1u },
    { 4u,  ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Chat",       L"WaveChat",      L"TopoChat",      kDefaultSampleRate, kDefaultChannels, 1u, 1u },
    { 5u,  ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Media",      L"WaveMedia",     L"TopoMedia",     kDefaultSampleRate, kDefaultChannels, 0u, 1u },
    { 6u,  ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Aux",        L"WaveAux",       L"TopoAux",       kDefaultSampleRate, kDefaultChannels, 0u, 1u },
    { 7u,  ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Ch 8",       L"WaveCh8",       L"TopoCh8",       kDefaultSampleRate, kDefaultChannels, 0u, 1u },
    { 8u,  ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Ch 9",       L"WaveCh9",       L"TopoCh9",       kDefaultSampleRate, kDefaultChannels, 0u, 1u },
    { 9u,  ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Ch 10",      L"WaveCh10",      L"TopoCh10",      kDefaultSampleRate, kDefaultChannels, 0u, 1u },
    { 10u, ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Ch 11",      L"WaveCh11",      L"TopoCh11",      kDefaultSampleRate, kDefaultChannels, 0u, 1u },
    { 11u, ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Ch 12",      L"WaveCh12",      L"TopoCh12",      kDefaultSampleRate, kDefaultChannels, 0u, 1u },
    { 12u, ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Ch 13",      L"WaveCh13",      L"TopoCh13",      kDefaultSampleRate, kDefaultChannels, 0u, 1u },
    { 13u, ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Ch 14",      L"WaveCh14",      L"TopoCh14",      kDefaultSampleRate, kDefaultChannels, 0u, 1u },
    { 14u, ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Ch 15",      L"WaveCh15",      L"TopoCh15",      kDefaultSampleRate, kDefaultChannels, 0u, 1u },
    { 15u, ATRISWAVE_ENDPOINT_RENDER,  L"AtrisWave Ch 16",      L"WaveCh16",      L"TopoCh16",      kDefaultSampleRate, kDefaultChannels, 0u, 1u }
};

const AtrisWaveEndpointDescriptor* AtrisWaveGetEndpointDescriptors(uint32_t* out_count)
{
    if (out_count != nullptr) {
        *out_count = ATRISWAVE_VAD_ENDPOINT_COUNT;
    }
    return kEndpointDescriptors;
}

NTSTATUS AtrisWaveInitializeEndpoints(PATRISWAVE_ADAPTER_CONTEXT ctx)
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
        ep->jack_connected = desc->default_jack_connected;
        ep->presence_detect_capable = desc->presence_detect_capable;

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

void AtrisWaveCleanupEndpoints(PATRISWAVE_ADAPTER_CONTEXT ctx)
{
    if (ctx == nullptr) {
        return;
    }

    for (uint32_t i = 0; i < ATRISWAVE_VAD_ENDPOINT_COUNT; ++i) {
        AtrisWaveEndpointRuntime* ep = &ctx->endpoints[i];
        ep->topo_port_events = nullptr;
        if (ep->pcm_buffer != nullptr) {
            ExFreePoolWithTag(ep->pcm_buffer, ATRISWAVE_POOL_TAG);
            ep->pcm_buffer = nullptr;
            ep->pcm_buffer_bytes = 0;
        }
    }
}

NTSTATUS AtrisWaveGetEndpointStats(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    uint32_t endpoint_id,
    AtrisWaveVadEndpointStats* out_stats)
{
    if (ctx == nullptr || out_stats == nullptr || endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL old_irql;
    KeAcquireSpinLock(&ctx->state_lock, &old_irql);
    const AtrisWaveEndpointRuntime* ep = &ctx->endpoints[endpoint_id];
    out_stats->endpoint_id = endpoint_id;
    out_stats->sample_rate = ep->ring_header.sample_rate;
    out_stats->channels = ep->ring_header.channels;
    out_stats->active = ep->active;
    out_stats->xrun_count = ep->ring_header.xrun_count;
    out_stats->reserved = ep->jack_connected;
    out_stats->frames_transferred = ep->frames_transferred;
    KeReleaseSpinLock(&ctx->state_lock, old_irql);

    return STATUS_SUCCESS;
}

NTSTATUS AtrisWaveSetEndpointFormat(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
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

    KIRQL old_irql;
    KeAcquireSpinLock(&ctx->state_lock, &old_irql);
    AtrisWaveEndpointRuntime* ep = &ctx->endpoints[endpoint_id];
    if (ep->active != 0u) {
        KeReleaseSpinLock(&ctx->state_lock, old_irql);
        return STATUS_DEVICE_BUSY;
    }
    ep->ring_header.sample_rate = sample_rate;
    ep->ring_header.channels = channels;
    ep->ring_header.write_frame_idx = 0u;
    ep->ring_header.read_frame_idx = 0u;
    KeReleaseSpinLock(&ctx->state_lock, old_irql);

    return STATUS_SUCCESS;
}

NTSTATUS AtrisWaveMapEndpointRing(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    uint32_t endpoint_id,
    AtrisWaveVadRingMapInfo* out_info)
{
    if (ctx == nullptr || out_info == nullptr || endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL old_irql;
    KeAcquireSpinLock(&ctx->state_lock, &old_irql);
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
    KeReleaseSpinLock(&ctx->state_lock, old_irql);

    return STATUS_SUCCESS;
}

NTSTATUS AtrisWaveSetEndpointConnected(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    uint32_t endpoint_id,
    uint32_t connected)
{
    if (ctx == nullptr || endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Slot 0 (AtrisWave System) is always connected to prevent silent default-device loss. */
    if (endpoint_id == 0u && connected == 0u) {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL old_irql;
    KeAcquireSpinLock(&ctx->state_lock, &old_irql);
    AtrisWaveEndpointRuntime* ep = &ctx->endpoints[endpoint_id];
    ep->jack_connected = (connected != 0u) ? 1u : 0u;
    if (ep->jack_connected == 0u) {
        ep->active = 0u;
        ep->ring_header.state = ATRISWAVE_VAD_RING_STATE_STOPPED;
        ep->ring_header.write_frame_idx = 0u;
        ep->ring_header.read_frame_idx = 0u;
    }
    KeReleaseSpinLock(&ctx->state_lock, old_irql);

    AtrisWaveSignalJackInfoChange(ctx, endpoint_id);
    return STATUS_SUCCESS;
}

void AtrisWaveGetEndpointStateMask(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    AtrisWaveVadEndpointStateMask* out_mask)
{
    if (ctx == nullptr || out_mask == nullptr) {
        return;
    }

    uint32_t conn_mask = 0u;
    uint32_t act_mask = 0u;

    KIRQL old_irql;
    KeAcquireSpinLock(&ctx->state_lock, &old_irql);
    for (uint32_t i = 0; i < ATRISWAVE_VAD_ENDPOINT_COUNT; ++i) {
        if (ctx->endpoints[i].jack_connected != 0u) {
            conn_mask |= (1u << i);
        }
        if (ctx->endpoints[i].active != 0u) {
            act_mask |= (1u << i);
        }
    }
    KeReleaseSpinLock(&ctx->state_lock, old_irql);

    out_mask->connected_mask = conn_mask;
    out_mask->active_mask = act_mask;
    out_mask->endpoint_count = ATRISWAVE_VAD_ENDPOINT_COUNT;
    out_mask->reserved = 0u;
}

void AtrisWaveSignalJackInfoChange(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    uint32_t endpoint_id)
{
    if (ctx == nullptr || endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
        return;
    }

    IPortEvents* port_events = nullptr;
    KIRQL old_irql;
    KeAcquireSpinLock(&ctx->state_lock, &old_irql);
    port_events = ctx->endpoints[endpoint_id].topo_port_events;
    KeReleaseSpinLock(&ctx->state_lock, old_irql);

    if (port_events != nullptr) {
        GUID event_set = KSEVENTSETID_PinCapsChange;
        const ULONG pin_id = (kEndpointDescriptors[endpoint_id].direction == ATRISWAVE_ENDPOINT_RENDER) ? 1u : 0u;
        port_events->GenerateEventList(
            &event_set,
            KSEVENT_PINCAPS_JACKINFOCHANGE,
            TRUE,
            pin_id,
            FALSE,
            ULONG(-1));
    }
}

uint32_t AtrisWaveRtPushFramesDispatch(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
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
    PATRISWAVE_ADAPTER_CONTEXT ctx,
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
