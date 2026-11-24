#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace atriswave::audio {

/// Per-process loopback capture using Windows 10/11 `ActivateAudioInterfaceAsync`
/// with `AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`.
/// Probes `ActivateAudioInterfaceAsync` dynamically at runtime (no build-number hard gate).
class ProcessLoopbackCapture final {
public:
    using AudioCallback = std::function<void(const float* data, uint32_t frames, uint32_t channels)>;

    ProcessLoopbackCapture() noexcept = default;
    ~ProcessLoopbackCapture();

    ProcessLoopbackCapture(const ProcessLoopbackCapture&) = delete;
    ProcessLoopbackCapture& operator=(const ProcessLoopbackCapture&) = delete;
    ProcessLoopbackCapture(ProcessLoopbackCapture&&) = delete;
    ProcessLoopbackCapture& operator=(ProcessLoopbackCapture&&) = delete;

    /// Runtime-probes whether `ActivateAudioInterfaceAsync` is available on this system.
    [[nodiscard]] static bool is_supported() noexcept;

    /// Starts per-process loopback capture for `pid` (48kHz stereo float32).
    /// Returns false gracefully if unsupported or blocked by OS/DRM.
    [[nodiscard]] bool start(uint32_t pid, bool include_tree, AudioCallback callback);

    void stop();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t target_pid() const noexcept { return target_pid_; }

private:
    void capture_loop();

    struct Impl;
    Impl* impl_{nullptr};

    uint32_t target_pid_{0};
    AudioCallback callback_;
    void* audio_event_{nullptr};
    void* stop_event_{nullptr};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace atriswave::audio
