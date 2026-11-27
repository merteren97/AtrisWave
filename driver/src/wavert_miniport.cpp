#include "driver_internal.h"
#include <stdunk.h>

namespace {

static constexpr uint32_t kTickPeriodMs = 10u;
static constexpr uint32_t kDefaultFramesPerTick = 480u; /* 10ms @ 48 kHz */
static constexpr uint32_t kMaxScratchFrames = 2048u;

template <typename T, typename... Args>
static T* AtrisWaveAllocKernelObject(Args&&... args)
{
    void* mem = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(T), ATRISWAVE_POOL_TAG);
    if (mem == nullptr) {
        return nullptr;
    }
    return new (mem) T(static_cast<Args&&>(args)...);
}

#define ATRISWAVE_KERNEL_ALLOC_OPS()                                         \
    static void* operator new(size_t, void* p) noexcept { return p; }        \
    static void operator delete(void* p) noexcept {                          \
        if (p != nullptr) { ExFreePoolWithTag(p, ATRISWAVE_POOL_TAG); }      \
    }                                                                        \
    static void operator delete(void* p, size_t) noexcept {                  \
        if (p != nullptr) { ExFreePoolWithTag(p, ATRISWAVE_POOL_TAG); }      \
    }

/* ============================================================================
 * Shared Audio Data Ranges (48 kHz Stereo 32-bit IEEE Float & 16-bit PCM)
 * ============================================================================ */
static KSDATARANGE_AUDIO kPinDataRangesStream[] = {
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        2,      /* MaximumChannels */
        32,     /* MinimumBitsPerSample */
        32,     /* MaximumBitsPerSample */
        48000,  /* MinimumSampleFrequency */
        48000   /* MaximumSampleFrequency */
    },
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        2,      /* MaximumChannels */
        16,     /* MinimumBitsPerSample */
        16,     /* MaximumBitsPerSample */
        48000,  /* MinimumSampleFrequency */
        48000   /* MaximumSampleFrequency */
    }
};

static PKSDATARANGE kPinDataRangePointersStream[] = {
    reinterpret_cast<PKSDATARANGE>(&kPinDataRangesStream[0]),
    reinterpret_cast<PKSDATARANGE>(&kPinDataRangesStream[1])
};

static KSDATARANGE kPinDataRangesBridge[] = {
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static PKSDATARANGE kPinDataRangePointersBridge[] = {
    &kPinDataRangesBridge[0]
};

static PCCONNECTION_DESCRIPTOR kRenderWaveConnections[] = {
    { PCFILTER_NODE, 0, PCFILTER_NODE, 1 }
};

static PCCONNECTION_DESCRIPTOR kCaptureWaveConnections[] = {
    { PCFILTER_NODE, 1, PCFILTER_NODE, 0 }
};

static PCCONNECTION_DESCRIPTOR kTopoConnections[] = {
    { PCFILTER_NODE, 0, PCFILTER_NODE, 1 }
};

static GUID kRenderCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_RENDER),
    STATICGUIDOF(KSCATEGORY_REALTIME)
};

static GUID kCaptureCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_CAPTURE),
    STATICGUIDOF(KSCATEGORY_REALTIME)
};

static GUID kTopoCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_TOPOLOGY)
};

static PCPIN_DESCRIPTOR kRenderWavePins[] = {
    {
        1, 1, 0,
        nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(kPinDataRangePointersStream),
            kPinDataRangePointersStream,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    },
    {
        0, 0, 0,
        nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(kPinDataRangePointersBridge),
            kPinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    }
};

static PCPIN_DESCRIPTOR kCaptureWavePins[] = {
    {
        1, 1, 0,
        nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(kPinDataRangePointersStream),
            kPinDataRangePointersStream,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    },
    {
        0, 0, 0,
        nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(kPinDataRangePointersBridge),
            kPinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    }
};

static PCFILTER_DESCRIPTOR kRenderWaveFilterDescriptor = {
    0,
    nullptr,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(kRenderWavePins),
    kRenderWavePins,
    0,
    0,
    nullptr,
    SIZEOF_ARRAY(kRenderWaveConnections),
    kRenderWaveConnections,
    SIZEOF_ARRAY(kRenderCategories),
    kRenderCategories
};

static PCFILTER_DESCRIPTOR kCaptureWaveFilterDescriptor = {
    0,
    nullptr,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(kCaptureWavePins),
    kCaptureWavePins,
    0,
    0,
    nullptr,
    SIZEOF_ARRAY(kCaptureWaveConnections),
    kCaptureWaveConnections,
    SIZEOF_ARRAY(kCaptureCategories),
    kCaptureCategories
};

/* ============================================================================
 * WaveRT Stream Implementation (IMiniportWaveRTStreamNotification)
 * ============================================================================ */
struct NotifyEventEntry {
    LIST_ENTRY link;
    PKEVENT event;
};

class CAtrisWaveStream final
    : public IMiniportWaveRTStreamNotification
    , public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    ATRISWAVE_KERNEL_ALLOC_OPS();

    CAtrisWaveStream(PUNKNOWN outer)
        : CUnknown(outer)
    {
        InitializeListHead(&notify_events_);
        KeInitializeSpinLock(&notify_lock_);
        KeInitializeTimer(&timer_);
        KeInitializeDpc(&dpc_, &CAtrisWaveStream::TimerDpcRoutine, this);
    }

    ~CAtrisWaveStream()
    {
        StopTimerAndDpc();
        ClearNotifyEvents();
        if (scratch_pcm_ != nullptr) {
            ExFreePoolWithTag(scratch_pcm_, ATRISWAVE_POOL_TAG);
            scratch_pcm_ = nullptr;
        }
        if (port_stream_ != nullptr) {
            port_stream_->Release();
            port_stream_ = nullptr;
        }
    }

    NTSTATUS Init(
        PATRISWAVE_ADAPTER_CONTEXT ctx,
        uint32_t endpoint_id,
        bool is_capture,
        PPORTWAVERTSTREAM port_stream,
        PKSDATAFORMAT format)
    {
        ctx_ = ctx;
        endpoint_id_ = endpoint_id;
        is_capture_ = is_capture;
        port_stream_ = port_stream;
        if (port_stream_ != nullptr) {
            port_stream_->AddRef();
        }

        const NTSTATUS fmt_st = SetFormat(format);
        if (!NT_SUCCESS(fmt_st)) {
            return fmt_st;
        }

        scratch_pcm_ = static_cast<float*>(ExAllocatePool2(
            POOL_FLAG_NON_PAGED,
            static_cast<size_t>(kMaxScratchFrames) * 2u * sizeof(float),
            ATRISWAVE_POOL_TAG));
        if (scratch_pcm_ == nullptr) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        return STATUS_SUCCESS;
    }

    /* IMiniportWaveRTStream */
    STDMETHODIMP AllocateAudioBuffer(
        _In_ ULONG requested_size,
        _Out_ PMDL* audio_buffer_mdl,
        _Out_ ULONG* actual_size,
        _Out_ ULONG* offset_from_first_page,
        _Out_ MEMORY_CACHING_TYPE* cache_type) override
    {
        if (port_stream_ == nullptr || audio_buffer_mdl == nullptr || actual_size == nullptr ||
            offset_from_first_page == nullptr || cache_type == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }

        const ULONG min_bytes = kDefaultFramesPerTick * frame_bytes_ * 4u;
        ULONG alloc_bytes = (requested_size < min_bytes) ? min_bytes : requested_size;
        const ULONG rem = alloc_bytes % frame_bytes_;
        if (rem != 0u) {
            alloc_bytes += (frame_bytes_ - rem);
        }

        PHYSICAL_ADDRESS low_addr{};
        PHYSICAL_ADDRESS high_addr{};
        high_addr.QuadPart = -1;
        PHYSICAL_ADDRESS boundary{};

        PMDL mdl = port_stream_->AllocatePagesForMdl(high_addr, alloc_bytes);
        if (mdl == nullptr) {
            mdl = port_stream_->AllocateContiguousPagesForMdl(low_addr, high_addr, alloc_bytes);
        }
        if (mdl == nullptr) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        PVOID mapped = port_stream_->MapAllocatedPages(mdl, MmCached);
        if (mapped == nullptr) {
            port_stream_->FreePagesFromMdl(mdl);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(mapped, alloc_bytes);

        dma_mdl_ = mdl;
        dma_buffer_ = static_cast<uint8_t*>(mapped);
        dma_bytes_ = alloc_bytes;
        byte_pos_ = 0u;
        last_notify_idx_ = 0u;

        *audio_buffer_mdl = mdl;
        *actual_size = alloc_bytes;
        *offset_from_first_page = 0u;
        *cache_type = MmCached;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(void) FreeAudioBuffer(
        _In_opt_ PMDL audio_buffer_mdl,
        _In_ ULONG buffer_size) override
    {
        UNREFERENCED_PARAMETER(buffer_size);
        StopTimerAndDpc();

        if (dma_buffer_ != nullptr && dma_mdl_ != nullptr && port_stream_ != nullptr) {
            port_stream_->UnmapAllocatedPages(dma_buffer_, dma_mdl_);
            port_stream_->FreePagesFromMdl(dma_mdl_);
        } else if (audio_buffer_mdl != nullptr && port_stream_ != nullptr) {
            port_stream_->FreePagesFromMdl(audio_buffer_mdl);
        }

        dma_buffer_ = nullptr;
        dma_mdl_ = nullptr;
        dma_bytes_ = 0u;
    }

    STDMETHODIMP_(void) GetHWLatency(_Out_ KSRTAUDIO_HWLATENCY* hw_latency) override
    {
        if (hw_latency != nullptr) {
            hw_latency->FifoSize = kDefaultFramesPerTick * frame_bytes_;
            hw_latency->ChipsetDelay = 0;
            hw_latency->CodecDelay = 0;
        }
    }

    STDMETHODIMP GetPositionRegister(_Out_ KSRTAUDIO_HWREGISTER* reg) override
    {
        UNREFERENCED_PARAMETER(reg);
        return STATUS_NOT_SUPPORTED;
    }

    STDMETHODIMP GetClockRegister(_Out_ KSRTAUDIO_HWREGISTER* reg) override
    {
        UNREFERENCED_PARAMETER(reg);
        return STATUS_NOT_SUPPORTED;
    }

    STDMETHODIMP GetPosition(_Out_ PKSAUDIO_POSITION position) override
    {
        if (position == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        const ULONG pos = static_cast<ULONG>(InterlockedCompareExchange(&byte_pos_, 0, 0));
        position->PlayOffset = pos;
        position->WriteOffset = pos;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP SetFormat(_In_ PKSDATAFORMAT format) override
    {
        if (format == nullptr || format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
            return STATUS_INVALID_PARAMETER;
        }
        const auto* wf = reinterpret_cast<const KSDATAFORMAT_WAVEFORMATEX*>(format);
        const WAVEFORMATEX& wfx = wf->WaveFormatEx;

        if (wfx.nChannels != 2 || wfx.nSamplesPerSec != 48000) {
            return STATUS_NO_MATCH;
        }

        bool is_float = false;
        if (wfx.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx.wBitsPerSample == 32) {
            is_float = true;
        } else if (wfx.wFormatTag == WAVE_FORMAT_PCM && wfx.wBitsPerSample == 16) {
            is_float = false;
        } else if (wfx.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                   format->FormatSize >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE)) {
            const auto* wfxe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&wf->WaveFormatEx);
            if (IsEqualGUIDAligned(wfxe->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && wfx.wBitsPerSample == 32) {
                is_float = true;
            } else if (IsEqualGUIDAligned(wfxe->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && wfx.wBitsPerSample == 16) {
                is_float = false;
            } else {
                return STATUS_NO_MATCH;
            }
        } else {
            return STATUS_NO_MATCH;
        }

        is_float32_ = is_float;
        sample_rate_ = wfx.nSamplesPerSec;
        channels_ = wfx.nChannels;
        frame_bytes_ = channels_ * (is_float32_ ? 4u : 2u);
        return STATUS_SUCCESS;
    }

    STDMETHODIMP SetState(_In_ KSSTATE state) override
    {
        if (state == state_) {
            return STATUS_SUCCESS;
        }

        if (state == KSSTATE_RUN) {
            if (ctx_ != nullptr && endpoint_id_ < ATRISWAVE_VAD_ENDPOINT_COUNT) {
                KIRQL old_irql;
                KeAcquireSpinLock(&ctx_->state_lock, &old_irql);
                ctx_->endpoints[endpoint_id_].active = 1u;
                ctx_->endpoints[endpoint_id_].ring_header.state = ATRISWAVE_VAD_RING_STATE_RUNNING;
                KeReleaseSpinLock(&ctx_->state_lock, old_irql);
            }
            InterlockedExchange(&running_, 1);
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(kTickPeriodMs) * 10000LL;
            KeSetTimerEx(&timer_, due, kTickPeriodMs, &dpc_);
        } else if (state == KSSTATE_PAUSE || state == KSSTATE_STOP) {
            StopTimerAndDpc();
            if (state == KSSTATE_STOP) {
                InterlockedExchange(&byte_pos_, 0);
                last_notify_idx_ = 0u;
                if (dma_buffer_ != nullptr && dma_bytes_ > 0u) {
                    RtlZeroMemory(dma_buffer_, dma_bytes_);
                }
                if (ctx_ != nullptr && endpoint_id_ < ATRISWAVE_VAD_ENDPOINT_COUNT) {
                    KIRQL old_irql;
                    KeAcquireSpinLock(&ctx_->state_lock, &old_irql);
                    ctx_->endpoints[endpoint_id_].active = 0u;
                    ctx_->endpoints[endpoint_id_].ring_header.state = ATRISWAVE_VAD_RING_STATE_STOPPED;
                    KeReleaseSpinLock(&ctx_->state_lock, old_irql);
                }
            }
        }

        state_ = state;
        return STATUS_SUCCESS;
    }

    /* IMiniportWaveRTStreamNotification */
    STDMETHODIMP AllocateBufferWithNotification(
        _In_ ULONG notification_count,
        _In_ ULONG requested_size,
        _Out_ PMDL* audio_buffer_mdl,
        _Out_ ULONG* actual_size,
        _Out_ ULONG* offset_from_first_page,
        _Out_ MEMORY_CACHING_TYPE* cache_type) override
    {
        notify_count_ = (notification_count == 0u) ? 2u : notification_count;
        return AllocateAudioBuffer(requested_size, audio_buffer_mdl, actual_size, offset_from_first_page, cache_type);
    }

    STDMETHODIMP_(void) FreeBufferWithNotification(
        _In_opt_ PMDL audio_buffer_mdl,
        _In_ ULONG buffer_size) override
    {
        ClearNotifyEvents();
        FreeAudioBuffer(audio_buffer_mdl, buffer_size);
    }

    STDMETHODIMP RegisterNotificationEvent(_In_ PKEVENT notification_event) override
    {
        if (notification_event == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        auto* entry = static_cast<NotifyEventEntry*>(
            ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(NotifyEventEntry), ATRISWAVE_POOL_TAG));
        if (entry == nullptr) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        entry->event = notification_event;

        KIRQL old_irql;
        KeAcquireSpinLock(&notify_lock_, &old_irql);
        InsertTailList(&notify_events_, &entry->link);
        KeReleaseSpinLock(&notify_lock_, old_irql);
        return STATUS_SUCCESS;
    }

    STDMETHODIMP UnregisterNotificationEvent(_In_ PKEVENT notification_event) override
    {
        if (notification_event == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        KIRQL old_irql;
        KeAcquireSpinLock(&notify_lock_, &old_irql);
        for (PLIST_ENTRY cur = notify_events_.Flink; cur != &notify_events_; cur = cur->Flink) {
            auto* entry = CONTAINING_RECORD(cur, NotifyEventEntry, link);
            if (entry->event == notification_event) {
                RemoveEntryList(cur);
                KeReleaseSpinLock(&notify_lock_, old_irql);
                ExFreePoolWithTag(entry, ATRISWAVE_POOL_TAG);
                return STATUS_SUCCESS;
            }
        }
        KeReleaseSpinLock(&notify_lock_, old_irql);
        return STATUS_NOT_FOUND;
    }

private:
    void StopTimerAndDpc()
    {
        InterlockedExchange(&running_, 0);
        KeCancelTimer(&timer_);
        KeFlushQueuedDpcs();
    }

    void ClearNotifyEvents()
    {
        KIRQL old_irql;
        KeAcquireSpinLock(&notify_lock_, &old_irql);
        while (!IsListEmpty(&notify_events_)) {
            PLIST_ENTRY link = RemoveHeadList(&notify_events_);
            auto* entry = CONTAINING_RECORD(link, NotifyEventEntry, link);
            ExFreePoolWithTag(entry, ATRISWAVE_POOL_TAG);
        }
        KeReleaseSpinLock(&notify_lock_, old_irql);
    }

    void SignalNotifyEventsDpc()
    {
        KeAcquireSpinLockAtDpcLevel(&notify_lock_);
        for (PLIST_ENTRY cur = notify_events_.Flink; cur != &notify_events_; cur = cur->Flink) {
            auto* entry = CONTAINING_RECORD(cur, NotifyEventEntry, link);
            if (entry->event != nullptr) {
                KeSetEvent(entry->event, IO_SOUND_INCREMENT, FALSE);
            }
        }
        KeReleaseSpinLockFromDpcLevel(&notify_lock_);
    }

    static void TimerDpcRoutine(PKDPC, PVOID deferred_context, PVOID, PVOID)
    {
        auto* self = static_cast<CAtrisWaveStream*>(deferred_context);
        if (self != nullptr) {
            self->OnTimerTickDpc();
        }
    }

    void OnTimerTickDpc()
    {
        if (InterlockedCompareExchange(&running_, 0, 0) == 0 ||
            dma_buffer_ == nullptr || dma_bytes_ == 0u || scratch_pcm_ == nullptr || ctx_ == nullptr) {
            return;
        }

        const uint32_t frames = kDefaultFramesPerTick;
        const ULONG chunk_bytes = frames * frame_bytes_;
        const ULONG cur_pos = static_cast<ULONG>(InterlockedCompareExchange(&byte_pos_, 0, 0));

        if (!is_capture_) {
            /* RENDER: copy from WaveRT DMA cyclic buffer -> kernel ring buffer (for Engine) */
            CopyFromDmaToFloat(cur_pos, frames, scratch_pcm_);
            (void)AtrisWaveRtPushFramesDispatch(ctx_, endpoint_id_, scratch_pcm_, frames);
        } else {
            /* CAPTURE: pull from kernel ring buffer (from Engine B1/B2) -> WaveRT DMA cyclic buffer */
            const uint32_t got = AtrisWaveRtPullFramesDispatch(ctx_, endpoint_id_, scratch_pcm_, frames);
            if (got < frames) {
                RtlZeroMemory(
                    scratch_pcm_ + static_cast<size_t>(got) * channels_,
                    static_cast<size_t>(frames - got) * channels_ * sizeof(float));
            }
            CopyFromFloatToDma(cur_pos, frames, scratch_pcm_);
        }

        const ULONG next_pos = (cur_pos + chunk_bytes) % dma_bytes_;
        InterlockedExchange(&byte_pos_, static_cast<LONG>(next_pos));

        if (notify_count_ > 0u) {
            const ULONG region_bytes = dma_bytes_ / notify_count_;
            if (region_bytes > 0u) {
                const ULONG new_idx = next_pos / region_bytes;
                if (new_idx != last_notify_idx_) {
                    last_notify_idx_ = new_idx;
                    SignalNotifyEventsDpc();
                }
            }
        }
    }

    void CopyFromDmaToFloat(ULONG byte_offset, uint32_t frames, float* dst) const
    {
        const ULONG total_bytes = frames * frame_bytes_;
        const ULONG first_bytes = (byte_offset + total_bytes <= dma_bytes_)
            ? total_bytes
            : (dma_bytes_ - byte_offset);
        const ULONG second_bytes = total_bytes - first_bytes;

        if (is_float32_) {
            RtlCopyMemory(dst, dma_buffer_ + byte_offset, first_bytes);
            if (second_bytes > 0u) {
                RtlCopyMemory(reinterpret_cast<uint8_t*>(dst) + first_bytes, dma_buffer_, second_bytes);
            }
            return;
        }

        /* 16-bit PCM -> float32 */
        const size_t first_samples = first_bytes / sizeof(int16_t);
        const auto* src1 = reinterpret_cast<const int16_t*>(dma_buffer_ + byte_offset);
        for (size_t i = 0; i < first_samples; ++i) {
            dst[i] = static_cast<float>(src1[i]) * (1.0f / 32768.0f);
        }
        if (second_bytes > 0u) {
            const size_t second_samples = second_bytes / sizeof(int16_t);
            const auto* src2 = reinterpret_cast<const int16_t*>(dma_buffer_);
            for (size_t i = 0; i < second_samples; ++i) {
                dst[first_samples + i] = static_cast<float>(src2[i]) * (1.0f / 32768.0f);
            }
        }
    }

    void CopyFromFloatToDma(ULONG byte_offset, uint32_t frames, const float* src)
    {
        const ULONG total_bytes = frames * frame_bytes_;
        const ULONG first_bytes = (byte_offset + total_bytes <= dma_bytes_)
            ? total_bytes
            : (dma_bytes_ - byte_offset);
        const ULONG second_bytes = total_bytes - first_bytes;

        if (is_float32_) {
            RtlCopyMemory(dma_buffer_ + byte_offset, src, first_bytes);
            if (second_bytes > 0u) {
                RtlCopyMemory(dma_buffer_, reinterpret_cast<const uint8_t*>(src) + first_bytes, second_bytes);
            }
            return;
        }

        /* float32 -> 16-bit PCM */
        const size_t first_samples = first_bytes / sizeof(int16_t);
        auto* dst1 = reinterpret_cast<int16_t*>(dma_buffer_ + byte_offset);
        for (size_t i = 0; i < first_samples; ++i) {
            float v = src[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            dst1[i] = static_cast<int16_t>(v * 32767.0f);
        }
        if (second_bytes > 0u) {
            const size_t second_samples = second_bytes / sizeof(int16_t);
            auto* dst2 = reinterpret_cast<int16_t*>(dma_buffer_);
            for (size_t i = 0; i < second_samples; ++i) {
                float v = src[first_samples + i];
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                dst2[i] = static_cast<int16_t>(v * 32767.0f);
            }
        }
    }

    PATRISWAVE_ADAPTER_CONTEXT ctx_{nullptr};
    uint32_t endpoint_id_{0u};
    bool is_capture_{false};
    bool is_float32_{true};
    uint32_t sample_rate_{48000u};
    uint32_t channels_{2u};
    ULONG frame_bytes_{8u};

    PPORTWAVERTSTREAM port_stream_{nullptr};
    PMDL dma_mdl_{nullptr};
    uint8_t* dma_buffer_{nullptr};
    ULONG dma_bytes_{0u};
    float* scratch_pcm_{nullptr};

    KSSTATE state_{KSSTATE_STOP};
    LONG running_{0};
    LONG byte_pos_{0};
    ULONG notify_count_{2u};
    ULONG last_notify_idx_{0u};

    KTIMER timer_{};
    KDPC dpc_{};
    KSPIN_LOCK notify_lock_{};
    LIST_ENTRY notify_events_{};
};

STDMETHODIMP CAtrisWaveStream::NonDelegatingQueryInterface(_In_ REFIID riid, _COM_Outptr_ PVOID* out)
{
    if (out == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    if (IsEqualGUIDAligned(riid, IID_IUnknown)) {
        *out = static_cast<PUNKNOWN>(static_cast<PMINIPORTWAVERTSTREAM>(this));
    } else if (IsEqualGUIDAligned(riid, IID_IMiniportWaveRTStream)) {
        *out = static_cast<PMINIPORTWAVERTSTREAM>(this);
    } else if (IsEqualGUIDAligned(riid, IID_IMiniportWaveRTStreamNotification)) {
        *out = static_cast<PMINIPORTWAVERTSTREAMNOTIFICATION>(this);
    } else {
        *out = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    reinterpret_cast<PUNKNOWN>(*out)->AddRef();
    return STATUS_SUCCESS;
}

/* ============================================================================
 * WaveRT Miniport (IMiniportWaveRT)
 * ============================================================================ */
class CAtrisWaveMiniportWaveRT final
    : public IMiniportWaveRT
    , public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    ATRISWAVE_KERNEL_ALLOC_OPS();

    CAtrisWaveMiniportWaveRT(PUNKNOWN outer, PATRISWAVE_ADAPTER_CONTEXT ctx, uint32_t endpoint_id)
        : CUnknown(outer), ctx_(ctx), endpoint_id_(endpoint_id)
    {
        uint32_t count = 0;
        const AtrisWaveEndpointDescriptor* descs = AtrisWaveGetEndpointDescriptors(&count);
        if (endpoint_id < count) {
            is_capture_ = (descs[endpoint_id].direction == ATRISWAVE_ENDPOINT_CAPTURE);
        }
    }

    ~CAtrisWaveMiniportWaveRT()
    {
        if (port_ != nullptr) {
            port_->Release();
            port_ = nullptr;
        }
    }

    STDMETHODIMP GetDescription(_Out_ PPCFILTER_DESCRIPTOR* description) override
    {
        if (description == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        *description = is_capture_ ? &kCaptureWaveFilterDescriptor : &kRenderWaveFilterDescriptor;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP DataRangeIntersection(
        _In_ ULONG pin_id,
        _In_ PKSDATARANGE client_range,
        _In_ PKSDATARANGE my_range,
        _In_ ULONG output_buffer_length,
        _Out_writes_bytes_to_opt_(output_buffer_length, *resultant_format_length) PVOID resultant_format,
        _Out_ PULONG resultant_format_length) override
    {
        UNREFERENCED_PARAMETER(pin_id);
        UNREFERENCED_PARAMETER(client_range);
        UNREFERENCED_PARAMETER(my_range);
        UNREFERENCED_PARAMETER(output_buffer_length);
        UNREFERENCED_PARAMETER(resultant_format);
        UNREFERENCED_PARAMETER(resultant_format_length);
        return STATUS_NOT_IMPLEMENTED;
    }

    STDMETHODIMP Init(
        _In_ PUNKNOWN unknown_adapter,
        _In_ PRESOURCELIST resource_list,
        _In_ PPORTWAVERT port) override
    {
        UNREFERENCED_PARAMETER(unknown_adapter);
        UNREFERENCED_PARAMETER(resource_list);
        if (port == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        port_ = port;
        port_->AddRef();
        return STATUS_SUCCESS;
    }

    STDMETHODIMP NewStream(
        _Out_ PMINIPORTWAVERTSTREAM* out_stream,
        _In_ PPORTWAVERTSTREAM port_stream,
        _In_ ULONG pin,
        _In_ BOOLEAN capture,
        _In_ PKSDATAFORMAT data_format) override
    {
        UNREFERENCED_PARAMETER(pin);
        if (out_stream == nullptr || port_stream == nullptr || data_format == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        *out_stream = nullptr;

        auto* stream = AtrisWaveAllocKernelObject<CAtrisWaveStream>(nullptr);
        if (stream == nullptr) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        stream->AddRef();

        const NTSTATUS st = stream->Init(ctx_, endpoint_id_, capture != FALSE, port_stream, data_format);
        if (!NT_SUCCESS(st)) {
            stream->Release();
            return st;
        }

        *out_stream = static_cast<PMINIPORTWAVERTSTREAM>(stream);
        return STATUS_SUCCESS;
    }

    STDMETHODIMP GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION device_description) override
    {
        if (device_description == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        RtlZeroMemory(device_description, sizeof(*device_description));
        device_description->Master = TRUE;
        device_description->ScatterGather = TRUE;
        device_description->Dma32BitAddresses = TRUE;
        device_description->Dma64BitAddresses = TRUE;
        device_description->InterfaceType = PCIBus;
        device_description->MaximumLength = 0x10000;
        return STATUS_SUCCESS;
    }

private:
    PATRISWAVE_ADAPTER_CONTEXT ctx_{nullptr};
    uint32_t endpoint_id_{0u};
    bool is_capture_{false};
    PPORTWAVERT port_{nullptr};
};

STDMETHODIMP CAtrisWaveMiniportWaveRT::NonDelegatingQueryInterface(_In_ REFIID riid, _COM_Outptr_ PVOID* out)
{
    if (out == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    if (IsEqualGUIDAligned(riid, IID_IUnknown)) {
        *out = static_cast<PUNKNOWN>(static_cast<PMINIPORTWAVERT>(this));
    } else if (IsEqualGUIDAligned(riid, IID_IMiniport)) {
        *out = static_cast<PMINIPORT>(this);
    } else if (IsEqualGUIDAligned(riid, IID_IMiniportWaveRT)) {
        *out = static_cast<PMINIPORTWAVERT>(this);
    } else {
        *out = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    reinterpret_cast<PUNKNOWN>(*out)->AddRef();
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Topology Miniport (IMiniportTopology) with KSJACK_DESCRIPTION & Presence Events
 * ============================================================================ */
class CAtrisWaveMiniportTopology final
    : public IMiniportTopology
    , public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    ATRISWAVE_KERNEL_ALLOC_OPS();

    CAtrisWaveMiniportTopology(PUNKNOWN outer, PATRISWAVE_ADAPTER_CONTEXT ctx, uint32_t endpoint_id)
        : CUnknown(outer), ctx_(ctx), endpoint_id_(endpoint_id)
    {
        uint32_t count = 0;
        const AtrisWaveEndpointDescriptor* descs = AtrisWaveGetEndpointDescriptors(&count);
        if (endpoint_id < count) {
            is_capture_ = (descs[endpoint_id].direction == ATRISWAVE_ENDPOINT_CAPTURE);
        }
        BuildDescriptors();
    }

    ~CAtrisWaveMiniportTopology()
    {
        if (ctx_ != nullptr && endpoint_id_ < ATRISWAVE_VAD_ENDPOINT_COUNT) {
            KIRQL old_irql;
            KeAcquireSpinLock(&ctx_->state_lock, &old_irql);
            if (ctx_->endpoints[endpoint_id_].topo_port_events == port_events_) {
                ctx_->endpoints[endpoint_id_].topo_port_events = nullptr;
            }
            KeReleaseSpinLock(&ctx_->state_lock, old_irql);
        }
        if (port_events_ != nullptr) {
            port_events_->Release();
            port_events_ = nullptr;
        }
        if (port_ != nullptr) {
            port_->Release();
            port_ = nullptr;
        }
    }

    STDMETHODIMP GetDescription(_Out_ PPCFILTER_DESCRIPTOR* description) override
    {
        if (description == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        *description = &filter_desc_;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP DataRangeIntersection(
        _In_ ULONG, _In_ PKSDATARANGE, _In_ PKSDATARANGE, _In_ ULONG, _Out_opt_ PVOID, _Out_ PULONG) override
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    STDMETHODIMP Init(
        _In_ PUNKNOWN unknown_adapter,
        _In_ PRESOURCELIST resource_list,
        _In_ PPORTTOPOLOGY port) override
    {
        UNREFERENCED_PARAMETER(unknown_adapter);
        UNREFERENCED_PARAMETER(resource_list);
        if (port == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        port_ = port;
        port_->AddRef();

        if (NT_SUCCESS(port_->QueryInterface(IID_IPortEvents, reinterpret_cast<PVOID*>(&port_events_))) &&
            port_events_ != nullptr && ctx_ != nullptr && endpoint_id_ < ATRISWAVE_VAD_ENDPOINT_COUNT) {
            KIRQL old_irql;
            KeAcquireSpinLock(&ctx_->state_lock, &old_irql);
            ctx_->endpoints[endpoint_id_].topo_port_events = port_events_;
            KeReleaseSpinLock(&ctx_->state_lock, old_irql);
        }

        return STATUS_SUCCESS;
    }

    static NTSTATUS PropertyHandlerJackDescription(_In_ PPCPROPERTY_REQUEST req)
    {
        auto* self = reinterpret_cast<CAtrisWaveMiniportTopology*>(req->MajorTarget);
        if (self == nullptr || self->ctx_ == nullptr || self->endpoint_id_ >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
            return STATUS_INVALID_PARAMETER;
        }

        if (req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
            if (req->ValueSize >= sizeof(ULONG) && req->Value != nullptr) {
                *static_cast<ULONG*>(req->Value) = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
                req->ValueSize = sizeof(ULONG);
                return STATUS_SUCCESS;
            }
            return STATUS_BUFFER_TOO_SMALL;
        }

        if (!(req->Verb & KSPROPERTY_TYPE_GET)) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }

        const ULONG needed = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION);
        if (req->ValueSize == 0) {
            req->ValueSize = needed;
            return STATUS_BUFFER_OVERFLOW;
        }
        if (req->ValueSize < needed || req->Value == nullptr) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        KIRQL old_irql;
        KeAcquireSpinLock(&self->ctx_->state_lock, &old_irql);
        const BOOL connected = (self->ctx_->endpoints[self->endpoint_id_].jack_connected != 0u) ? TRUE : FALSE;
        KeReleaseSpinLock(&self->ctx_->state_lock, old_irql);

        auto* multi = static_cast<PKSMULTIPLE_ITEM>(req->Value);
        multi->Size = needed;
        multi->Count = 1;

        auto* jack = reinterpret_cast<PKSJACK_DESCRIPTION>(multi + 1);
        RtlZeroMemory(jack, sizeof(*jack));
        jack->ChannelMapping = KSAUDIO_SPEAKER_STEREO;
        jack->Color = 0x0038BDF8u;
        jack->ConnectionType = eConnTypeOtherDigital;
        jack->GeoLocation = eGeoLocRear;
        jack->GenLocation = eGenLocPrimaryBox;
        jack->PortConnection = ePortConnIntegratedDevice;
        jack->IsConnected = connected;

        req->ValueSize = needed;
        return STATUS_SUCCESS;
    }

    static NTSTATUS PropertyHandlerJackDescription2(_In_ PPCPROPERTY_REQUEST req)
    {
        auto* self = reinterpret_cast<CAtrisWaveMiniportTopology*>(req->MajorTarget);
        if (self == nullptr || self->ctx_ == nullptr || self->endpoint_id_ >= ATRISWAVE_VAD_ENDPOINT_COUNT) {
            return STATUS_INVALID_PARAMETER;
        }

        if (req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
            if (req->ValueSize >= sizeof(ULONG) && req->Value != nullptr) {
                *static_cast<ULONG*>(req->Value) = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
                req->ValueSize = sizeof(ULONG);
                return STATUS_SUCCESS;
            }
            return STATUS_BUFFER_TOO_SMALL;
        }

        if (!(req->Verb & KSPROPERTY_TYPE_GET)) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }

        const ULONG needed = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION2);
        if (req->ValueSize == 0) {
            req->ValueSize = needed;
            return STATUS_BUFFER_OVERFLOW;
        }
        if (req->ValueSize < needed || req->Value == nullptr) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        auto* multi = static_cast<PKSMULTIPLE_ITEM>(req->Value);
        multi->Size = needed;
        multi->Count = 1;

        auto* jack2 = reinterpret_cast<PKSJACK_DESCRIPTION2>(multi + 1);
        RtlZeroMemory(jack2, sizeof(*jack2));
        jack2->DeviceStateInfo = 0u;
        jack2->JackCapabilities = JACKDESC2_PRESENCE_DETECT_CAPABILITY;

        req->ValueSize = needed;
        return STATUS_SUCCESS;
    }

    static NTSTATUS EventHandlerJackChange(_In_ PPCEVENT_REQUEST req)
    {
        auto* self = reinterpret_cast<CAtrisWaveMiniportTopology*>(req->MajorTarget);
        if (self == nullptr || self->port_events_ == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        if (req->Verb == PCEVENT_VERB_ADD) {
            self->port_events_->AddEventToEventList(req->EventEntry);
        }
        return STATUS_SUCCESS;
    }

private:
    void BuildDescriptors()
    {
        RtlZeroMemory(props_, sizeof(props_));
        props_[0].Set = &KSPROPSETID_Jack;
        props_[0].Id = KSPROPERTY_JACK_DESCRIPTION;
        props_[0].Flags = PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT;
        props_[0].Handler = &CAtrisWaveMiniportTopology::PropertyHandlerJackDescription;

        props_[1].Set = &KSPROPSETID_Jack;
        props_[1].Id = KSPROPERTY_JACK_DESCRIPTION2;
        props_[1].Flags = PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT;
        props_[1].Handler = &CAtrisWaveMiniportTopology::PropertyHandlerJackDescription2;

        RtlZeroMemory(&event_item_, sizeof(event_item_));
        event_item_.Set = &KSEVENTSETID_PinCapsChange;
        event_item_.Id = KSEVENT_PINCAPS_JACKINFOCHANGE;
        event_item_.Flags = PCEVENT_ITEM_FLAG_ENABLE;
        event_item_.Handler = &CAtrisWaveMiniportTopology::EventHandlerJackChange;

        RtlZeroMemory(&pin_automation_, sizeof(pin_automation_));
        pin_automation_.PropertyItemSize = sizeof(PCPROPERTY_ITEM);
        pin_automation_.PropertyCount = SIZEOF_ARRAY(props_);
        pin_automation_.Properties = props_;
        pin_automation_.EventItemSize = sizeof(PCEVENT_ITEM);
        pin_automation_.EventCount = 1;
        pin_automation_.Events = &event_item_;

        RtlZeroMemory(pins_, sizeof(pins_));
        /* Pin 0: Input bridge (for Capture: External Mic Jack; for Render: Internal Bridge from WaveRT) */
        pins_[0].AutomationTable = is_capture_ ? &pin_automation_ : nullptr;
        pins_[0].KsPinDescriptor.DataRangesCount = SIZEOF_ARRAY(kPinDataRangePointersBridge);
        pins_[0].KsPinDescriptor.DataRanges = kPinDataRangePointersBridge;
        pins_[0].KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_IN;
        pins_[0].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
        pins_[0].KsPinDescriptor.Category = is_capture_ ? &KSNODETYPE_MICROPHONE : &KSCATEGORY_AUDIO;

        /* Pin 1: Output bridge (for Render: External Speaker Jack; for Capture: Internal Bridge to WaveRT) */
        pins_[1].AutomationTable = is_capture_ ? nullptr : &pin_automation_;
        pins_[1].KsPinDescriptor.DataRangesCount = SIZEOF_ARRAY(kPinDataRangePointersBridge);
        pins_[1].KsPinDescriptor.DataRanges = kPinDataRangePointersBridge;
        pins_[1].KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;
        pins_[1].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
        pins_[1].KsPinDescriptor.Category = is_capture_ ? &KSCATEGORY_AUDIO : &KSNODETYPE_SPEAKER;

        RtlZeroMemory(&filter_desc_, sizeof(filter_desc_));
        filter_desc_.PinSize = sizeof(PCPIN_DESCRIPTOR);
        filter_desc_.PinCount = SIZEOF_ARRAY(pins_);
        filter_desc_.Pins = pins_;
        filter_desc_.ConnectionCount = SIZEOF_ARRAY(kTopoConnections);
        filter_desc_.Connections = kTopoConnections;
        filter_desc_.CategoryCount = SIZEOF_ARRAY(kTopoCategories);
        filter_desc_.Categories = kTopoCategories;
    }

    PATRISWAVE_ADAPTER_CONTEXT ctx_{nullptr};
    uint32_t endpoint_id_{0u};
    bool is_capture_{false};
    PPORTTOPOLOGY port_{nullptr};
    IPortEvents* port_events_{nullptr};

    PCPROPERTY_ITEM props_[2]{};
    PCEVENT_ITEM event_item_{};
    PCAUTOMATION_TABLE pin_automation_{};
    PCPIN_DESCRIPTOR pins_[2]{};
    PCFILTER_DESCRIPTOR filter_desc_{};
};

STDMETHODIMP CAtrisWaveMiniportTopology::NonDelegatingQueryInterface(_In_ REFIID riid, _COM_Outptr_ PVOID* out)
{
    if (out == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    if (IsEqualGUIDAligned(riid, IID_IUnknown)) {
        *out = static_cast<PUNKNOWN>(static_cast<PMINIPORTTOPOLOGY>(this));
    } else if (IsEqualGUIDAligned(riid, IID_IMiniport)) {
        *out = static_cast<PMINIPORT>(this);
    } else if (IsEqualGUIDAligned(riid, IID_IMiniportTopology)) {
        *out = static_cast<PMINIPORTTOPOLOGY>(this);
    } else {
        *out = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    reinterpret_cast<PUNKNOWN>(*out)->AddRef();
    return STATUS_SUCCESS;
}

} // namespace

NTSTATUS AtrisWaveCreateMiniportWaveRT(
    _Out_ PUNKNOWN* unknown,
    _In_ PATRISWAVE_ADAPTER_CONTEXT ctx,
    _In_ uint32_t endpoint_id)
{
    if (unknown == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    *unknown = nullptr;

    auto* mp = AtrisWaveAllocKernelObject<CAtrisWaveMiniportWaveRT>(nullptr, ctx, endpoint_id);
    if (mp == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    mp->AddRef();
    *unknown = static_cast<PUNKNOWN>(static_cast<PMINIPORTWAVERT>(mp));
    return STATUS_SUCCESS;
}

NTSTATUS AtrisWaveCreateMiniportTopology(
    _Out_ PUNKNOWN* unknown,
    _In_ PATRISWAVE_ADAPTER_CONTEXT ctx,
    _In_ uint32_t endpoint_id)
{
    if (unknown == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    *unknown = nullptr;

    auto* mp = AtrisWaveAllocKernelObject<CAtrisWaveMiniportTopology>(nullptr, ctx, endpoint_id);
    if (mp == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    mp->AddRef();
    *unknown = static_cast<PUNKNOWN>(static_cast<PMINIPORTTOPOLOGY>(mp));
    return STATUS_SUCCESS;
}
