#pragma once

#include "atriswave/dsp/dsp_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace atriswave::dsp {

/// Allocation-free real-time pitch/voice shifter using dual-tap Hann-windowed
/// fractional delay-line modulation (semitones in [-12.0f, +12.0f]).
class PitchShifter final {
public:
    static constexpr std::size_t kBufferSize = 4096;
    static constexpr std::size_t kBufferMask = kBufferSize - 1;
    static constexpr std::size_t kWindowSamples = 2048;

    PitchShifter() noexcept {
        prepare(48000.0);
    }

    void prepare(double sample_rate) noexcept {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 192000.0);
        reset();
    }

    void reset() noexcept {
        buffer_.fill(0.0f);
        write_pos_ = 0;
        phase_ = 0.0f;
    }

    void set_params(float semitones, float wet = 1.0f) noexcept {
        semitones_ = std::clamp(semitones, -12.0f, 12.0f);
        wet_ = std::clamp(wet, 0.0f, 1.0f);
        pitch_ratio_ = std::pow(2.0f, semitones_ / 12.0f);
    }

    [[nodiscard]] float process_sample(float input) noexcept {
        const float clean = std::isfinite(input) ? input : 0.0f;
        buffer_[write_pos_] = clean;

        if (std::abs(semitones_) <= 0.01f) {
            write_pos_ = (write_pos_ + 1) & kBufferMask;
            return clean;
        }

        const float phase_inc = (1.0f - pitch_ratio_) / static_cast<float>(kWindowSamples);
        phase_ += phase_inc;
        phase_ -= std::floor(phase_);

        float phase2 = phase_ + 0.5f;
        phase2 -= std::floor(phase2);

        constexpr float kBaseDelay = 2.0f;
        const float delay1 = kBaseDelay + phase_ * static_cast<float>(kWindowSamples);
        const float delay2 = kBaseDelay + phase2 * static_cast<float>(kWindowSamples);

        const float w1 = 0.5f * (1.0f - std::cos(static_cast<float>(kTwoPi) * phase_));
        const float w2 = 0.5f * (1.0f - std::cos(static_cast<float>(kTwoPi) * phase2));

        const float s1 = read_interpolated(delay1);
        const float s2 = read_interpolated(delay2);
        const float shifted = w1 * s1 + w2 * s2;

        write_pos_ = (write_pos_ + 1) & kBufferMask;
        return flush_denormal(clean + wet_ * (shifted - clean));
    }

    void process_block(float* samples, std::size_t num_frames) noexcept {
        if (samples == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < num_frames; ++i) {
            samples[i] = process_sample(samples[i]);
        }
    }

    [[nodiscard]] float semitones() const noexcept { return semitones_; }
    [[nodiscard]] float wet() const noexcept { return wet_; }

private:
    [[nodiscard]] float read_interpolated(float delay_samples) const noexcept {
        const float pos = static_cast<float>(write_pos_) - delay_samples;
        const float floor_pos = std::floor(pos);
        const float frac = pos - floor_pos;

        const auto i0 = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(floor_pos)) & kBufferMask;
        const auto i1 = (i0 + 1) & kBufferMask;
        return buffer_[i0] + frac * (buffer_[i1] - buffer_[i0]);
    }

    double sample_rate_{48000.0};
    float semitones_{0.0f};
    float wet_{1.0f};
    float pitch_ratio_{1.0f};
    float phase_{0.0f};

    std::array<float, kBufferSize> buffer_{};
    std::size_t write_pos_{0};
};

} // namespace atriswave::dsp
