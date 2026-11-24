#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "atriswave/driver_ioctl.h"
#include "atriswave/ring_buffer_validate.h"

#define ATRISWAVE_POOL_TAG 'DWVA'

typedef enum AtrisWaveEndpointDirection {
    ATRISWAVE_ENDPOINT_RENDER = 0,
    ATRISWAVE_ENDPOINT_CAPTURE = 1
} AtrisWaveEndpointDirection;

typedef struct AtrisWaveEndpointDescriptor {
    uint32_t endpoint_id;
    AtrisWaveEndpointDirection direction;
    const wchar_t* friendly_name;
    const wchar_t* pin_name;
    uint32_t default_sample_rate;
    uint32_t default_channels;
    uint32_t default_jack_connected;
    uint32_t presence_detect_capable;
} AtrisWaveEndpointDescriptor;

typedef struct AtrisWaveEndpointRuntime {
    AtrisWaveVadRingHeader ring_header;
    float* pcm_buffer;
    size_t pcm_buffer_bytes;
    uint64_t frames_transferred;
    uint32_t active;
    uint32_t jack_connected;
    uint32_t presence_detect_capable;
    uint32_t reserved;
} AtrisWaveEndpointRuntime;

typedef struct ATRISWAVE_DEVICE_CONTEXT {
    WDFQUEUE control_queue;
    WDFSPINLOCK state_lock;
    AtrisWaveEndpointRuntime endpoints[ATRISWAVE_VAD_ENDPOINT_COUNT];
} ATRISWAVE_DEVICE_CONTEXT, *PATRISWAVE_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ATRISWAVE_DEVICE_CONTEXT, AtrisWaveGetDeviceContext)

/* driver_entry.cpp */
EVT_WDF_DRIVER_DEVICE_ADD AtrisWaveEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP AtrisWaveEvtDeviceContextCleanup;

/* control_device.cpp */
NTSTATUS AtrisWaveInitializeControlQueue(WDFDEVICE device);
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL AtrisWaveEvtIoDeviceControl;

/* virtual_endpoints.cpp */
const AtrisWaveEndpointDescriptor* AtrisWaveGetEndpointDescriptors(uint32_t* out_count);
NTSTATUS AtrisWaveInitializeEndpoints(PATRISWAVE_DEVICE_CONTEXT ctx);
void AtrisWaveCleanupEndpoints(PATRISWAVE_DEVICE_CONTEXT ctx);

NTSTATUS AtrisWaveGetEndpointStats(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id,
    AtrisWaveVadEndpointStats* out_stats);

NTSTATUS AtrisWaveSetEndpointFormat(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id,
    uint32_t sample_rate,
    uint32_t channels);

NTSTATUS AtrisWaveMapEndpointRing(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id,
    AtrisWaveVadRingMapInfo* out_info);

NTSTATUS AtrisWaveSetEndpointConnected(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id,
    uint32_t connected);

void AtrisWaveSignalJackInfoChange(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id);

/* DISPATCH_LEVEL-safe zero-DSP WaveRT/ACX cyclic ring transfer entrypoints */
uint32_t AtrisWaveRtPushFramesDispatch(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id,
    const float* src_pcm,
    uint32_t frames);

uint32_t AtrisWaveRtPullFramesDispatch(
    PATRISWAVE_DEVICE_CONTEXT ctx,
    uint32_t endpoint_id,
    float* dst_pcm,
    uint32_t frames);
