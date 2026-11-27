#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace atriswave::audio {

enum class StreamDirection {
    Capture,
    Render,
    Loopback
};

/// Real-time event-driven WASAPI shared-mode stream (`IAudioClient3::InitializeSharedAudioStream`
/// with automatic fallback to `IAudioClient::Initialize`).
class WasapiStream final {
public:
    /// For Capture: `data` contains `frames * channels` input float32 samples.
    /// For Render: callback must fill `data` with `frames * channels` output float32 samples.
    using AudioCallback = std::function<void(float* data, uint32_t frames, uint32_t channels)>;

    WasapiStream() noexcept = default;
    ~WasapiStream();

    WasapiStream(const WasapiStream&) = delete;
    WasapiStream& operator=(const WasapiStream&) = delete;
    WasapiStream(WasapiStream&&) = delete;
    WasapiStream& operator=(WasapiStream&&) = delete;

    /// Opens the specified WASAPI endpoint (empty `device_id` selects the default multimedia device).
    [[nodiscard]] bool open(const std::string& device_id,
                            StreamDirection direction,
                            uint32_t preferred_sample_rate = 48000,
                            uint32_t preferred_channels = 2);

    /// Starts the event-driven audio thread with MMCSS "Pro Audio" and FTZ/DAZ enabled.
    [[nodiscard]] bool start(AudioCallback callback);

    /// Stops the audio thread and releases stream resources.
    void stop();
    void close();

    [[nodiscard]] uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] uint32_t buffer_frames() const noexcept { return buffer_frames_; }
    [[nodiscard]] bool is_open() const noexcept { return audio_client_ != nullptr; }
    [[nodiscard]] bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_device_invalidated() const noexcept {
        return device_invalidated_.load(std::memory_order_acquire);
    }

private:
    enum class SampleFormat {
        Float32,
        Pcm16,
        Pcm24,
        Pcm32
    };

    void audio_thread_loop();

    struct Impl;
    Impl* audio_client_{nullptr};

    StreamDirection direction_{StreamDirection::Capture};
    SampleFormat sample_format_{SampleFormat::Float32};
    uint32_t sample_rate_{48000};
    uint32_t channels_{2};
    uint32_t buffer_frames_{128};
    uint32_t bits_per_sample_{32};

    AudioCallback callback_;
    void* audio_event_{nullptr};
    void* stop_event_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<bool> device_invalidated_{false};
    std::thread worker_;
};

} // namespace atriswave::audio
