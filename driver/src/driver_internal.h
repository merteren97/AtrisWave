#pragma once

#ifndef _KERNEL_MODE
#define _KERNEL_MODE 1
#endif

#include <portcls.h>
#include <wdmsec.h>
#include <ntstrsafe.h>

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
    const wchar_t* wave_subdevice_name;
    const wchar_t* topo_subdevice_name;
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
    IPortEvents* topo_port_events;
} AtrisWaveEndpointRuntime;

typedef struct ATRISWAVE_ADAPTER_CONTEXT {
    PDEVICE_OBJECT fdo;
    PDEVICE_OBJECT control_device;
    KSPIN_LOCK state_lock;
    LONG initialized;
    AtrisWaveEndpointRuntime endpoints[ATRISWAVE_VAD_ENDPOINT_COUNT];
} ATRISWAVE_ADAPTER_CONTEXT, *PATRISWAVE_ADAPTER_CONTEXT;

/* Global singleton adapter context for sideband control device routing */
extern ATRISWAVE_ADAPTER_CONTEXT g_atriswave_ctx;

/* virtual_endpoints.cpp */
const AtrisWaveEndpointDescriptor* AtrisWaveGetEndpointDescriptors(uint32_t* out_count);
NTSTATUS AtrisWaveInitializeEndpoints(PATRISWAVE_ADAPTER_CONTEXT ctx);
void AtrisWaveCleanupEndpoints(PATRISWAVE_ADAPTER_CONTEXT ctx);

NTSTATUS AtrisWaveGetEndpointStats(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    uint32_t endpoint_id,
    AtrisWaveVadEndpointStats* out_stats);

NTSTATUS AtrisWaveSetEndpointFormat(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    uint32_t endpoint_id,
    uint32_t sample_rate,
    uint32_t channels);

NTSTATUS AtrisWaveMapEndpointRing(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    uint32_t endpoint_id,
    AtrisWaveVadRingMapInfo* out_info);

NTSTATUS AtrisWaveSetEndpointConnected(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    uint32_t endpoint_id,
    uint32_t connected);

void AtrisWaveGetEndpointStateMask(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    AtrisWaveVadEndpointStateMask* out_mask);

void AtrisWaveSignalJackInfoChange(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    uint32_t endpoint_id);

uint32_t AtrisWaveRtPushFramesDispatch(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    uint32_t endpoint_id,
    const float* src_pcm,
    uint32_t frames);

uint32_t AtrisWaveRtPullFramesDispatch(
    PATRISWAVE_ADAPTER_CONTEXT ctx,
    uint32_t endpoint_id,
    float* dst_pcm,
    uint32_t frames);

/* control_device.cpp */
NTSTATUS AtrisWaveCreateControlDevice(PDRIVER_OBJECT driver_object, PATRISWAVE_ADAPTER_CONTEXT ctx);
void AtrisWaveDestroyControlDevice(PATRISWAVE_ADAPTER_CONTEXT ctx);
NTSTATUS AtrisWaveHandleControlIrp(PDEVICE_OBJECT device_object, PIRP irp);

/* wavert_miniport.cpp */
NTSTATUS AtrisWaveCreateMiniportWaveRT(
    _Out_ PUNKNOWN* unknown,
    _In_ PATRISWAVE_ADAPTER_CONTEXT ctx,
    _In_ uint32_t endpoint_id);

NTSTATUS AtrisWaveCreateMiniportTopology(
    _Out_ PUNKNOWN* unknown,
    _In_ PATRISWAVE_ADAPTER_CONTEXT ctx,
    _In_ uint32_t endpoint_id);
