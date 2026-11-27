#include <atriswave/audio/app_router.hpp>
#include <atriswave/audio/com_utils.hpp>

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <hstring.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace atriswave::audio {
namespace {

std::string query_process_exe_name(DWORD pid) {
    if (pid == 0) {
        return "System Sounds";
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) {
        return "pid:" + std::to_string(pid);
    }

    wchar_t path_buf[MAX_PATH * 2]{};
    DWORD size = static_cast<DWORD>(std::size(path_buf));
    std::string result;
    if (QueryFullProcessImageNameW(proc, 0, path_buf, &size) && size > 0) {
        const std::filesystem::path p(std::wstring_view(path_buf, size));
        result = wide_to_utf8(p.filename().wstring());
    } else {
        result = "pid:" + std::to_string(pid);
    }
    CloseHandle(proc);
    return result;
}

// Undocumented Win10/11 IAudioPolicyConfigFactory COM interface vtable layout.
// Probed safely via RoGetActivationFactory("Windows.Media.Internal.AudioPolicyConfig").
struct IAudioPolicyConfigFactoryV21H2 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved1() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved2() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved3() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved4() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved5() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved6() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved7() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved8() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved9() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved10() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved11() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved12() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved13() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved14() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved15() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved16() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved17() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved18() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved19() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved20() = 0;
    virtual HRESULT STDMETHODCALLTYPE __reserved21() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(
        DWORD processId,
        EDataFlow flow,
        ERole role,
        HSTRING deviceId) = 0;
};

constexpr GUID kIidAudioPolicyConfigV2 = {
    0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};

} // namespace

std::vector<AudioSessionInfo> AppRouter::list_sessions() {
    ScopedComInit com;
    std::vector<AudioSessionInfo> sessions;
    if (!com.ok()) {
        return sessions;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            enumerator.put_void())) ||
        !enumerator) {
        return sessions;
    }

    const EDataFlow flows[2] = {eRender, eCapture};
    for (EDataFlow flow : flows) {
        const bool is_capture_flow = (flow == eCapture);
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put())) ||
            !collection) {
            continue;
        }

        UINT dev_count = 0;
        if (FAILED(collection->GetCount(&dev_count)) || dev_count == 0) {
            continue;
        }

        for (UINT dev_idx = 0; dev_idx < dev_count; ++dev_idx) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(dev_idx, device.put())) || !device) {
                continue;
            }

            std::string device_id;
            LPWSTR raw_dev_id = nullptr;
            if (SUCCEEDED(device->GetId(&raw_dev_id)) && raw_dev_id != nullptr) {
                device_id = wide_to_utf8(raw_dev_id);
                CoTaskMemFree(raw_dev_id);
            }

            ComPtr<IAudioSessionManager2> session_mgr;
            if (FAILED(device->Activate(
                    __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, session_mgr.put_void())) ||
                !session_mgr) {
                continue;
            }

            ComPtr<IAudioSessionEnumerator> session_enum;
            if (FAILED(session_mgr->GetSessionEnumerator(session_enum.put())) || !session_enum) {
                continue;
            }

            int count = 0;
            if (FAILED(session_enum->GetCount(&count)) || count <= 0) {
                continue;
            }

            for (int i = 0; i < count; ++i) {
                ComPtr<IAudioSessionControl> ctrl;
                if (FAILED(session_enum->GetSession(i, ctrl.put())) || !ctrl) {
                    continue;
                }

                AudioSessionState state = AudioSessionStateExpired;
                if (FAILED(ctrl->GetState(&state)) || state == AudioSessionStateExpired) {
                    continue;
                }

                ComPtr<IAudioSessionControl2> ctrl2;
                if (FAILED(ctrl.as(ctrl2)) || !ctrl2) {
                    continue;
                }

                DWORD pid = 0;
                if (FAILED(ctrl2->GetProcessId(&pid)) || pid == 0) {
                    continue;
                }

                float peak = 0.0f;
                ComPtr<IAudioMeterInformation> meter;
                if (SUCCEEDED(ctrl.as(meter)) && meter) {
                    float raw_peak = 0.0f;
                    if (SUCCEEDED(meter->GetPeakValue(&raw_peak))) {
                        peak = raw_peak;
                    }
                }

                // Only surface apps actively producing (render) or capturing (mic) audio
                const bool is_active = (state == AudioSessionStateActive);
                if (!is_active && peak <= 0.0001f) {
                    continue;
                }

                const std::string proc_name = query_process_exe_name(pid);
                if (proc_name.empty() ||
                    _stricmp(proc_name.c_str(), "AtrisWaveEngine.exe") == 0 ||
                    _stricmp(proc_name.c_str(), "AtrisWave.UI.exe") == 0 ||
                    _stricmp(proc_name.c_str(), "AtrisWaveWatchdog.exe") == 0) {
                    continue;
                }

                AudioSessionInfo info{};
                info.pid = static_cast<uint32_t>(pid);
                info.process_name = proc_name;
                info.device_id = device_id;
                info.peak = peak;
                info.is_capture = is_capture_flow;
                info.is_active = is_active;

                LPWSTR raw_display = nullptr;
                if (SUCCEEDED(ctrl->GetDisplayName(&raw_display)) && raw_display != nullptr) {
                    info.display_name = wide_to_utf8(raw_display);
                    CoTaskMemFree(raw_display);
                }
                if (info.display_name.empty() || info.display_name[0] == '@') {
                    info.display_name = info.process_name;
                }

                ComPtr<ISimpleAudioVolume> simple_vol;
                if (SUCCEEDED(ctrl.as(simple_vol)) && simple_vol) {
                    float vol = 1.0f;
                    BOOL muted = FALSE;
                    if (SUCCEEDED(simple_vol->GetMasterVolume(&vol))) {
                        info.volume = vol;
                    }
                    if (SUCCEEDED(simple_vol->GetMute(&muted))) {
                        info.muted = (muted != FALSE);
                    }
                }

                // Deduplicate multi-process apps (e.g. Chrome/Discord) per direction
                auto existing = std::find_if(sessions.begin(), sessions.end(), [&](const AudioSessionInfo& s) {
                    return s.is_capture == info.is_capture &&
                           _stricmp(s.process_name.c_str(), info.process_name.c_str()) == 0;
                });
                if (existing != sessions.end()) {
                    if (info.peak > existing->peak || (info.is_active && !existing->is_active)) {
                        *existing = std::move(info);
                    }
                } else {
                    sessions.push_back(std::move(info));
                }
            }
        }
    }

    std::sort(sessions.begin(), sessions.end(), [](const AudioSessionInfo& a, const AudioSessionInfo& b) {
        if (a.is_capture != b.is_capture) {
            return !a.is_capture;
        }
        return _stricmp(a.process_name.c_str(), b.process_name.c_str()) < 0;
    });

    return sessions;
}

RouteResult AppRouter::route_app_to_device(uint32_t pid, const std::string& target_device_id) {
    if (pid == 0) {
        return {false, "unsupported", "Invalid target PID"};
    }

    HMODULE combase = LoadLibraryW(L"combase.dll");
    if (combase == nullptr) {
        return {false, "unsupported", "combase.dll unavailable"};
    }

    using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
    using WindowsCreateStringFn = HRESULT(WINAPI*)(LPCWSTR, UINT32, HSTRING*);
    using WindowsDeleteStringFn = HRESULT(WINAPI*)(HSTRING);

    auto ro_get_factory = reinterpret_cast<RoGetActivationFactoryFn>(
        GetProcAddress(combase, "RoGetActivationFactory"));
    auto win_create_str = reinterpret_cast<WindowsCreateStringFn>(
        GetProcAddress(combase, "WindowsCreateString"));
    auto win_delete_str = reinterpret_cast<WindowsDeleteStringFn>(
        GetProcAddress(combase, "WindowsDeleteString"));

    if (ro_get_factory == nullptr || win_create_str == nullptr || win_delete_str == nullptr) {
        FreeLibrary(combase);
        return {false, "unsupported", "WinRT activation exports unavailable"};
    }

    ScopedComInit com;
    if (!com.ok()) {
        FreeLibrary(combase);
        return {false, "unsupported", "COM initialization failed"};
    }

    constexpr wchar_t kClassName[] = L"Windows.Media.Internal.AudioPolicyConfig";
    HSTRING class_hstr = nullptr;
    if (FAILED(win_create_str(kClassName, static_cast<UINT32>(std::size(kClassName) - 1), &class_hstr))) {
        FreeLibrary(combase);
        return {false, "unsupported", "Failed to create class HSTRING"};
    }

    ComPtr<IAudioPolicyConfigFactoryV21H2> factory;
    const HRESULT hr = ro_get_factory(class_hstr, kIidAudioPolicyConfigV2, factory.put_void());
    win_delete_str(class_hstr);

    if (FAILED(hr) || !factory) {
        FreeLibrary(combase);
        return {false, "unsupported", "IAudioPolicyConfigFactory unavailable on this Windows build"};
    }

    const std::wstring wide_dev = utf8_to_wide(target_device_id);
    HSTRING dev_hstr = nullptr;
    if (!wide_dev.empty()) {
        if (FAILED(win_create_str(wide_dev.c_str(), static_cast<UINT32>(wide_dev.size()), &dev_hstr))) {
            factory.reset();
            FreeLibrary(combase);
            return {false, "unsupported", "Failed to create device HSTRING"};
        }
    }

    const HRESULT hr_mm = factory->SetPersistedDefaultAudioEndpoint(
        static_cast<DWORD>(pid), eRender, eMultimedia, dev_hstr);
    const HRESULT hr_console = factory->SetPersistedDefaultAudioEndpoint(
        static_cast<DWORD>(pid), eRender, eConsole, dev_hstr);

    if (dev_hstr != nullptr) {
        win_delete_str(dev_hstr);
    }
    factory.reset();
    FreeLibrary(combase);

    if (SUCCEEDED(hr_mm) && SUCCEEDED(hr_console)) {
        return {true, "policy_config", "Routed process via IAudioPolicyConfigFactory"};
    }

    return {false, "unsupported", "SetPersistedDefaultAudioEndpoint rejected or unsupported"};
}

bool AppRouter::set_app_session_volume_mute(std::string_view process_name,
                                            float volume,
                                            bool muted) {
    if (process_name.empty()) {
        return false;
    }

    ScopedComInit com;
    if (!com.ok()) {
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            enumerator.put_void())) ||
        !enumerator) {
        return false;
    }

    ComPtr<IMMDevice> default_render;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, default_render.put())) ||
        !default_render) {
        return false;
    }

    ComPtr<IAudioSessionManager2> session_mgr;
    if (FAILED(default_render->Activate(
            __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, session_mgr.put_void())) ||
        !session_mgr) {
        return false;
    }

    ComPtr<IAudioSessionEnumerator> session_enum;
    if (FAILED(session_mgr->GetSessionEnumerator(session_enum.put())) || !session_enum) {
        return false;
    }

    int count = 0;
    if (FAILED(session_enum->GetCount(&count)) || count <= 0) {
        return false;
    }

    const float clamped_vol = std::clamp(volume, 0.0f, 1.0f);
    bool updated_any = false;

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> ctrl;
        if (FAILED(session_enum->GetSession(i, ctrl.put())) || !ctrl) {
            continue;
        }
        ComPtr<IAudioSessionControl2> ctrl2;
        if (FAILED(ctrl.as(ctrl2)) || !ctrl2) {
            continue;
        }
        DWORD pid = 0;
        if (FAILED(ctrl2->GetProcessId(&pid))) {
            continue;
        }
        const std::string proc = query_process_exe_name(pid);
        if (_stricmp(proc.c_str(), std::string(process_name).c_str()) != 0) {
            continue;
        }
        ComPtr<ISimpleAudioVolume> simple_vol;
        if (SUCCEEDED(ctrl.as(simple_vol)) && simple_vol) {
            const HRESULT hr1 = simple_vol->SetMasterVolume(clamped_vol, nullptr);
            const HRESULT hr2 = simple_vol->SetMute(muted ? TRUE : FALSE, nullptr);
            if (SUCCEEDED(hr1) && SUCCEEDED(hr2)) {
                updated_any = true;
            }
        }
    }

    return updated_any;
}

} // namespace atriswave::audio
