#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATRISWAVE_PIPE_NAME_A "\\\\.\\pipe\\AtrisWaveEngine"
#define ATRISWAVE_PIPE_NAME_W L"\\\\.\\pipe\\AtrisWaveEngine"

#define ATRISWAVE_TELEMETRY_SHM_NAME_A "Local\\AtrisWaveTelemetryV3"
#define ATRISWAVE_TELEMETRY_SHM_NAME_W L"Local\\AtrisWaveTelemetryV3"

#define ATRISWAVE_TELEMETRY_MAGIC 0x4157544Du /* "AWTM" */
#define ATRISWAVE_TELEMETRY_VERSION 3u
#define ATRISWAVE_MAX_CHANNELS 16u
#define ATRISWAVE_MAX_BUSES 4u /* 0=A1, 1=A2, 2=B1, 3=B2 */
#define ATRISWAVE_SPECTRUM_BINS 32u

enum AtrisWaveEngineState {
    ATRISWAVE_ENGINE_STATE_STOPPED = 0u,
    ATRISWAVE_ENGINE_STATE_RUNNING = 1u,
    ATRISWAVE_ENGINE_STATE_DEGRADED = 2u
};

enum AtrisWaveChannelFlags {
    ATRISWAVE_CHANNEL_FLAG_NONE = 0u,
    ATRISWAVE_CHANNEL_FLAG_MUTED = (1u << 0),
    ATRISWAVE_CHANNEL_FLAG_CLIPPING = (1u << 1),
    ATRISWAVE_CHANNEL_FLAG_ACTIVE = (1u << 2)
};

#pragma pack(push, 4)

typedef struct ChannelMeterFrame {
    uint32_t channel_id;
    float peak_left;
    float peak_right;
    float rms_left;
    float rms_right;
    float gain_reduction_db;
    uint32_t flags;
    uint32_t reserved;
} ChannelMeterFrame;

typedef struct TelemetryHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t channel_count;
    uint32_t sample_rate;
    uint32_t buffer_frames;
    uint32_t engine_state;
    uint64_t sequence;
    uint64_t timestamp_qpc;
    float cpu_load_percent;
    uint32_t xrun_count;
    uint32_t bus_count;
    uint32_t spectrum_bins;
    uint8_t reserved[8];
} TelemetryHeader;

typedef struct TelemetrySnapshot {
    TelemetryHeader header;
    ChannelMeterFrame channels[ATRISWAVE_MAX_CHANNELS];
    ChannelMeterFrame buses[ATRISWAVE_MAX_BUSES];
    float spectrum_db[ATRISWAVE_SPECTRUM_BINS];
} TelemetrySnapshot;

#pragma pack(pop)

#ifdef __cplusplus
} /* extern "C" */

static_assert(sizeof(ChannelMeterFrame) == 32, "ChannelMeterFrame must be 32 bytes");
static_assert(offsetof(ChannelMeterFrame, channel_id) == 0, "channel_id offset mismatch");
static_assert(offsetof(ChannelMeterFrame, peak_left) == 4, "peak_left offset mismatch");
static_assert(offsetof(ChannelMeterFrame, peak_right) == 8, "peak_right offset mismatch");
static_assert(offsetof(ChannelMeterFrame, rms_left) == 12, "rms_left offset mismatch");
static_assert(offsetof(ChannelMeterFrame, rms_right) == 16, "rms_right offset mismatch");
static_assert(offsetof(ChannelMeterFrame, gain_reduction_db) == 20, "gain_reduction_db offset mismatch");
static_assert(offsetof(ChannelMeterFrame, flags) == 24, "flags offset mismatch");
static_assert(offsetof(ChannelMeterFrame, reserved) == 28, "reserved offset mismatch");

static_assert(sizeof(TelemetryHeader) == 64, "TelemetryHeader must be 64 bytes");
static_assert(offsetof(TelemetryHeader, magic) == 0, "magic offset mismatch");
static_assert(offsetof(TelemetryHeader, version) == 4, "version offset mismatch");
static_assert(offsetof(TelemetryHeader, channel_count) == 8, "channel_count offset mismatch");
static_assert(offsetof(TelemetryHeader, sample_rate) == 12, "sample_rate offset mismatch");
static_assert(offsetof(TelemetryHeader, buffer_frames) == 16, "buffer_frames offset mismatch");
static_assert(offsetof(TelemetryHeader, engine_state) == 20, "engine_state offset mismatch");
static_assert(offsetof(TelemetryHeader, sequence) == 24, "sequence offset mismatch");
static_assert(offsetof(TelemetryHeader, sequence) % alignof(uint64_t) == 0, "sequence must be 8-byte aligned for atomic_ref");
static_assert(offsetof(TelemetryHeader, timestamp_qpc) == 32, "timestamp_qpc offset mismatch");
static_assert(offsetof(TelemetryHeader, cpu_load_percent) == 40, "cpu_load_percent offset mismatch");
static_assert(offsetof(TelemetryHeader, xrun_count) == 44, "xrun_count offset mismatch");
static_assert(offsetof(TelemetryHeader, bus_count) == 48, "bus_count offset mismatch");
static_assert(offsetof(TelemetryHeader, spectrum_bins) == 52, "spectrum_bins offset mismatch");
static_assert(offsetof(TelemetryHeader, reserved) == 56, "reserved offset mismatch");

static_assert(sizeof(TelemetrySnapshot) == 832u, "TelemetrySnapshot must be 832 bytes");
static_assert(offsetof(TelemetrySnapshot, header) == 0, "header offset mismatch");
static_assert(offsetof(TelemetrySnapshot, channels) == 64, "channels offset mismatch");
static_assert(offsetof(TelemetrySnapshot, buses) == 576, "buses offset mismatch");
static_assert(offsetof(TelemetrySnapshot, spectrum_db) == 704, "spectrum_db offset mismatch");
#endif
