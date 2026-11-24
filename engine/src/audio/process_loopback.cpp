#include <atriswave/audio/process_loopback.hpp>
#include <atriswave/audio/com_utils.hpp>
#include <atriswave/rt_safety.hpp>

#include <audioclient.h>
#include <mmdeviceapi.h>

#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#define ATRISWAVE_HAS_PROCESS_LOOPBACK_SDK 1
#else
#define ATRISWAVE_HAS_PROCESS_LOOPBACK_SDK 0
#endif

#include <algorithm>
#include <cstring>
#include <vector>

namespace atriswave::audio {

struct ProcessLoopbackCapture::Impl {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    uint32_t sample_rate{48000};
    uint32_t channels{2};
    uint32_t buffer_frames{480};
};

namespace {

using ActivateAudioInterfaceAsyncFn = HRESULT(WINAPI*)(
    LPCWSTR deviceInterfacePath,
    REFIID riid,
    PROPVARIANT* activationParams,
    IActivateAudioInterfaceCompletionHandler* completionHandler,
    IActivateAudioInterfaceAsyncOperation** activationOperation);

ActivateAudioInterfaceAsyncFn resolve_activate_async() noexcept {
    HMODULE mod = GetModuleHandleW(L"Mmdevapi.dll");
    if (mod == nullptr) {
        mod = LoadLibraryW(L"Mmdevapi.dll");
    }
    if (mod == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<ActivateAudioInterfaceAsyncFn>(
        GetProcAddress(mod, "ActivateAudioInterfaceAsync"));
}

#if ATRISWAVE_HAS_PROCESS_LOOPBACK_SDK
class LoopbackCompletionHandler final
    : public IActivateAudioInterfaceCompletionHandler
    , public IAgileObject {
public:
    LoopbackCompletionHandler() noexcept
        : done_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~LoopbackCompletionHandler() noexcept {
        if (done_event_ != nullptr) {
            CloseHandle(done_event_);
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* operation) override {
        if (operation != nullptr) {
            HRESULT activate_hr = E_FAIL;
            ComPtr<IUnknown> unk;
            if (SUCCEEDED(operation->GetActivateResult(&activate_hr, unk.put())) &&
                SUCCEEDED(activate_hr) && unk) {
                result_hr_ = unk.as(audio_client_);
            } else {
                result_hr_ = FAILED(activate_hr) ? activate_hr : E_FAIL;
            }
        }
        if (done_event_ != nullptr) {
            SetEvent(done_event_);
        }
        return S_OK;
    }

    [[nodiscard]] bool wait(DWORD timeout_ms = 3000) const noexcept {
        if (done_event_ == nullptr) {
            return false;
        }
        return WaitForSingleObject(done_event_, timeout_ms) == WAIT_OBJECT_0;
    }

    [[nodiscard]] HRESULT result_hr() const noexcept { return result_hr_; }
    [[nodiscard]] ComPtr<IAudioClient> take_client() noexcept { return std::move(audio_client_); }

private:
    std::atomic<ULONG> ref_count_{1};
    HANDLE done_event_{nullptr};
    HRESULT result_hr_{E_FAIL};
    ComPtr<IAudioClient> audio_client_;
};
#endif

} // namespace

ProcessLoopbackCapture::~ProcessLoopbackCapture() {
    stop();
}

bool ProcessLoopbackCapture::is_supported() noexcept {
#if ATRISWAVE_HAS_PROCESS_LOOPBACK_SDK
    return resolve_activate_async() != nullptr;
#else
    return false;
#endif
}

bool ProcessLoopbackCapture::start(uint32_t pid, bool include_tree, AudioCallback callback) {
    stop();

#if !ATRISWAVE_HAS_PROCESS_LOOPBACK_SDK
    (void)pid;
    (void)include_tree;
    (void)callback;
    return false;
#else
    if (pid == 0) {
        return false;
    }

    auto activate_fn = resolve_activate_async();
    if (activate_fn == nullptr) {
        return false;
    }

    ScopedComInit com;
    if (!com.ok()) {
        return false;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = include_tree
        ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
        : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activate_props{};
    activate_props.vt = VT_BLOB;
    activate_props.blob.cbSize = sizeof(params);
    activate_props.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* raw_handler = new LoopbackCompletionHandler();
    ComPtr<IActivateAudioInterfaceCompletionHandler> handler_guard;
    handler_guard.attach(raw_handler);

    ComPtr<IActivateAudioInterfaceAsyncOperation> async_op;
    const HRESULT hr = activate_fn(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activate_props,
        raw_handler,
        async_op.put());

    if (FAILED(hr) || !raw_handler->wait(3000) || FAILED(raw_handler->result_hr())) {
        return false;
    }

    ComPtr<IAudioClient> client = raw_handler->take_client();
    if (!client) {
        return false;
    }

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 48000;
    wfx.wBitsPerSample = 32;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    const HRESULT init_hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        100000, // 10ms
        0,
        &wfx,
        nullptr);

    if (FAILED(init_hr)) {
        return false;
    }

    HANDLE audio_evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE stop_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (audio_evt == nullptr || stop_evt == nullptr) {
        if (audio_evt != nullptr) CloseHandle(audio_evt);
        if (stop_evt != nullptr) CloseHandle(stop_evt);
        return false;
    }

    if (FAILED(client->SetEventHandle(audio_evt))) {
        CloseHandle(audio_evt);
        CloseHandle(stop_evt);
        return false;
    }

    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), capture.put_void())) || !capture) {
        CloseHandle(audio_evt);
        CloseHandle(stop_evt);
        return false;
    }

    UINT32 buf_frames = 480;
    client->GetBufferSize(&buf_frames);

    if (FAILED(client->Start())) {
        CloseHandle(audio_evt);
        CloseHandle(stop_evt);
        return false;
    }

    auto* impl = new Impl();
    impl->client = std::move(client);
    impl->capture = std::move(capture);
    impl->sample_rate = 48000;
    impl->channels = 2;
    impl->buffer_frames = buf_frames;

    impl_ = impl;
    target_pid_ = pid;
    callback_ = std::move(callback);
    audio_event_ = audio_evt;
    stop_event_ = stop_evt;
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&ProcessLoopbackCapture::capture_loop, this);
    return true;
#endif
}

void ProcessLoopbackCapture::stop() {
    running_.store(false, std::memory_order_release);
    if (stop_event_ != nullptr) {
        SetEvent(static_cast<HANDLE>(stop_event_));
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (impl_ != nullptr && impl_->client) {
        impl_->client->Stop();
    }
    if (audio_event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(audio_event_));
        audio_event_ = nullptr;
    }
    if (stop_event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
    target_pid_ = 0;
}

void ProcessLoopbackCapture::capture_loop() {
    ScopedComInit com;
    MmcssGuard mmcss(L"Pro Audio");
    DenormalGuard denormal;

    const size_t max_frames = std::max<size_t>(impl_->buffer_frames, 4096);
    std::vector<float> scratch(max_frames * impl_->channels, 0.0f);
    HANDLE handles[2] = {
        static_cast<HANDLE>(stop_event_),
        static_cast<HANDLE>(audio_event_)
    };

    while (running_.load(std::memory_order_acquire)) {
        const DWORD wait_res = WaitForMultipleObjects(2, handles, FALSE, 500);
        if (wait_res == WAIT_OBJECT_0) {
            break;
        }
        if (wait_res != WAIT_OBJECT_0 + 1) {
            continue;
        }

        UINT32 packet_frames = 0;
        HRESULT hr = impl_->capture->GetNextPacketSize(&packet_frames);
        while (SUCCEEDED(hr) && packet_frames > 0) {
            BYTE* data = nullptr;
            UINT32 frames_available = 0;
            DWORD flags = 0;
            hr = impl_->capture->GetBuffer(&data, &frames_available, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                break;
            }

            const size_t safe_frames = std::min<size_t>(frames_available, max_frames);
            const size_t sample_count = safe_frames * impl_->channels;

            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                std::fill_n(scratch.data(), sample_count, 0.0f);
            } else {
                std::memcpy(scratch.data(), data, sample_count * sizeof(float));
            }

            impl_->capture->ReleaseBuffer(frames_available);

            if (callback_ && safe_frames > 0) {
                callback_(scratch.data(), static_cast<uint32_t>(safe_frames), impl_->channels);
            }

            hr = impl_->capture->GetNextPacketSize(&packet_frames);
        }

        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            break;
        }
    }

    running_.store(false, std::memory_order_release);
}

} // namespace atriswave::audio
