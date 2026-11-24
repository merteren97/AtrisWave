#include "driver_internal.h"

extern "C" DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT driver_object,
    _In_ PUNICODE_STRING registry_path)
{
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, AtrisWaveEvtDeviceAdd);

    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);

    return WdfDriverCreate(
        driver_object,
        registry_path,
        &attrs,
        &config,
        WDF_NO_HANDLE);
}

NTSTATUS AtrisWaveEvtDeviceAdd(
    _In_ WDFDRIVER driver,
    _Inout_ PWDFDEVICE_INIT device_init)
{
    UNREFERENCED_PARAMETER(driver);

    DECLARE_CONST_UNICODE_STRING(nt_device_name, ATRISWAVE_VAD_NT_DEVICE_NAME_W);
    DECLARE_CONST_UNICODE_STRING(dos_device_name, ATRISWAVE_VAD_DOS_DEVICE_NAME_W);
    /* Restrict control device to SYSTEM, Built-in Admins, and Interactive Users only. */
    DECLARE_CONST_UNICODE_STRING(hardened_sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");

    NTSTATUS status = WdfDeviceInitAssignName(device_init, &nt_device_name);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceInitAssignSDDLString(device_init, &hardened_sddl);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WdfDeviceInitSetDeviceType(device_init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetIoType(device_init, WdfDeviceIoBuffered);

    WDF_OBJECT_ATTRIBUTES device_attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attrs, ATRISWAVE_DEVICE_CONTEXT);
    device_attrs.EvtCleanupCallback = AtrisWaveEvtDeviceContextCleanup;

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&device_init, &device_attrs, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    PATRISWAVE_DEVICE_CONTEXT ctx = AtrisWaveGetDeviceContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));

    WDF_OBJECT_ATTRIBUTES lock_attrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&lock_attrs);
    lock_attrs.ParentObject = device;
    status = WdfSpinLockCreate(&lock_attrs, &ctx->state_lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = AtrisWaveInitializeEndpoints(ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateSymbolicLink(device, &dos_device_name);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(
        device,
        &GUID_DEVINTERFACE_ATRISWAVE_VAD,
        nullptr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return AtrisWaveInitializeControlQueue(device);
}

void AtrisWaveEvtDeviceContextCleanup(_In_ WDFOBJECT device_object)
{
    PATRISWAVE_DEVICE_CONTEXT ctx = AtrisWaveGetDeviceContext(device_object);
    if (ctx != nullptr) {
        AtrisWaveCleanupEndpoints(ctx);
    }
}
