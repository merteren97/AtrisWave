#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <avrt.h>
#include <pmmintrin.h>
#include <xmmintrin.h>

#include "atriswave/plugin_ipc.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <utility>

namespace {

class ScopedHandle final {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedHandle() noexcept {
        reset();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE next = nullptr) noexcept {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = next;
    }

private:
    HANDLE handle_{nullptr};
};

class MmcssGuard final {
public:
    explicit MmcssGuard(const wchar_t* task_name = L"Pro Audio") noexcept {
        if (task_name != nullptr) {
            handle_ = AvSetMmThreadCharacteristicsW(task_name, &task_index_);
            if (handle_ != nullptr) {
                AvSetMmThreadPriority(handle_, AVRT_PRIORITY_CRITICAL);
            }
        }
    }

    ~MmcssGuard() noexcept {
        if (handle_ != nullptr) {
            AvRevertMmThreadCharacteristics(handle_);
            handle_ = nullptr;
        }
    }

    MmcssGuard(const MmcssGuard&) = delete;
    MmcssGuard& operator=(const MmcssGuard&) = delete;

private:
    HANDLE handle_{nullptr};
    DWORD task_index_{0};
};

void enable_ftz_daz() noexcept {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}

struct HostOptions {
    std::wstring slot_name;
    std::wstring plugin_path;
    uint32_t sample_rate{48000u};
};

bool parse_args(int argc, wchar_t** argv, HostOptions& out) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--slot" && (i + 1) < argc) {
            out.slot_name = argv[++i];
        } else if (arg == L"--plugin" && (i + 1) < argc) {
            out.plugin_path = argv[++i];
        } else if (arg == L"--sample-rate" && (i + 1) < argc) {
            const unsigned long parsed = std::wcstoul(argv[++i], nullptr, 10);
            if (parsed == 0ul || parsed > 384000ul) {
                return false;
            }
            out.sample_rate = static_cast<uint32_t>(parsed);
        } else {
            return false;
        }
    }
    return !out.slot_name.empty() && !out.plugin_path.empty();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    HostOptions options{};
    if (!parse_args(argc, argv, options)) {
        return 1;
    }

    const std::wstring shm_name = L"Local\\" + options.slot_name;
    const std::wstring req_event_name = shm_name + L"_req";
    const std::wstring done_event_name = shm_name + L"_done";

    ScopedHandle shm_handle(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shm_name.c_str()));
    if (!shm_handle.valid()) {
        return 2;
    }

    auto* slot = static_cast<AtrisWavePluginSlotLayout*>(
        MapViewOfFile(shm_handle.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AtrisWavePluginSlotLayout)));
    if (slot == nullptr) {
        return 3;
    }

    struct MappingUnmapper {
        AtrisWavePluginSlotLayout* ptr{nullptr};
        ~MappingUnmapper() noexcept {
            if (ptr != nullptr) {
                UnmapViewOfFile(ptr);
            }
        }
    } unmapper{slot};

    if (slot->magic != ATRISWAVE_PLUGIN_IPC_MAGIC ||
        slot->abi_version != ATRISWAVE_PLUGIN_IPC_ABI_VERSION ||
        slot->max_frames != ATRISWAVE_PLUGIN_MAX_FRAMES) {
        slot->status = ATRISWAVE_PLUGIN_STATUS_ERROR;
        return 4;
    }

    ScopedHandle req_event(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, req_event_name.c_str()));
    ScopedHandle done_event(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, done_event_name.c_str()));
    if (!req_event.valid() || !done_event.valid()) {
        slot->status = ATRISWAVE_PLUGIN_STATUS_ERROR;
        return 5;
    }

    HMODULE plugin_module = LoadLibraryW(options.plugin_path.c_str());
    if (plugin_module == nullptr) {
        slot->status = ATRISWAVE_PLUGIN_STATUS_ERROR;
        SetEvent(done_event.get());
        return 6;
    }

    struct ModuleFreer {
        HMODULE mod{nullptr};
        ~ModuleFreer() noexcept {
            if (mod != nullptr) {
                FreeLibrary(mod);
            }
        }
    } module_freer{plugin_module};

    const auto exports = atriswave::discover_plugin_dll_exports(plugin_module);
    if (exports.has_atriswave_c == 0u) {
        // CLAP / VST3 DLLs are recognized cleanly via exports; C-ABI is required for DSP execution here.
        slot->status = ATRISWAVE_PLUGIN_STATUS_ERROR;
        SetEvent(done_event.get());
        return (exports.detected_format != ATRISWAVE_PLUGIN_FORMAT_UNKNOWN) ? 7 : 8;
    }

    const auto get_api = reinterpret_cast<AtrisWavePluginGetApiFn>(
        GetProcAddress(plugin_module, ATRISWAVE_PLUGIN_GET_API_SYMBOL));
    if (get_api == nullptr) {
        slot->status = ATRISWAVE_PLUGIN_STATUS_ERROR;
        SetEvent(done_event.get());
        return 9;
    }

    const AtrisWavePluginApi* api = get_api();
    if (api == nullptr ||
        api->abi_version != ATRISWAVE_PLUGIN_IPC_ABI_VERSION ||
        api->create == nullptr ||
        api->destroy == nullptr ||
        api->process == nullptr) {
        slot->status = ATRISWAVE_PLUGIN_STATUS_ERROR;
        SetEvent(done_event.get());
        return 10;
    }

    void* instance = api->create(options.sample_rate, ATRISWAVE_PLUGIN_MAX_FRAMES);
    if (instance == nullptr) {
        slot->status = ATRISWAVE_PLUGIN_STATUS_ERROR;
        SetEvent(done_event.get());
        return 11;
    }

    struct PluginDestroyer {
        const AtrisWavePluginApi* api{nullptr};
        void* instance{nullptr};
        ~PluginDestroyer() noexcept {
            if (api != nullptr && instance != nullptr && api->destroy != nullptr) {
                api->destroy(instance);
            }
        }
    } destroyer{api, instance};

    MmcssGuard mmcss_guard(L"Pro Audio");
    enable_ftz_daz();

    slot->sample_rate = options.sample_rate;
    slot->status = ATRISWAVE_PLUGIN_STATUS_READY;
    SetEvent(done_event.get());

    for (;;) {
        const DWORD wait_rc = WaitForSingleObject(req_event.get(), INFINITE);
        if (wait_rc != WAIT_OBJECT_0) {
            slot->status = ATRISWAVE_PLUGIN_STATUS_ERROR;
            SetEvent(done_event.get());
            break;
        }

        const uint32_t cmd = slot->command;
        if (cmd == ATRISWAVE_PLUGIN_CMD_PROCESS) {
            const uint32_t frames = std::min(slot->num_frames, ATRISWAVE_PLUGIN_MAX_FRAMES);
            api->process(
                instance,
                slot->in_left,
                slot->in_right,
                slot->out_left,
                slot->out_right,
                frames);
            slot->status = ATRISWAVE_PLUGIN_STATUS_READY;
            SetEvent(done_event.get());
        } else if (cmd == ATRISWAVE_PLUGIN_CMD_SET_PARAM) {
            if (api->set_param != nullptr) {
                api->set_param(instance, slot->param_index, slot->param_value);
            }
            slot->status = ATRISWAVE_PLUGIN_STATUS_READY;
            SetEvent(done_event.get());
        } else if (cmd == ATRISWAVE_PLUGIN_CMD_SHUTDOWN) {
            slot->status = ATRISWAVE_PLUGIN_STATUS_IDLE;
            SetEvent(done_event.get());
            break;
        } else if (cmd == ATRISWAVE_PLUGIN_CMD_SIMULATE_CRASH) {
            TerminateProcess(GetCurrentProcess(), 0xC0000005u);
        } else {
            slot->status = ATRISWAVE_PLUGIN_STATUS_ERROR;
            SetEvent(done_event.get());
        }
    }

    return 0;
}
