#include <atriswave/audio/wasapi_stream.hpp>
#include <atriswave/audio/com_utils.hpp>
#include <atriswave/rt_safety.hpp>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace atriswave::audio {

struct WasapiStream::Impl {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioRenderClient> render;
};

namespace {

bool detect_sample_format(const WAVEFORMATEX* fmt,
                          uint32_t& bits_out,
                          bool& is_float_out) noexcept {
    if (fmt == nullptr) {
        return false;
    }
    bits_out = fmt->wBitsPerSample;

    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && fmt->wBitsPerSample == 32) {
        is_float_out = true;
        return true;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_PCM &&
        (fmt->wBitsPerSample == 16 || fmt->wBitsPerSample == 24 || fmt->wBitsPerSample == 32)) {
        is_float_out = false;
        return true;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        fmt->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && fmt->wBitsPerSample == 32) {
            is_float_out = true;
            return true;
        }
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
            (fmt->wBitsPerSample == 16 || fmt->wBitsPerSample == 24 || fmt->wBitsPerSample == 32)) {
            is_float_out = false;
            return true;
        }
    }
    return false;
}

void convert_to_float32(const uint8_t* src,
                        float* dst,
                        size_t sample_count,
                        uint32_t bits_per_sample,
                        bool is_float) noexcept {
    if (is_float) {
        std::memcpy(dst, src, sample_count * sizeof(float));
        return;
    }
    if (bits_per_sample == 16) {
        const auto* s16 = reinterpret_cast<const int16_t*>(src);
        constexpr float kScale = 1.0f / 32768.0f;
        for (size_t i = 0; i < sample_count; ++i) {
            dst[i] = static_cast<float>(s16[i]) * kScale;
        }
    } else if (bits_per_sample == 24) {
        constexpr float kScale = 1.0f / 8388608.0f;
        for (size_t i = 0; i < sample_count; ++i) {
            const uint8_t* p = src + i * 3;
            int32_t val = static_cast<int32_t>(p[0]) |
                          (static_cast<int32_t>(p[1]) << 8) |
                          (static_cast<int32_t>(static_cast<int8_t>(p[2])) << 16);
            dst[i] = static_cast<float>(val) * kScale;
        }
    } else if (bits_per_sample == 32) {
        const auto* s32 = reinterpret_cast<const int32_t*>(src);
        constexpr float kScale = 1.0f / 2147483648.0f;
        for (size_t i = 0; i < sample_count; ++i) {
            dst[i] = static_cast<float>(s32[i]) * kScale;
        }
    }
}

void convert_from_float32(const float* src,
                          uint8_t* dst,
                          size_t sample_count,
                          uint32_t bits_per_sample,
                          bool is_float) noexcept {
    if (is_float) {
        std::memcpy(dst, src, sample_count * sizeof(float));
        return;
    }
    if (bits_per_sample == 16) {
        auto* d16 = reinterpret_cast<int16_t*>(dst);
        for (size_t i = 0; i < sample_count; ++i) {
            const float clamped = std::clamp(src[i], -1.0f, 1.0f);
            d16[i] = static_cast<int16_t>(clamped * 32767.0f);
        }
    } else if (bits_per_sample == 24) {
        for (size_t i = 0; i < sample_count; ++i) {
            const float clamped = std::clamp(src[i], -1.0f, 1.0f);
            const int32_t val = static_cast<int32_t>(clamped * 8388607.0f);
            uint8_t* p = dst + i * 3;
            p[0] = static_cast<uint8_t>(val & 0xFF);
            p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
            p[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
        }
    } else if (bits_per_sample == 32) {
        auto* d32 = reinterpret_cast<int32_t*>(dst);
        for (size_t i = 0; i < sample_count; ++i) {
            const float clamped = std::clamp(src[i], -1.0f, 1.0f);
            d32[i] = static_cast<int32_t>(clamped * 2147483647.0f);
        }
    }
}

} // namespace

WasapiStream::~WasapiStream() {
    close();
}

bool WasapiStream::open(const std::string& device_id,
                        StreamDirection direction,
                        uint32_t /*preferred_sample_rate*/,
                        uint32_t /*preferred_channels*/) {
    close();
    ScopedComInit com;
    if (!com.ok()) {
        return false;
    }

    direction_ = direction;
    device_invalidated_.store(false, std::memory_order_release);

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

    auto impl = new Impl();

    HRESULT hr = S_OK;
    if (device_id.empty()) {
        const EDataFlow flow = (direction == StreamDirection::Capture) ? eCapture : eRender;
        hr = enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, impl->device.put());
    } else {
        const std::wstring wide_id = utf8_to_wide(device_id);
        hr = enumerator->GetDevice(wide_id.c_str(), impl->device.put());
    }

    if (FAILED(hr) || !impl->device) {
        delete impl;
        return false;
    }

    // Prefer IAudioClient3 for low-latency hardware-floor shared mode
    ComPtr<IAudioClient3> client3;
    hr = impl->device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, client3.put_void());
    if (SUCCEEDED(hr) && client3) {
        hr = client3.as(impl->client);
    } else {
        hr = impl->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, impl->client.put_void());
    }

    if (FAILED(hr) || !impl->client) {
        delete impl;
        return false;
    }

    WAVEFORMATEX* mix_fmt = nullptr;
    if (FAILED(impl->client->GetMixFormat(&mix_fmt)) || mix_fmt == nullptr) {
        delete impl;
        return false;
    }

    uint32_t bits = 32;
    bool is_float = true;
    if (!detect_sample_format(mix_fmt, bits, is_float)) {
        CoTaskMemFree(mix_fmt);
        delete impl;
        return false;
    }

    sample_rate_ = mix_fmt->nSamplesPerSec;
    channels_ = mix_fmt->nChannels;
    bits_per_sample_ = bits;
    if (is_float) {
        sample_format_ = SampleFormat::Float32;
    } else if (bits == 16) {
        sample_format_ = SampleFormat::Pcm16;
    } else if (bits == 24) {
        sample_format_ = SampleFormat::Pcm24;
    } else {
        sample_format_ = SampleFormat::Pcm32;
    }

    bool initialized = false;
    const bool is_loopback = (direction == StreamDirection::Loopback);
    if (!is_loopback && client3) {
        UINT32 default_period = 0;
        UINT32 fundamental_period = 0;
        UINT32 min_period = 0;
        UINT32 max_period = 0;
        if (SUCCEEDED(client3->GetSharedModeEnginePeriod(
                mix_fmt, &default_period, &fundamental_period, &min_period, &max_period)) &&
            (default_period > 0 || min_period > 0)) {
            const UINT32 chosen_period = (default_period > 0) ? default_period : min_period;
            const HRESULT init3_hr = client3->InitializeSharedAudioStream(
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                chosen_period,
                mix_fmt,
                nullptr);
            if (SUCCEEDED(init3_hr)) {
                initialized = true;
            }
        }
    }

    if (!initialized) {
        const DWORD stream_flags = is_loopback
            ? AUDCLNT_STREAMFLAGS_LOOPBACK
            : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        const HRESULT init_hr = impl->client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            stream_flags,
            is_loopback ? 200000 : 0,
            0,
            mix_fmt,
            nullptr);
        initialized = SUCCEEDED(init_hr);
    }

    CoTaskMemFree(mix_fmt);

    if (!initialized) {
        delete impl;
        return false;
    }

    UINT32 buf_frames = 0;
    if (FAILED(impl->client->GetBufferSize(&buf_frames)) || buf_frames == 0) {
        delete impl;
        return false;
    }
    buffer_frames_ = buf_frames;

    HANDLE audio_evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE stop_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (audio_evt == nullptr || stop_evt == nullptr) {
        if (audio_evt != nullptr) CloseHandle(audio_evt);
        if (stop_evt != nullptr) CloseHandle(stop_evt);
        delete impl;
        return false;
    }

    if (!is_loopback && FAILED(impl->client->SetEventHandle(audio_evt))) {
        CloseHandle(audio_evt);
        CloseHandle(stop_evt);
        delete impl;
        return false;
    }

    if (direction == StreamDirection::Capture || is_loopback) {
        hr = impl->client->GetService(__uuidof(IAudioCaptureClient), impl->capture.put_void());
    } else {
        hr = impl->client->GetService(__uuidof(IAudioRenderClient), impl->render.put_void());
    }

    if (FAILED(hr)) {
        CloseHandle(audio_evt);
        CloseHandle(stop_evt);
        delete impl;
        return false;
    }

    audio_event_ = audio_evt;
    stop_event_ = stop_evt;
    audio_client_ = impl;
    return true;
}

bool WasapiStream::start(AudioCallback callback) {
    if (audio_client_ == nullptr || running_.load(std::memory_order_acquire)) {
        return false;
    }
    callback_ = std::move(callback);
    ResetEvent(static_cast<HANDLE>(stop_event_));

    if (FAILED(audio_client_->client->Start())) {
        return false;
    }

    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&WasapiStream::audio_thread_loop, this);
    return true;
}

void WasapiStream::stop() {
    running_.store(false, std::memory_order_release);
    if (stop_event_ != nullptr) {
        SetEvent(static_cast<HANDLE>(stop_event_));
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (audio_client_ != nullptr && audio_client_->client) {
        audio_client_->client->Stop();
    }
}

void WasapiStream::close() {
    stop();
    if (audio_event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(audio_event_));
        audio_event_ = nullptr;
    }
    if (stop_event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }
    delete audio_client_;
    audio_client_ = nullptr;
}

void WasapiStream::audio_thread_loop() {
    ScopedComInit com;
    MmcssGuard mmcss(L"Pro Audio");
    DenormalGuard denormal;

    const bool is_float = (sample_format_ == SampleFormat::Float32);
    const size_t max_frames = std::max<size_t>(buffer_frames_, 4096);
    std::vector<float> scratch(max_frames * channels_, 0.0f);

    HANDLE handles[2] = {
        static_cast<HANDLE>(stop_event_),
        static_cast<HANDLE>(audio_event_)
    };

    while (running_.load(std::memory_order_acquire)) {
        if (direction_ == StreamDirection::Loopback) {
            if (WaitForSingleObject(static_cast<HANDLE>(stop_event_), 5) == WAIT_OBJECT_0) {
                break;
            }
        } else {
            const DWORD wait_res = WaitForMultipleObjects(2, handles, FALSE, 500);
            if (wait_res == WAIT_OBJECT_0) {
                break;
            }
            if (wait_res != WAIT_OBJECT_0 + 1) {
                continue;
            }
        }

        if (direction_ == StreamDirection::Capture || direction_ == StreamDirection::Loopback) {
            UINT32 packet_frames = 0;
            HRESULT hr = audio_client_->capture->GetNextPacketSize(&packet_frames);
            while (SUCCEEDED(hr) && packet_frames > 0) {
                BYTE* data = nullptr;
                UINT32 frames_available = 0;
                DWORD flags = 0;
                hr = audio_client_->capture->GetBuffer(&data, &frames_available, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    break;
                }

                const size_t safe_frames = std::min<size_t>(frames_available, max_frames);
                const size_t sample_count = safe_frames * channels_;

                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                    std::fill_n(scratch.data(), sample_count, 0.0f);
                } else {
                    convert_to_float32(data, scratch.data(), sample_count, bits_per_sample_, is_float);
                }

                audio_client_->capture->ReleaseBuffer(frames_available);

                if (callback_ && safe_frames > 0) {
                    callback_(scratch.data(), static_cast<uint32_t>(safe_frames), channels_);
                }

                hr = audio_client_->capture->GetNextPacketSize(&packet_frames);
            }

            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                device_invalidated_.store(true, std::memory_order_release);
                break;
            }
        } else {
            UINT32 padding = 0;
            HRESULT hr = audio_client_->client->GetCurrentPadding(&padding);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                device_invalidated_.store(true, std::memory_order_release);
                break;
            }
            if (FAILED(hr)) {
                continue;
            }

            const UINT32 frames_to_write = (buffer_frames_ > padding) ? (buffer_frames_ - padding) : 0;
            if (frames_to_write == 0) {
                continue;
            }

            const size_t safe_frames = std::min<size_t>(frames_to_write, max_frames);

            BYTE* data = nullptr;
            hr = audio_client_->render->GetBuffer(static_cast<UINT32>(safe_frames), &data);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                device_invalidated_.store(true, std::memory_order_release);
                break;
            }
            if (FAILED(hr) || data == nullptr) {
                continue;
            }

            const size_t sample_count = safe_frames * channels_;
            if (callback_) {
                callback_(scratch.data(), static_cast<uint32_t>(safe_frames), channels_);
            } else {
                std::fill_n(scratch.data(), sample_count, 0.0f);
            }

            convert_from_float32(scratch.data(), data, sample_count, bits_per_sample_, is_float);
            audio_client_->render->ReleaseBuffer(static_cast<UINT32>(safe_frames), 0);
        }
    }

    running_.store(false, std::memory_order_release);
}

} // namespace atriswave::audio
