#define PUT_GUIDS_HERE
#include "driver_internal.h"

ATRISWAVE_ADAPTER_CONTEXT g_atriswave_ctx{};

namespace {

PDRIVER_DISPATCH g_orig_create = nullptr;
PDRIVER_DISPATCH g_orig_close = nullptr;
PDRIVER_DISPATCH g_orig_device_control = nullptr;
PDRIVER_UNLOAD g_orig_unload = nullptr;

NTSTATUS AtrisWaveDispatchCreate(PDEVICE_OBJECT dev, PIRP irp)
{
    if (dev == g_atriswave_ctx.control_device) {
        return AtrisWaveHandleControlIrp(dev, irp);
    }
    return (g_orig_create != nullptr) ? g_orig_create(dev, irp) : PcDispatchIrp(dev, irp);
}

NTSTATUS AtrisWaveDispatchClose(PDEVICE_OBJECT dev, PIRP irp)
{
    if (dev == g_atriswave_ctx.control_device) {
        return AtrisWaveHandleControlIrp(dev, irp);
    }
    return (g_orig_close != nullptr) ? g_orig_close(dev, irp) : PcDispatchIrp(dev, irp);
}

NTSTATUS AtrisWaveDispatchDeviceControl(PDEVICE_OBJECT dev, PIRP irp)
{
    if (dev == g_atriswave_ctx.control_device) {
        return AtrisWaveHandleControlIrp(dev, irp);
    }
    return (g_orig_device_control != nullptr) ? g_orig_device_control(dev, irp) : PcDispatchIrp(dev, irp);
}

void AtrisWaveDriverUnload(PDRIVER_OBJECT driver_object)
{
    AtrisWaveDestroyControlDevice(&g_atriswave_ctx);
    AtrisWaveCleanupEndpoints(&g_atriswave_ctx);
    if (g_orig_unload != nullptr) {
        g_orig_unload(driver_object);
    }
}

NTSTATUS RegisterEndpointSubdevices(
    PDEVICE_OBJECT device_object,
    PIRP irp,
    PRESOURCELIST resource_list,
    uint32_t endpoint_id)
{
    uint32_t count = 0;
    const AtrisWaveEndpointDescriptor* descs = AtrisWaveGetEndpointDescriptors(&count);
    if (endpoint_id >= count) {
        return STATUS_INVALID_PARAMETER;
    }
    const AtrisWaveEndpointDescriptor& desc = descs[endpoint_id];

    PPORT port_wave = nullptr;
    PPORT port_topo = nullptr;
    PUNKNOWN miniport_wave = nullptr;
    PUNKNOWN miniport_topo = nullptr;

    NTSTATUS status = PcNewPort(&port_wave, CLSID_PortWaveRT);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = AtrisWaveCreateMiniportWaveRT(&miniport_wave, &g_atriswave_ctx, endpoint_id);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = port_wave->Init(device_object, irp, miniport_wave, nullptr, resource_list);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = PcRegisterSubdevice(device_object, const_cast<PWSTR>(desc.wave_subdevice_name), port_wave);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = PcNewPort(&port_topo, CLSID_PortTopology);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = AtrisWaveCreateMiniportTopology(&miniport_topo, &g_atriswave_ctx, endpoint_id);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = port_topo->Init(device_object, irp, miniport_topo, nullptr, resource_list);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = PcRegisterSubdevice(device_object, const_cast<PWSTR>(desc.topo_subdevice_name), port_topo);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    if (desc.direction == ATRISWAVE_ENDPOINT_RENDER) {
        /* WaveRT Bridge Out (pin 1) -> Topology Bridge In (pin 0) */
        status = PcRegisterPhysicalConnection(device_object, port_wave, 1u, port_topo, 0u);
    } else {
        /* Topology Bridge Out (pin 1) -> WaveRT Bridge In (pin 1) */
        status = PcRegisterPhysicalConnection(device_object, port_topo, 1u, port_wave, 1u);
    }

cleanup:
    if (miniport_topo != nullptr) miniport_topo->Release();
    if (port_topo != nullptr) port_topo->Release();
    if (miniport_wave != nullptr) miniport_wave->Release();
    if (port_wave != nullptr) port_wave->Release();
    return status;
}

NTSTATUS AtrisWaveStartDevice(
    _In_ PDEVICE_OBJECT device_object,
    _In_ PIRP irp,
    _In_ PRESOURCELIST resource_list)
{
    g_atriswave_ctx.fdo = device_object;

    if (InterlockedCompareExchange(&g_atriswave_ctx.initialized, 1, 0) == 0) {
        KeInitializeSpinLock(&g_atriswave_ctx.state_lock);
        NTSTATUS init_st = AtrisWaveInitializeEndpoints(&g_atriswave_ctx);
        if (!NT_SUCCESS(init_st)) {
            InterlockedExchange(&g_atriswave_ctx.initialized, 0);
            return init_st;
        }
        init_st = AtrisWaveCreateControlDevice(device_object->DriverObject, &g_atriswave_ctx);
        if (!NT_SUCCESS(init_st)) {
            AtrisWaveCleanupEndpoints(&g_atriswave_ctx);
            InterlockedExchange(&g_atriswave_ctx.initialized, 0);
            return init_st;
        }
    }

    PRESOURCELIST empty_res = resource_list;
    PRESOURCELIST created_empty = nullptr;
    if (empty_res == nullptr) {
        NTSTATUS rl_st = PcNewResourceList(&created_empty, nullptr, NonPagedPoolNx, nullptr, 0);
        if (!NT_SUCCESS(rl_st)) {
            return rl_st;
        }
        empty_res = created_empty;
    }

    NTSTATUS status = STATUS_SUCCESS;
    for (uint32_t i = 0; i < ATRISWAVE_VAD_ENDPOINT_COUNT; ++i) {
        status = RegisterEndpointSubdevices(device_object, irp, empty_res, i);
        if (!NT_SUCCESS(status)) {
            break;
        }
    }

    if (created_empty != nullptr) {
        created_empty->Release();
    }

    return status;
}

NTSTATUS AtrisWaveAddDevice(
    _In_ PDRIVER_OBJECT driver_object,
    _In_ PDEVICE_OBJECT physical_device_object)
{
    /* 16 endpoints x 2 subdevices (WaveRT + Topology) = 32 subdevices; reserve 48 slots */
    return PcAddAdapterDevice(
        driver_object,
        physical_device_object,
        AtrisWaveStartDevice,
        48,
        0);
}

} // namespace

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT driver_object,
    _In_ PUNICODE_STRING registry_path)
{
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    NTSTATUS status = PcInitializeAdapterDriver(
        driver_object,
        registry_path,
        AtrisWaveAddDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    g_orig_create = driver_object->MajorFunction[IRP_MJ_CREATE];
    g_orig_close = driver_object->MajorFunction[IRP_MJ_CLOSE];
    g_orig_device_control = driver_object->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    g_orig_unload = driver_object->DriverUnload;

    driver_object->MajorFunction[IRP_MJ_CREATE] = AtrisWaveDispatchCreate;
    driver_object->MajorFunction[IRP_MJ_CLOSE] = AtrisWaveDispatchClose;
    driver_object->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AtrisWaveDispatchDeviceControl;
    driver_object->DriverUnload = AtrisWaveDriverUnload;

    return STATUS_SUCCESS;
}
