#include "driver_internal.h"

NTSTATUS AtrisWaveCreateControlDevice(PDRIVER_OBJECT driver_object, PATRISWAVE_ADAPTER_CONTEXT ctx)
{
    if (driver_object == nullptr || ctx == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    if (ctx->control_device != nullptr) {
        return STATUS_SUCCESS;
    }

    UNICODE_STRING nt_name;
    UNICODE_STRING dos_name;
    UNICODE_STRING sddl;
    RtlInitUnicodeString(&nt_name, ATRISWAVE_VAD_NT_DEVICE_NAME_W);
    RtlInitUnicodeString(&dos_name, ATRISWAVE_VAD_DOS_DEVICE_NAME_W);
    /* Restrict control device to SYSTEM, Built-in Admins, and Interactive Users only. */
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");

    PDEVICE_OBJECT ctrl_dev = nullptr;
    NTSTATUS status = IoCreateDeviceSecure(
        driver_object,
        0,
        &nt_name,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &sddl,
        &GUID_DEVINTERFACE_ATRISWAVE_VAD,
        &ctrl_dev);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctrl_dev->Flags |= DO_BUFFERED_IO;

    status = IoCreateSymbolicLink(&dos_name, &nt_name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(ctrl_dev);
        return status;
    }

    ctrl_dev->Flags &= ~DO_DEVICE_INITIALIZING;
    ctx->control_device = ctrl_dev;
    return STATUS_SUCCESS;
}

void AtrisWaveDestroyControlDevice(PATRISWAVE_ADAPTER_CONTEXT ctx)
{
    if (ctx == nullptr || ctx->control_device == nullptr) {
        return;
    }

    UNICODE_STRING dos_name;
    RtlInitUnicodeString(&dos_name, ATRISWAVE_VAD_DOS_DEVICE_NAME_W);
    IoDeleteSymbolicLink(&dos_name);
    IoDeleteDevice(ctx->control_device);
    ctx->control_device = nullptr;
}

NTSTATUS AtrisWaveHandleControlIrp(PDEVICE_OBJECT device_object, PIRP irp)
{
    UNREFERENCED_PARAMETER(device_object);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    const UCHAR major = stack->MajorFunction;

    if (major == IRP_MJ_CREATE || major == IRP_MJ_CLOSE) {
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    if (major != IRP_MJ_DEVICE_CONTROL) {
        irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    const ULONG ioctl = stack->Parameters.DeviceIoControl.IoControlCode;
    const ULONG in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sys_buf = irp->AssociatedIrp.SystemBuffer;

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR bytes_returned = 0;

    switch (ioctl) {
        case IOCTL_ATRISWAVE_GET_VERSION: {
            if (sys_buf == nullptr || out_len < sizeof(AtrisWaveVadVersionInfo)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            auto* out_info = static_cast<AtrisWaveVadVersionInfo*>(sys_buf);
            out_info->abi_version = ATRISWAVE_DRIVER_ABI_VERSION;
            out_info->major = ATRISWAVE_VAD_VERSION_MAJOR;
            out_info->minor = ATRISWAVE_VAD_VERSION_MINOR;
            out_info->patch = ATRISWAVE_VAD_VERSION_PATCH;
            out_info->endpoint_count = ATRISWAVE_VAD_ENDPOINT_COUNT;
            out_info->flags = 1u; /* bit 0 = PortCls WaveRT active */
            bytes_returned = sizeof(AtrisWaveVadVersionInfo);
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_ATRISWAVE_GET_ENDPOINT_STATS: {
            if (sys_buf == nullptr ||
                in_len < sizeof(AtrisWaveVadEndpointQueryRequest) ||
                out_len < sizeof(AtrisWaveVadEndpointStats)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            const uint32_t ep_id = static_cast<const AtrisWaveVadEndpointQueryRequest*>(sys_buf)->endpoint_id;
            AtrisWaveVadEndpointStats stats{};
            status = AtrisWaveGetEndpointStats(&g_atriswave_ctx, ep_id, &stats);
            if (NT_SUCCESS(status)) {
                RtlCopyMemory(sys_buf, &stats, sizeof(stats));
                bytes_returned = sizeof(stats);
            }
            break;
        }

        case IOCTL_ATRISWAVE_MAP_ENDPOINT_RING: {
            if (sys_buf == nullptr ||
                in_len < sizeof(AtrisWaveVadEndpointQueryRequest) ||
                out_len < sizeof(AtrisWaveVadRingMapInfo)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            const uint32_t ep_id = static_cast<const AtrisWaveVadEndpointQueryRequest*>(sys_buf)->endpoint_id;
            AtrisWaveVadRingMapInfo map_info{};
            status = AtrisWaveMapEndpointRing(&g_atriswave_ctx, ep_id, &map_info);
            if (NT_SUCCESS(status)) {
                RtlCopyMemory(sys_buf, &map_info, sizeof(map_info));
                bytes_returned = sizeof(map_info);
            }
            break;
        }

        case IOCTL_ATRISWAVE_SET_ENDPOINT_FORMAT: {
            if (sys_buf == nullptr || in_len < sizeof(AtrisWaveVadEndpointFormatRequest)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            const auto* req = static_cast<const AtrisWaveVadEndpointFormatRequest*>(sys_buf);
            status = AtrisWaveSetEndpointFormat(&g_atriswave_ctx, req->endpoint_id, req->sample_rate, req->channels);
            break;
        }

        case IOCTL_ATRISWAVE_SET_ENDPOINT_CONNECTED: {
            if (sys_buf == nullptr || in_len < sizeof(AtrisWaveVadSetConnectedRequest)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            const auto* req = static_cast<const AtrisWaveVadSetConnectedRequest*>(sys_buf);
            status = AtrisWaveSetEndpointConnected(&g_atriswave_ctx, req->endpoint_id, req->connected);
            break;
        }

        case IOCTL_ATRISWAVE_GET_ENDPOINT_STATES: {
            if (sys_buf == nullptr || out_len < sizeof(AtrisWaveVadEndpointStateMask)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            AtrisWaveVadEndpointStateMask mask{};
            AtrisWaveGetEndpointStateMask(&g_atriswave_ctx, &mask);
            RtlCopyMemory(sys_buf, &mask, sizeof(mask));
            bytes_returned = sizeof(mask);
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_ATRISWAVE_WRITE_CAPTURE_PCM: {
            if (sys_buf == nullptr || in_len < sizeof(AtrisWaveVadPcmWriteHeader)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            const auto* hdr = static_cast<const AtrisWaveVadPcmWriteHeader*>(sys_buf);
            if (hdr->endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT ||
                hdr->frames == 0u ||
                hdr->frames > ATRISWAVE_VAD_MAX_PCM_CHUNK_FRAMES ||
                hdr->channels < ATRISWAVE_VAD_MIN_CHANNELS ||
                hdr->channels > ATRISWAVE_VAD_MAX_CHANNELS) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            const size_t expected_pcm_bytes =
                static_cast<size_t>(hdr->frames) * static_cast<size_t>(hdr->channels) * sizeof(float);
            if (static_cast<size_t>(in_len) < sizeof(AtrisWaveVadPcmWriteHeader) + expected_pcm_bytes) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            const auto* samples = reinterpret_cast<const float*>(
                static_cast<const uint8_t*>(sys_buf) + sizeof(AtrisWaveVadPcmWriteHeader));
            (void)AtrisWaveRtPushFramesDispatch(&g_atriswave_ctx, hdr->endpoint_id, samples, hdr->frames);
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_ATRISWAVE_READ_RENDER_PCM: {
            if (sys_buf == nullptr ||
                in_len < sizeof(AtrisWaveVadPcmReadRequest) ||
                out_len < sizeof(AtrisWaveVadPcmReadHeader)) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            const auto req = *static_cast<const AtrisWaveVadPcmReadRequest*>(sys_buf);
            if (req.endpoint_id >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            const AtrisWaveEndpointRuntime* ep = &g_atriswave_ctx.endpoints[req.endpoint_id];
            const uint32_t channels = ep->ring_header.channels;
            const size_t max_out_bytes = static_cast<size_t>(out_len) - sizeof(AtrisWaveVadPcmReadHeader);
            const uint32_t max_fit_frames = (channels > 0u)
                ? static_cast<uint32_t>(max_out_bytes / (static_cast<size_t>(channels) * sizeof(float)))
                : 0u;
            uint32_t to_read = req.max_frames;
            if (to_read > max_fit_frames) {
                to_read = max_fit_frames;
            }
            if (to_read > ATRISWAVE_VAD_MAX_PCM_CHUNK_FRAMES) {
                to_read = ATRISWAVE_VAD_MAX_PCM_CHUNK_FRAMES;
            }

            auto* out_hdr = static_cast<AtrisWaveVadPcmReadHeader*>(sys_buf);
            auto* out_pcm = reinterpret_cast<float*>(
                static_cast<uint8_t*>(sys_buf) + sizeof(AtrisWaveVadPcmReadHeader));
            const uint32_t got = (to_read > 0u)
                ? AtrisWaveRtPullFramesDispatch(&g_atriswave_ctx, req.endpoint_id, out_pcm, to_read)
                : 0u;
            const uint32_t avail = atriswave_vad_available_read(&ep->ring_header, nullptr, nullptr);

            out_hdr->endpoint_id = req.endpoint_id;
            out_hdr->frames_read = got;
            out_hdr->channels = channels;
            out_hdr->available_frames = avail;
            bytes_returned = sizeof(AtrisWaveVadPcmReadHeader) +
                             static_cast<size_t>(got) * static_cast<size_t>(channels) * sizeof(float);
            status = STATUS_SUCCESS;
            break;
        }

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = bytes_returned;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}
