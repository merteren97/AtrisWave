#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace atriswave::dsp {

/// Zero-external-dependency WAV decoder + 8-voice polyphonic Soundboard Engine.
/// WAV decoding and resampling run strictly on the control thread; RT audio thread
/// reads pre-decoded PCM buffers lock-free with zero heap allocations.
class SoundboardEngine final {
public:
    static constexpr std::size_t kMaxVoices = 8;
    static constexpr std::size_t kFadeOutFrames = 480; // 10ms @ 48kHz

    struct PlayRequest {
        std::filesystem::path wav_path;
        float gain{1.0f};
        bool send_a1{true};
        bool send_b1{true};
        bool send_b2{true};
        bool loop{false};
    };

    struct PlayResult {
        bool ok{false};
        int slot{-1};
        uint32_t duration_ms{0};
    };

    /// Control-thread: decodes a WAV file (PCM16, PCM24, IEEE Float32; mono/stereo),
    /// resamples linearly to 48 kHz stereo, and triggers playback on a free voice slot.
    [[nodiscard]] PlayResult play(const PlayRequest& req) {
        std::vector<float> left;
        std::vector<float> right;
        if (!decode_wav_to_48k_stereo(req.wav_path, left, right) || left.empty()) {
            return {};
        }

        const uint32_t duration_ms =
            static_cast<uint32_t>((left.size() * 1000ULL) / 48000ULL);

        std::lock_guard lock(control_mutex_);
        int chosen = -1;
        for (std::size_t i = 0; i < kMaxVoices; ++i) {
            if (voices_[i].state.load(std::memory_order_acquire) == VoiceState::Idle) {
                chosen = static_cast<int>(i);
                break;
            }
        }
        if (chosen < 0) {
            // Steal voice 0 cleanly by waiting briefly after requesting stop
            voices_[0].stop_requested.store(true, std::memory_order_release);
            return {};
        }

        auto& v = voices_[static_cast<std::size_t>(chosen)];
        v.pcm_l = std::move(left);
        v.pcm_r = std::move(right);
        v.frame_count = v.pcm_l.size();
        v.playhead = 0;
        v.fade_remaining = 0;
        v.gain.store(std::clamp(req.gain, 0.0f, 2.0f), std::memory_order_relaxed);
        v.send_a1.store(req.send_a1, std::memory_order_relaxed);
        v.send_b1.store(req.send_b1, std::memory_order_relaxed);
        v.send_b2.store(req.send_b2, std::memory_order_relaxed);
        v.loop.store(req.loop, std::memory_order_relaxed);
        v.stop_requested.store(false, std::memory_order_relaxed);
        v.state.store(VoiceState::Playing, std::memory_order_release);

        return {true, chosen, duration_ms};
    }

    /// Control-thread: requests click-free fade-out on one slot (`-1` = all voices).
    void stop(int slot = -1) noexcept {
        for (std::size_t i = 0; i < kMaxVoices; ++i) {
            if (slot >= 0 && static_cast<std::size_t>(slot) != i) {
                continue;
            }
            if (voices_[i].state.load(std::memory_order_acquire) == VoiceState::Playing) {
                voices_[i].stop_requested.store(true, std::memory_order_release);
            }
        }
    }

    [[nodiscard]] uint32_t active_voices() const noexcept {
        uint32_t count = 0;
        for (const auto& v : voices_) {
            if (v.state.load(std::memory_order_acquire) != VoiceState::Idle) {
                ++count;
            }
        }
        return count;
    }

    /// RT-thread: mixes all active voices into Bus A1, B1, and B2 buffers (zero locks, zero allocs).
    void mix_into_buses(float* a1_l,
                        float* a1_r,
                        float* b1_l,
                        float* b1_r,
                        float* b2_l,
                        float* b2_r,
                        std::size_t frames) noexcept {
        if (frames == 0) {
            return;
        }

        for (auto& v : voices_) {
            VoiceState st = v.state.load(std::memory_order_acquire);
            if (st == VoiceState::Idle) {
                continue;
            }
            if (st == VoiceState::Playing && v.stop_requested.load(std::memory_order_acquire)) {
                st = VoiceState::Stopping;
                v.fade_remaining = kFadeOutFrames;
                v.state.store(VoiceState::Stopping, std::memory_order_release);
            }

            const float g = v.gain.load(std::memory_order_relaxed);
            const bool to_a1 = v.send_a1.load(std::memory_order_relaxed);
            const bool to_b1 = v.send_b1.load(std::memory_order_relaxed);
            const bool to_b2 = v.send_b2.load(std::memory_order_relaxed);
            const bool do_loop = v.loop.load(std::memory_order_relaxed);

            for (std::size_t i = 0; i < frames; ++i) {
                if (v.playhead >= v.frame_count) {
                    if (do_loop && st == VoiceState::Playing && v.frame_count > 0) {
                        v.playhead = 0;
                    } else {
                        v.state.store(VoiceState::Idle, std::memory_order_release);
                        break;
                    }
                }

                float env = g;
                if (st == VoiceState::Stopping) {
                    if (v.fade_remaining == 0) {
                        v.state.store(VoiceState::Idle, std::memory_order_release);
                        break;
                    }
                    env *= static_cast<float>(v.fade_remaining) / static_cast<float>(kFadeOutFrames);
                    --v.fade_remaining;
                }

                const float sl = v.pcm_l[v.playhead] * env;
                const float sr = v.pcm_r[v.playhead] * env;
                ++v.playhead;

                if (to_a1 && a1_l != nullptr && a1_r != nullptr) {
                    a1_l[i] += sl;
                    a1_r[i] += sr;
                }
                if (to_b1 && b1_l != nullptr && b1_r != nullptr) {
                    b1_l[i] += sl;
                    b1_r[i] += sr;
                }
                if (to_b2 && b2_l != nullptr && b2_r != nullptr) {
                    b2_l[i] += sl;
                    b2_r[i] += sr;
                }
            }
        }
    }

private:
    enum class VoiceState : uint8_t {
        Idle = 0,
        Playing = 1,
        Stopping = 2
    };

    struct VoiceSlot {
        std::atomic<VoiceState> state{VoiceState::Idle};
        std::atomic<bool> stop_requested{false};
        std::atomic<float> gain{1.0f};
        std::atomic<bool> send_a1{true};
        std::atomic<bool> send_b1{true};
        std::atomic<bool> send_b2{true};
        std::atomic<bool> loop{false};

        std::vector<float> pcm_l;
        std::vector<float> pcm_r;
        std::size_t frame_count{0};
        std::size_t playhead{0};
        std::size_t fade_remaining{0};
    };

    static uint16_t read_u16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }

    static uint32_t read_u32(const uint8_t* p) noexcept {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    [[nodiscard]] static bool decode_wav_to_48k_stereo(const std::filesystem::path& path,
                                                       std::vector<float>& out_l,
                                                       std::vector<float>& out_r) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff file_size = in.tellg();
        if (file_size < 44 || file_size > 64 * 1024 * 1024) {
            return false;
        }
        in.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(static_cast<std::size_t>(file_size));
        if (!in.read(reinterpret_cast<char*>(bytes.data()), file_size)) {
            return false;
        }

        if (std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
            std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
            return false;
        }

        uint16_t audio_format = 0;
        uint16_t channels = 0;
        uint32_t sample_rate = 0;
        uint16_t bits_per_sample = 0;
        std::size_t data_offset = 0;
        uint32_t data_size = 0;

        std::size_t pos = 12;
        while (pos + 8 <= bytes.size()) {
            const uint32_t chunk_size = read_u32(bytes.data() + pos + 4);
            const std::size_t payload = pos + 8;
            if (chunk_size > bytes.size() - payload) {
                break;
            }
            if (std::memcmp(bytes.data() + pos, "fmt ", 4) == 0 && chunk_size >= 16) {
                audio_format = read_u16(bytes.data() + payload);
                channels = read_u16(bytes.data() + payload + 2);
                sample_rate = read_u32(bytes.data() + payload + 4);
                bits_per_sample = read_u16(bytes.data() + payload + 14);
            } else if (std::memcmp(bytes.data() + pos, "data", 4) == 0) {
                data_offset = payload;
                data_size = chunk_size;
            }
            pos = payload + chunk_size + (chunk_size & 1u);
        }

        if (data_offset == 0 || data_size == 0 || channels < 1 || channels > 2 ||
            sample_rate < 8000 || sample_rate > 192000) {
            return false;
        }
        const bool is_pcm = (audio_format == 1 && (bits_per_sample == 16 || bits_per_sample == 24));
        const bool is_float = (audio_format == 3 && bits_per_sample == 32);
        if (!is_pcm && !is_float) {
            return false;
        }

        const std::size_t bytes_per_sample = bits_per_sample / 8;
        const std::size_t frame_bytes = bytes_per_sample * channels;
        const std::size_t src_frames = data_size / frame_bytes;
        if (src_frames == 0) {
            return false;
        }

        std::vector<float> src_l(src_frames, 0.0f);
        std::vector<float> src_r(src_frames, 0.0f);

        auto decode_sample = [&](const uint8_t* p) -> float {
            if (is_float) {
                float f = 0.0f;
                std::memcpy(&f, p, sizeof(float));
                return std::isfinite(f) ? std::clamp(f, -1.0f, 1.0f) : 0.0f;
            }
            if (bits_per_sample == 16) {
                const int16_t s = static_cast<int16_t>(read_u16(p));
                return static_cast<float>(s) / 32768.0f;
            }
            const int32_t s24 = static_cast<int32_t>(p[0]) |
                                (static_cast<int32_t>(p[1]) << 8) |
                                (static_cast<int32_t>(static_cast<int8_t>(p[2])) << 16);
            return static_cast<float>(s24) / 8388608.0f;
        };

        const uint8_t* src_ptr = bytes.data() + data_offset;
        for (std::size_t i = 0; i < src_frames; ++i) {
            const uint8_t* frame_p = src_ptr + i * frame_bytes;
            src_l[i] = decode_sample(frame_p);
            src_r[i] = (channels == 2) ? decode_sample(frame_p + bytes_per_sample) : src_l[i];
        }

        if (sample_rate == 48000) {
            out_l = std::move(src_l);
            out_r = std::move(src_r);
            return true;
        }

        const double ratio = static_cast<double>(sample_rate) / 48000.0;
        const std::size_t dst_frames =
            std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<double>(src_frames) / ratio));
        out_l.resize(dst_frames);
        out_r.resize(dst_frames);

        for (std::size_t i = 0; i < dst_frames; ++i) {
            const double src_pos = static_cast<double>(i) * ratio;
            const std::size_t idx0 = std::min(static_cast<std::size_t>(src_pos), src_frames - 1);
            const std::size_t idx1 = std::min(idx0 + 1, src_frames - 1);
            const float frac = static_cast<float>(src_pos - static_cast<double>(idx0));
            out_l[i] = src_l[idx0] + frac * (src_l[idx1] - src_l[idx0]);
            out_r[i] = src_r[idx0] + frac * (src_r[idx1] - src_r[idx0]);
        }
        return true;
    }

    std::mutex control_mutex_;
    std::array<VoiceSlot, kMaxVoices> voices_{};
};

} // namespace atriswave::dsp
