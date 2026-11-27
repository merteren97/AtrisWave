#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <stddef.h>
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
#ifndef UINT64_MAX
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL
#endif
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#else
#include <stddef.h>
#include <stdint.h>
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* {3713CB20-17E2-4673-8615-F26D528D8255} */
EXTERN_C __declspec(selectany) const GUID GUID_DEVINTERFACE_ATRISWAVE_VAD = {
    0x3713cb20u,
    0x17e2u,
    0x4673u,
    { 0x86u, 0x15u, 0xf2u, 0x6du, 0x52u, 0x8du, 0x82u, 0x55u }
};

#define ATRISWAVE_VAD_WIN32_DEVICE_NAME_A "\\\\.\\AtrisWaveVAD"
#define ATRISWAVE_VAD_WIN32_DEVICE_NAME_W L"\\\\.\\AtrisWaveVAD"
#define ATRISWAVE_VAD_NT_DEVICE_NAME_W    L"\\Device\\AtrisWaveVAD"
#define ATRISWAVE_VAD_DOS_DEVICE_NAME_W   L"\\DosDevices\\AtrisWaveVAD"

#define ATRISWAVE_DRIVER_ABI_VERSION  1u
#define ATRISWAVE_VAD_RING_MAGIC      0x41575644u /* "AWVD" */
#define ATRISWAVE_VAD_ENDPOINT_COUNT  16u

#define ATRISWAVE_VAD_MIN_SAMPLE_RATE 8000u
#define ATRISWAVE_VAD_MAX_SAMPLE_RATE 192000u
#define ATRISWAVE_VAD_MIN_CHANNELS    1u
#define ATRISWAVE_VAD_MAX_CHANNELS    8u
#define ATRISWAVE_VAD_MIN_RING_FRAMES 256u
#define ATRISWAVE_VAD_MAX_RING_FRAMES 65536u

#define ATRISWAVE_VAD_VERSION_MAJOR   0u
#define ATRISWAVE_VAD_VERSION_MINOR   2u
#define ATRISWAVE_VAD_VERSION_PATCH   0u

#define ATRISWAVE_VAD_MAX_PCM_CHUNK_FRAMES 2048u

#define IOCTL_ATRISWAVE_GET_VERSION \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_ATRISWAVE_GET_ENDPOINT_STATS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_ATRISWAVE_MAP_ENDPOINT_RING \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_ATRISWAVE_SET_ENDPOINT_FORMAT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_ATRISWAVE_SET_ENDPOINT_CONNECTED \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_ATRISWAVE_WRITE_CAPTURE_PCM \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_ATRISWAVE_READ_RENDER_PCM \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_ATRISWAVE_GET_ENDPOINT_STATES \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_READ_ACCESS)

#ifndef __cplusplus
typedef enum AtrisWaveVadEndpointId {
    ATRISWAVE_VAD_ENDPOINT_SYSTEM = 0,
    ATRISWAVE_VAD_ENDPOINT_MIC_OUT = 1,
    ATRISWAVE_VAD_ENDPOINT_STREAM_MIX = 2,
    ATRISWAVE_VAD_ENDPOINT_CH4 = 3,
    ATRISWAVE_VAD_ENDPOINT_CH5 = 4,
    ATRISWAVE_VAD_ENDPOINT_CH6 = 5,
    ATRISWAVE_VAD_ENDPOINT_CH7 = 6,
    ATRISWAVE_VAD_ENDPOINT_CH8 = 7,
    ATRISWAVE_VAD_ENDPOINT_CH9 = 8,
    ATRISWAVE_VAD_ENDPOINT_CH10 = 9,
    ATRISWAVE_VAD_ENDPOINT_CH11 = 10,
    ATRISWAVE_VAD_ENDPOINT_CH12 = 11,
    ATRISWAVE_VAD_ENDPOINT_CH13 = 12,
    ATRISWAVE_VAD_ENDPOINT_CH14 = 13,
    ATRISWAVE_VAD_ENDPOINT_CH15 = 14,
    ATRISWAVE_VAD_ENDPOINT_CH16 = 15
} AtrisWaveVadEndpointId;
#endif

typedef enum AtrisWaveVadRingState {
    ATRISWAVE_VAD_RING_STATE_STOPPED = 0u,
    ATRISWAVE_VAD_RING_STATE_RUNNING = 1u,
    ATRISWAVE_VAD_RING_STATE_XRUN = 2u
} AtrisWaveVadRingState;

#pragma pack(push, 4)

typedef struct AtrisWaveVadVersionInfo {
    uint32_t abi_version;
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    uint32_t endpoint_count;
    uint32_t flags;
} AtrisWaveVadVersionInfo;

typedef struct AtrisWaveVadRingHeader {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t endpoint_id;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t capacity_frames;
    uint64_t write_frame_idx;
    uint64_t read_frame_idx;
    uint32_t xrun_count;
    uint32_t state;
    uint16_t reserved[8];
} AtrisWaveVadRingHeader;

typedef struct AtrisWaveVadEndpointStats {
    uint32_t endpoint_id;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t active;
    uint32_t xrun_count;
    uint32_t reserved;
    uint64_t frames_transferred;
} AtrisWaveVadEndpointStats;

typedef struct AtrisWaveVadEndpointQueryRequest {
    uint32_t endpoint_id;
    uint32_t reserved;
} AtrisWaveVadEndpointQueryRequest;

typedef struct AtrisWaveVadEndpointFormatRequest {
    uint32_t endpoint_id;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t reserved;
} AtrisWaveVadEndpointFormatRequest;

typedef struct AtrisWaveVadRingMapInfo {
    uint32_t endpoint_id;
    uint32_t capacity_frames;
    uint32_t sample_rate;
    uint32_t channels;
    uint64_t ring_bytes;
    uint32_t state;
    uint32_t reserved;
} AtrisWaveVadRingMapInfo;

typedef struct AtrisWaveVadSetConnectedRequest {
    uint32_t endpoint_id;
    uint32_t connected;
} AtrisWaveVadSetConnectedRequest;

typedef struct AtrisWaveVadPcmWriteHeader {
    uint32_t endpoint_id;
    uint32_t frames;
    uint32_t channels;
    uint32_t reserved;
    /* Followed immediately by frames * channels float32 samples */
} AtrisWaveVadPcmWriteHeader;

typedef struct AtrisWaveVadPcmReadRequest {
    uint32_t endpoint_id;
    uint32_t max_frames;
} AtrisWaveVadPcmReadRequest;

typedef struct AtrisWaveVadPcmReadHeader {
    uint32_t endpoint_id;
    uint32_t frames_read;
    uint32_t channels;
    uint32_t available_frames;
    /* Followed immediately by frames_read * channels float32 samples */
} AtrisWaveVadPcmReadHeader;

typedef struct AtrisWaveVadEndpointStateMask {
    uint32_t connected_mask;
    uint32_t active_mask;
    uint32_t endpoint_count;
    uint32_t reserved;
} AtrisWaveVadEndpointStateMask;

#pragma pack(pop)

#ifdef __cplusplus
} /* extern "C" */

enum class AtrisWaveVadEndpointId : uint32_t {
    System = 0,
    MicOut = 1,
    StreamMix = 2,
    Game = 3,
    Chat = 4,
    Media = 5,
    Aux = 6,
    Ch4 = 3,
    Ch5 = 4,
    Ch6 = 5,
    Ch7 = 6,
    Ch8 = 7,
    Ch9 = 8,
    Ch10 = 9,
    Ch11 = 10,
    Ch12 = 11,
    Ch13 = 12,
    Ch14 = 13,
    Ch15 = 14,
    Ch16 = 15
};

static_assert(sizeof(AtrisWaveVadVersionInfo) == 24, "AtrisWaveVadVersionInfo must be 24 bytes");
static_assert(offsetof(AtrisWaveVadVersionInfo, abi_version) == 0, "abi_version offset mismatch");
static_assert(offsetof(AtrisWaveVadVersionInfo, flags) == 20, "flags offset mismatch");

static_assert(sizeof(AtrisWaveVadRingHeader) == 64, "AtrisWaveVadRingHeader must be 64 bytes");
static_assert(offsetof(AtrisWaveVadRingHeader, magic) == 0, "magic offset mismatch");
static_assert(offsetof(AtrisWaveVadRingHeader, abi_version) == 4, "abi_version offset mismatch");
static_assert(offsetof(AtrisWaveVadRingHeader, endpoint_id) == 8, "endpoint_id offset mismatch");
static_assert(offsetof(AtrisWaveVadRingHeader, sample_rate) == 12, "sample_rate offset mismatch");
static_assert(offsetof(AtrisWaveVadRingHeader, channels) == 16, "channels offset mismatch");
static_assert(offsetof(AtrisWaveVadRingHeader, capacity_frames) == 20, "capacity_frames offset mismatch");
static_assert(offsetof(AtrisWaveVadRingHeader, write_frame_idx) == 24, "write_frame_idx offset mismatch");
static_assert(offsetof(AtrisWaveVadRingHeader, write_frame_idx) % 8 == 0,
              "write_frame_idx must be 8-byte aligned");
static_assert(offsetof(AtrisWaveVadRingHeader, read_frame_idx) == 32, "read_frame_idx offset mismatch");
static_assert(offsetof(AtrisWaveVadRingHeader, read_frame_idx) % 8 == 0,
              "read_frame_idx must be 8-byte aligned");
static_assert(offsetof(AtrisWaveVadRingHeader, xrun_count) == 40, "xrun_count offset mismatch");
static_assert(offsetof(AtrisWaveVadRingHeader, state) == 44, "state offset mismatch");
static_assert(offsetof(AtrisWaveVadRingHeader, reserved) == 48, "reserved offset mismatch");

static_assert(sizeof(AtrisWaveVadEndpointStats) == 32, "AtrisWaveVadEndpointStats must be 32 bytes");
static_assert(offsetof(AtrisWaveVadEndpointStats, frames_transferred) == 24,
              "frames_transferred offset mismatch");
static_assert(offsetof(AtrisWaveVadEndpointStats, frames_transferred) % 8 == 0,
              "frames_transferred must be 8-byte aligned");

static_assert(sizeof(AtrisWaveVadEndpointQueryRequest) == 8,
              "AtrisWaveVadEndpointQueryRequest must be 8 bytes");
static_assert(sizeof(AtrisWaveVadEndpointFormatRequest) == 16,
              "AtrisWaveVadEndpointFormatRequest must be 16 bytes");
static_assert(sizeof(AtrisWaveVadRingMapInfo) == 32,
              "AtrisWaveVadRingMapInfo must be 32 bytes");
static_assert(sizeof(AtrisWaveVadSetConnectedRequest) == 8,
              "AtrisWaveVadSetConnectedRequest must be 8 bytes");
static_assert(sizeof(AtrisWaveVadPcmWriteHeader) == 16,
              "AtrisWaveVadPcmWriteHeader must be 16 bytes");
static_assert(sizeof(AtrisWaveVadPcmReadRequest) == 8,
              "AtrisWaveVadPcmReadRequest must be 8 bytes");
static_assert(sizeof(AtrisWaveVadPcmReadHeader) == 16,
              "AtrisWaveVadPcmReadHeader must be 16 bytes");
static_assert(sizeof(AtrisWaveVadEndpointStateMask) == 16,
              "AtrisWaveVadEndpointStateMask must be 16 bytes");
#endif
