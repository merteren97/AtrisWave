#include "driver_internal.h"

NTSTATUS AtrisWaveInitializeControlQueue(WDFDEVICE device)
{
    PATRISWAVE_DEVICE_CONTEXT ctx = AtrisWaveGetDeviceContext(device);

    WDF_IO_QUEUE_CONFIG queue_config;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config, WdfIoQueueDispatchParallel);
    queue_config.EvtIoDeviceControl = AtrisWaveEvtIoDeviceControl;

    return WdfIoQueueCreate(
        device,
        &queue_config,
        WDF_NO_OBJECT_ATTRIBUTES,
        &ctx->control_queue);
}

void AtrisWaveEvtIoDeviceControl(
    _In_ WDFQUEUE queue,
    _In_ WDFREQUEST request,
    _In_ size_t output_buffer_length,
    _In_ size_t input_buffer_length,
    _In_ ULONG io_control_code)
{
    UNREFERENCED_PARAMETER(output_buffer_length);
    UNREFERENCED_PARAMETER(input_buffer_length);

    WDFDEVICE device = WdfIoQueueGetDevice(queue);
    PATRISWAVE_DEVICE_CONTEXT ctx = AtrisWaveGetDeviceContext(device);

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t bytes_returned = 0;

    switch (io_control_code) {
        case IOCTL_ATRISWAVE_GET_VERSION: {
            AtrisWaveVadVersionInfo* out_info = nullptr;
            status = WdfRequestRetrieveOutputBuffer(
                request,
                sizeof(AtrisWaveVadVersionInfo),
                reinterpret_cast<PVOID*>(&out_info),
                nullptr);
            if (NT_SUCCESS(status)) {
                out_info->abi_version = ATRISWAVE_DRIVER_ABI_VERSION;
                out_info->major = ATRISWAVE_VAD_VERSION_MAJOR;
                out_info->minor = ATRISWAVE_VAD_VERSION_MINOR;
                out_info->patch = ATRISWAVE_VAD_VERSION_PATCH;
                out_info->endpoint_count = ATRISWAVE_VAD_ENDPOINT_COUNT;
                out_info->flags = 0u;
                bytes_returned = sizeof(AtrisWaveVadVersionInfo);
            }
            break;
        }

        case IOCTL_ATRISWAVE_GET_ENDPOINT_STATS: {
            const AtrisWaveVadEndpointQueryRequest* req = nullptr;
            AtrisWaveVadEndpointStats* out_stats = nullptr;

            status = WdfRequestRetrieveInputBuffer(
                request,
                sizeof(AtrisWaveVadEndpointQueryRequest),
                reinterpret_cast<PVOID*>(&req),
                nullptr);
            if (!NT_SUCCESS(status)) {
                break;
            }

            status = WdfRequestRetrieveOutputBuffer(
                request,
                sizeof(AtrisWaveVadEndpointStats),
                reinterpret_cast<PVOID*>(&out_stats),
                nullptr);
            if (!NT_SUCCESS(status)) {
                break;
            }

            if (req->endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            status = AtrisWaveGetEndpointStats(ctx, req->endpoint_id, out_stats);
            if (NT_SUCCESS(status)) {
                bytes_returned = sizeof(AtrisWaveVadEndpointStats);
            }
            break;
        }

        case IOCTL_ATRISWAVE_MAP_ENDPOINT_RING: {
            const AtrisWaveVadEndpointQueryRequest* req = nullptr;
            AtrisWaveVadRingMapInfo* out_map = nullptr;

            status = WdfRequestRetrieveInputBuffer(
                request,
                sizeof(AtrisWaveVadEndpointQueryRequest),
                reinterpret_cast<PVOID*>(&req),
                nullptr);
            if (!NT_SUCCESS(status)) {
                break;
            }

            status = WdfRequestRetrieveOutputBuffer(
                request,
                sizeof(AtrisWaveVadRingMapInfo),
                reinterpret_cast<PVOID*>(&out_map),
                nullptr);
            if (!NT_SUCCESS(status)) {
                break;
            }

            if (req->endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            status = AtrisWaveMapEndpointRing(ctx, req->endpoint_id, out_map);
            if (NT_SUCCESS(status)) {
                bytes_returned = sizeof(AtrisWaveVadRingMapInfo);
            }
            break;
        }

        case IOCTL_ATRISWAVE_SET_ENDPOINT_FORMAT: {
            const AtrisWaveVadEndpointFormatRequest* req = nullptr;
            status = WdfRequestRetrieveInputBuffer(
                request,
                sizeof(AtrisWaveVadEndpointFormatRequest),
                reinterpret_cast<PVOID*>(&req),
                nullptr);
            if (!NT_SUCCESS(status)) {
                break;
            }

            if (req->endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT ||
                req->sample_rate < ATRISWAVE_VAD_MIN_SAMPLE_RATE ||
                req->sample_rate > ATRISWAVE_VAD_MAX_SAMPLE_RATE ||
                req->channels < ATRISWAVE_VAD_MIN_CHANNELS ||
                req->channels > ATRISWAVE_VAD_MAX_CHANNELS) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            status = AtrisWaveSetEndpointFormat(
                ctx,
                req->endpoint_id,
                req->sample_rate,
                req->channels);
            break;
        }

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    WdfRequestCompleteWithInformation(request, status, bytes_returned);
}
