#pragma once

#include "atriswave/dsp/compressor.hpp"
#include "atriswave/dsp/dsp_math.hpp"
#include "atriswave/dsp/limiter.hpp"
#include "atriswave/ipc_protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace atriswave::dsp {

enum class ChannelId : std::uint32_t {
    Master = 0,
    Game = 1,
    Chat = 2,
    Media = 3,
    Aux = 4,
    Mic = 5
};

inline constexpr std::size_t kMixerChannelCount = 6;

struct MixerChannelAtomic {
    std::atomic<float> volume{1.0f};
    std::atomic<bool> muted{false};
    std::atomic<bool> solo{false};
};

struct MixerChannelMeter {
    float peak_left{0.0f};
    float peak_right{0.0f};
    float rms_left{0.0f};
    float rms_right{0.0f};
    float gain_reduction_db{0.0f};
    bool muted{false};
    bool clipping{false};
    bool active{false};
};

struct MixerChannelMeterAtomic {
    std::atomic<float> peak_left{0.0f};
    std::atomic<float> peak_right{0.0f};
    std::atomic<float> rms_left{0.0f};
    std::atomic<float> rms_right{0.0f};
    std::atomic<float> gain_reduction_db{0.0f};
    std::atomic<bool> muted{false};
    std::atomic<bool> clipping{false};
    std::atomic<bool> active{false};

    void store(const MixerChannelMeter& m) noexcept {
        peak_left.store(m.peak_left, std::memory_order_relaxed);
        peak_right.store(m.peak_right, std::memory_order_relaxed);
        rms_left.store(m.rms_left, std::memory_order_relaxed);
        rms_right.store(m.rms_right, std::memory_order_relaxed);
        gain_reduction_db.store(m.gain_reduction_db, std::memory_order_relaxed);
        muted.store(m.muted, std::memory_order_relaxed);
        clipping.store(m.clipping, std::memory_order_relaxed);
        active.store(m.active, std::memory_order_relaxed);
    }

    [[nodiscard]] MixerChannelMeter load() const noexcept {
        MixerChannelMeter m;
        m.peak_left = peak_left.load(std::memory_order_relaxed);
        m.peak_right = peak_right.load(std::memory_order_relaxed);
        m.rms_left = rms_left.load(std::memory_order_relaxed);
        m.rms_right = rms_right.load(std::memory_order_relaxed);
        m.gain_reduction_db = gain_reduction_db.load(std::memory_order_relaxed);
        m.muted = muted.load(std::memory_order_relaxed);
        m.clipping = clipping.load(std::memory_order_relaxed);
        m.active = active.load(std::memory_order_relaxed);
        return m;
    }
};

/// 6-channel real-time mixer bus (Master=0, Game=1, Chat=2, Media=3, Aux=4, Mic=5)
/// with smoothed per-channel gain, mute/solo logic, Mic -> Game/Media sidechain
/// ducking, master brickwall limiter, and per-channel Peak/RMS/Clipping telemetry.
class MixerBus final {
public:
    MixerBus() noexcept {
        prepare(48000.0);
    }

    void prepare(double sample_rate) noexcept {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 384000.0);
        for (auto& s : gain_smoothers_) {
            s.prepare(sample_rate_, 10.0f);
            s.reset(1.0f);
        }
        ducker_.prepare(sample_rate_);
        ducker_.set_params(-32.0f, 8.0f, 6.0f, 15.0f, 250.0f, 0.0f, false);
        master_limiter_.prepare(sample_rate_);
        master_limiter_.set_params(-0.5f, 50.0f, 2.0f);
        reset();
    }

    void reset() noexcept {
        for (std::size_t i = 0; i < kMixerChannelCount; ++i) {
            gain_smoothers_[i].reset(channels_[i].volume.load(std::memory_order_relaxed));
            meters_[i].store(MixerChannelMeter{});
        }
        ducker_.reset();
        master_limiter_.reset();
    }

    void set_channel(ChannelId id, float volume, bool muted, bool solo) noexcept {
        const auto idx = static_cast<std::size_t>(id);
        if (idx >= kMixerChannelCount) {
            return;
        }
        channels_[idx].volume.store(std::clamp(volume, 0.0f, 2.0f), std::memory_order_relaxed);
        channels_[idx].muted.store(muted, std::memory_order_relaxed);
        channels_[idx].solo.store(solo, std::memory_order_relaxed);
    }

    void set_ducking(bool enabled, float depth_db = -12.0f, float threshold_db = -32.0f) noexcept {
        ducking_enabled_.store(enabled, std::memory_order_relaxed);
        ducking_depth_db_.store(std::clamp(depth_db, -48.0f, 0.0f), std::memory_order_relaxed);
        ducking_threshold_db_.store(std::clamp(threshold_db, -60.0f, -6.0f), std::memory_order_relaxed);
    }

    /// Mixes input channels [Game, Chat, Media, Aux, Mic] (indices 1..5) into master_out_l/r.
    /// Unused input pointers may be nullptr (treated as silence).
    /// external_mic_envelope can be >= 0.0f from MicPipeline, or < 0.0f to detect from Mic input.
    void process(const std::array<const float*, kMixerChannelCount>& in_l,
                 const std::array<const float*, kMixerChannelCount>& in_r,
                 float* master_out_l,
                 float* master_out_r,
                 std::size_t num_frames,
                 float external_mic_envelope = -1.0f) noexcept {
        if (master_out_l == nullptr || master_out_r == nullptr || num_frames == 0) {
            return;
        }

        // Determine if any non-master channel has solo active
        bool any_solo = false;
        for (std::size_t ch = 1; ch < kMixerChannelCount; ++ch) {
            if (channels_[ch].solo.load(std::memory_order_relaxed)) {
                any_solo = true;
                break;
            }
        }

        const bool master_muted = channels_[0].muted.load(std::memory_order_relaxed);
        const float master_vol = channels_[0].volume.load(std::memory_order_relaxed);
        gain_smoothers_[0].set_target(master_muted ? 0.0f : master_vol);

        std::array<bool, kMixerChannelCount> ch_muted{};
        ch_muted[0] = master_muted;

        for (std::size_t ch = 1; ch < kMixerChannelCount; ++ch) {
            const bool m = channels_[ch].muted.load(std::memory_order_relaxed);
            const bool s = channels_[ch].solo.load(std::memory_order_relaxed);
            const bool effective_mute = m || (any_solo && !s);
            ch_muted[ch] = effective_mute;
            const float vol = channels_[ch].volume.load(std::memory_order_relaxed);
            gain_smoothers_[ch].set_target(effective_mute ? 0.0f : vol);
        }

        const bool duck_on = ducking_enabled_.load(std::memory_order_relaxed);
        const float duck_depth = ducking_depth_db_.load(std::memory_order_relaxed);
        const float duck_thresh = ducking_threshold_db_.load(std::memory_order_relaxed);
        // Configure ducker ratio from depth_db (high ratio to clamp down by depth)
        ducker_.set_params(duck_thresh, 10.0f, 6.0f, 15.0f, 250.0f, 0.0f, false);
        const float min_duck_linear = db_to_linear(duck_depth);

        std::array<float, kMixerChannelCount> pk_l{};
        std::array<float, kMixerChannelCount> pk_r{};
        std::array<double, kMixerChannelCount> sq_l{};
        std::array<double, kMixerChannelCount> sq_r{};

        for (std::size_t i = 0; i < num_frames; ++i) {
            // Sidechain signal from Mic (channel 5) or external envelope
            float sc_sample = 0.0f;
            if (external_mic_envelope >= 0.0f) {
                sc_sample = external_mic_envelope;
            } else {
                const float ml = (in_l[5] != nullptr) ? in_l[5][i] : 0.0f;
                const float mr = (in_r[5] != nullptr) ? in_r[5][i] : ml;
                sc_sample = std::max(std::abs(ml), std::abs(mr));
            }

            float duck_gain = 1.0f;
            if (duck_on) {
                duck_gain = std::max(ducker_.process_with_sidechain(1.0f, sc_sample), min_duck_linear);
            }

            float mix_l = 0.0f;
            float mix_r = 0.0f;

            for (std::size_t ch = 1; ch < kMixerChannelCount; ++ch) {
                const float g = gain_smoothers_[ch].next();
                float l = (in_l[ch] != nullptr) ? in_l[ch][i] : 0.0f;
                float r = (in_r[ch] != nullptr) ? in_r[ch][i] : l;
                if (!std::isfinite(l)) l = 0.0f;
                if (!std::isfinite(r)) r = 0.0f;

                l *= g;
                r *= g;

                // Apply mic -> Game (1) & Media (3) sidechain ducking
                if (duck_on && (ch == static_cast<std::size_t>(ChannelId::Game) ||
                                ch == static_cast<std::size_t>(ChannelId::Media))) {
                    l *= duck_gain;
                    r *= duck_gain;
                }

                pk_l[ch] = std::max(pk_l[ch], std::abs(l));
                pk_r[ch] = std::max(pk_r[ch], std::abs(r));
                sq_l[ch] += static_cast<double>(l) * l;
                sq_r[ch] += static_cast<double>(r) * r;

                mix_l += l;
                mix_r += r;
            }

            const float mg = gain_smoothers_[0].next();
            mix_l *= mg;
            mix_r *= mg;

            // Master soft-clip + brickwall limiter
            master_limiter_.process_stereo(mix_l, mix_r);

            master_out_l[i] = mix_l;
            master_out_r[i] = mix_r;

            pk_l[0] = std::max(pk_l[0], std::abs(mix_l));
            pk_r[0] = std::max(pk_r[0], std::abs(mix_r));
            sq_l[0] += static_cast<double>(mix_l) * mix_l;
            sq_r[0] += static_cast<double>(mix_r) * mix_r;
        }

        const double inv_frames = 1.0 / static_cast<double>(num_frames);
        const float duck_gr_db = duck_on ? ducker_.gain_reduction_db() : 0.0f;

        for (std::size_t ch = 0; ch < kMixerChannelCount; ++ch) {
            MixerChannelMeter m;
            m.peak_left = pk_l[ch];
            m.peak_right = pk_r[ch];
            m.rms_left = static_cast<float>(std::sqrt(sq_l[ch] * inv_frames));
            m.rms_right = static_cast<float>(std::sqrt(sq_r[ch] * inv_frames));
            m.muted = ch_muted[ch];
            m.clipping = (pk_l[ch] >= 0.99f) || (pk_r[ch] >= 0.99f);
            m.active = (pk_l[ch] > 1e-5f) || (pk_r[ch] > 1e-5f);

            if (ch == 0) {
                m.gain_reduction_db = master_limiter_.gain_reduction_db();
            } else if (ch == static_cast<std::size_t>(ChannelId::Game) ||
                       ch == static_cast<std::size_t>(ChannelId::Media)) {
                m.gain_reduction_db = duck_gr_db;
            } else {
                m.gain_reduction_db = 0.0f;
            }
            meters_[ch].store(m);
        }
    }

    void fill_telemetry(ChannelMeterFrame* frames, std::size_t max_frames) const noexcept {
        if (frames == nullptr) {
            return;
        }
        const std::size_t count = std::min(max_frames, kMixerChannelCount);
        for (std::size_t i = 0; i < count; ++i) {
            const MixerChannelMeter m = meters_[i].load();
            std::uint32_t flags = ATRISWAVE_CHANNEL_FLAG_NONE;
            if (m.muted) flags |= ATRISWAVE_CHANNEL_FLAG_MUTED;
            if (m.clipping) flags |= ATRISWAVE_CHANNEL_FLAG_CLIPPING;
            if (m.active) flags |= ATRISWAVE_CHANNEL_FLAG_ACTIVE;

            frames[i].channel_id = static_cast<std::uint32_t>(i);
            frames[i].peak_left = m.peak_left;
            frames[i].peak_right = m.peak_right;
            frames[i].rms_left = m.rms_left;
            frames[i].rms_right = m.rms_right;
            frames[i].gain_reduction_db = m.gain_reduction_db;
            frames[i].flags = flags;
            frames[i].reserved = 0u;
        }
    }

    [[nodiscard]] MixerChannelMeter meter(ChannelId id) const noexcept {
        const auto idx = static_cast<std::size_t>(id);
        return meters_[std::min(idx, kMixerChannelCount - 1)].load();
    }

private:
    double sample_rate_{48000.0};
    std::array<MixerChannelAtomic, kMixerChannelCount> channels_{};
    std::array<OnePoleSmoother, kMixerChannelCount> gain_smoothers_{};
    std::array<MixerChannelMeterAtomic, kMixerChannelCount> meters_{};

    std::atomic<bool> ducking_enabled_{false};
    std::atomic<float> ducking_depth_db_{-12.0f};
    std::atomic<float> ducking_threshold_db_{-32.0f};

    DynamicsCompressor ducker_{};
    LookaheadLimiter master_limiter_{};
};

} // namespace atriswave::dsp
