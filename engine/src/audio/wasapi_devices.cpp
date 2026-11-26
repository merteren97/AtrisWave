#include <atriswave/audio/wasapi_devices.hpp>
#include <atriswave/audio/com_utils.hpp>

#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace atriswave::audio {
namespace {

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string get_default_device_id(IMMDeviceEnumerator* enumerator, EDataFlow flow) {
    constexpr ERole kRoles[3] = {eMultimedia, eConsole, eCommunications};
    for (ERole role : kRoles) {
        ComPtr<IMMDevice> def_dev;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, def_dev.put())) || !def_dev) {
            continue;
        }
        LPWSTR raw_id = nullptr;
        if (SUCCEEDED(def_dev->GetId(&raw_id)) && raw_id != nullptr) {
            std::string id = wide_to_utf8(raw_id);
            CoTaskMemFree(raw_id);
            if (!id.empty()) {
                return id;
            }
        }
    }
    return {};
}

void enumerate_flow(IMMDeviceEnumerator* enumerator,
                    EDataFlow flow,
                    bool query_mix_format,
                    std::vector<AudioDeviceInfo>& out) {
    const std::string default_id = get_default_device_id(enumerator, flow);

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put())) || !collection) {
        return;
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return;
    }

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put())) || !device) {
            continue;
        }

        AudioDeviceInfo info{};
        info.is_capture = (flow == eCapture);

        LPWSTR raw_id = nullptr;
        if (SUCCEEDED(device->GetId(&raw_id)) && raw_id != nullptr) {
            info.id = wide_to_utf8(raw_id);
            CoTaskMemFree(raw_id);
        } else {
            continue;
        }

        info.is_default = (!default_id.empty() && info.id == default_id);

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.put())) && props) {
            PROPVARIANT var_name;
            PropVariantInit(&var_name);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var_name)) &&
                var_name.vt == VT_LPWSTR && var_name.pwszVal != nullptr) {
                info.name = wide_to_utf8(var_name.pwszVal);
            }
            PropVariantClear(&var_name);
        }
        if (info.name.empty()) {
            info.name = info.id;
        }

        info.is_virtual = WasapiDeviceEnumerator::is_virtual_device_name(info.name);
        info.virtual_pair_group = WasapiDeviceEnumerator::detect_virtual_pair_group(info.name);

        if (query_mix_format) {
            ComPtr<IAudioClient> audio_client;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, audio_client.put_void())) &&
                audio_client) {
                WAVEFORMATEX* mix_fmt = nullptr;
                if (SUCCEEDED(audio_client->GetMixFormat(&mix_fmt)) && mix_fmt != nullptr) {
                    info.sample_rate = mix_fmt->nSamplesPerSec;
                    info.channels = mix_fmt->nChannels;
                    CoTaskMemFree(mix_fmt);
                }
            }
        }

        out.push_back(std::move(info));
    }
}

} // namespace

bool WasapiDeviceEnumerator::is_virtual_device_name(std::string_view name) noexcept {
    const std::string lower = to_lower_ascii(name);
    return lower.find("cable input") != std::string::npos ||
           lower.find("cable output") != std::string::npos ||
           lower.find("cable-a") != std::string::npos ||
           lower.find("cable-b") != std::string::npos ||
           lower.find("vb-audio") != std::string::npos ||
           lower.find("virtual audio") != std::string::npos ||
           lower.find("atriswave") != std::string::npos ||
           lower.find("voicemeeter") != std::string::npos;
}

std::string WasapiDeviceEnumerator::detect_virtual_pair_group(std::string_view name) {
    const std::string lower = to_lower_ascii(name);
    if (lower.find("cable-a") != std::string::npos) {
        return "vb-cable-a";
    }
    if (lower.find("cable-b") != std::string::npos) {
        return "vb-cable-b";
    }
    if (lower.find("cable input") != std::string::npos ||
        lower.find("cable output") != std::string::npos ||
        lower.find("vb-audio virtual cable") != std::string::npos) {
        return "vb-cable-0";
    }
    if (lower.find("atriswave") != std::string::npos) {
        if (lower.find("system") != std::string::npos) return "atriswave-vad-0";
        if (lower.find("mic") != std::string::npos) return "atriswave-vad-1";
        if (lower.find("stream") != std::string::npos) return "atriswave-vad-2";
        const size_t ch_pos = lower.find("ch ");
        if (ch_pos != std::string::npos && (ch_pos + 3) < lower.size()) {
            const int ch_num = std::atoi(lower.c_str() + ch_pos + 3);
            if (ch_num >= 1 && ch_num <= 16) {
                return "atriswave-vad-" + std::to_string(ch_num - 1);
            }
        }
        return "atriswave-vad-0";
    }
    return {};
}

std::vector<AudioDeviceInfo> WasapiDeviceEnumerator::enumerate_devices(bool query_mix_format) {
    ScopedComInit com;
    std::vector<AudioDeviceInfo> devices;
    if (!com.ok()) {
        return devices;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    const HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        enumerator.put_void());

    if (FAILED(hr) || !enumerator) {
        return devices;
    }

    enumerate_flow(enumerator.get(), eCapture, query_mix_format, devices);
    enumerate_flow(enumerator.get(), eRender, query_mix_format, devices);
    return devices;
}

} // namespace atriswave::audio
