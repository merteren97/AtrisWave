#pragma once

#include "atriswave/dsp/dsp_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace atriswave::dsp {

/// Adaptive 4-point Hermite cubic resampler driven by a PI Software DLL/PLL
/// controller that smoothly modulates the resampling ratio within +/-0.5%
/// to absorb hardware crystal clock drift without clicks or buffer underruns.
class AdaptivePllResampler final {
public:
    static constexpr double kMaxDriftRatio = 0.005; // +/-0.5% (+/-5000 ppm)

    AdaptivePllResampler() noexcept {
        prepare(48000.0, 48000.0);
    }

    void prepare(double nominal_in_rate, double nominal_out_rate) noexcept {
        nominal_in_rate_ = std::clamp(nominal_in_rate, 8000.0, 384000.0);
        nominal_out_rate_ = std::clamp(nominal_out_rate, 8000.0, 384000.0);
        nominal_ratio_ = nominal_in_rate_ / nominal_out_rate_;
        reset();
    }

    void reset() noexcept {
        hist_l_.fill(0.0f);
        hist_r_.fill(0.0f);
        phase_ = 0.0;
        integral_error_ = 0.0;
        ratio_offset_ = 0.0;
        current_ratio_ = nominal_ratio_;
    }

    /// Updates the PI loop filter from ring-buffer fill ratio (0.0 .. 1.0).
    /// Positive error (buffer fuller than target) increases input consumption rate.
    void update_pll(double current_fill_ratio, double target_fill_ratio = 0.5) noexcept {
        const double fill = std::clamp(current_fill_ratio, 0.0, 1.0);
        const double target = std::clamp(target_fill_ratio, 0.05, 0.95);
        const double error = fill - target;

        constexpr double kKp = 0.006;
        constexpr double kKi = 0.0002;
        constexpr double kMaxIntegral = kMaxDriftRatio / kKi;

        integral_error_ = std::clamp(integral_error_ + error, -kMaxIntegral, kMaxIntegral);
        ratio_offset_ = std::clamp(kKp * error + kKi * integral_error_,
                                   -kMaxDriftRatio,
                                   kMaxDriftRatio);
        current_ratio_ = nominal_ratio_ * (1.0 + ratio_offset_);
    }

    void set_ratio_offset(double offset) noexcept {
        ratio_offset_ = std::clamp(offset, -kMaxDriftRatio, kMaxDriftRatio);
        current_ratio_ = nominal_ratio_ * (1.0 + ratio_offset_);
    }

    [[nodiscard]] double current_ratio() const noexcept { return current_ratio_; }
    [[nodiscard]] double ratio_offset() const noexcept { return ratio_offset_; }

    /// Streams mono input frames into output frames using 4-point Hermite cubic interpolation.
    /// Returns the number of input frames consumed; writes produced output count to out_frames_produced.
    std::size_t process_mono(const float* in,
                             std::size_t in_frames,
                             float* out,
                             std::size_t max_out_frames,
                             std::size_t& out_frames_produced) noexcept {
        out_frames_produced = 0;
        if (in == nullptr || out == nullptr || in_frames == 0 || max_out_frames == 0) {
            return 0;
        }

        std::size_t in_idx = 0;
        while (out_frames_produced < max_out_frames) {
            while (phase_ >= 1.0) {
                if (in_idx >= in_frames) {
                    return in_idx;
                }
                push_history_mono(in[in_idx]);
                ++in_idx;
                phase_ -= 1.0;
            }

            const float t = static_cast<float>(phase_);
            out[out_frames_produced++] = hermite4(hist_l_[0], hist_l_[1], hist_l_[2], hist_l_[3], t);
            phase_ += current_ratio_;
        }

        // Consume any remaining integer steps if phase >= 1.0
        while (phase_ >= 1.0 && in_idx < in_frames) {
            push_history_mono(in[in_idx]);
            ++in_idx;
            phase_ -= 1.0;
        }
        return in_idx;
    }

    /// Streams stereo planar input frames into stereo planar output frames.
    std::size_t process_stereo(const float* in_l,
                               const float* in_r,
                               std::size_t in_frames,
                               float* out_l,
                               float* out_r,
                               std::size_t max_out_frames,
                               std::size_t& out_frames_produced) noexcept {
        out_frames_produced = 0;
        if (in_l == nullptr || in_r == nullptr || out_l == nullptr || out_r == nullptr ||
            in_frames == 0 || max_out_frames == 0) {
            return 0;
        }

        std::size_t in_idx = 0;
        while (out_frames_produced < max_out_frames) {
            while (phase_ >= 1.0) {
                if (in_idx >= in_frames) {
                    return in_idx;
                }
                push_history_stereo(in_l[in_idx], in_r[in_idx]);
                ++in_idx;
                phase_ -= 1.0;
            }

            const float t = static_cast<float>(phase_);
            out_l[out_frames_produced] = hermite4(hist_l_[0], hist_l_[1], hist_l_[2], hist_l_[3], t);
            out_r[out_frames_produced] = hermite4(hist_r_[0], hist_r_[1], hist_r_[2], hist_r_[3], t);
            ++out_frames_produced;
            phase_ += current_ratio_;
        }

        while (phase_ >= 1.0 && in_idx < in_frames) {
            push_history_stereo(in_l[in_idx], in_r[in_idx]);
            ++in_idx;
            phase_ -= 1.0;
        }
        return in_idx;
    }

private:
    void push_history_mono(float sample) noexcept {
        const float clean = std::isfinite(sample) ? sample : 0.0f;
        hist_l_[0] = hist_l_[1];
        hist_l_[1] = hist_l_[2];
        hist_l_[2] = hist_l_[3];
        hist_l_[3] = clean;
    }

    void push_history_stereo(float l, float r) noexcept {
        const float clean_l = std::isfinite(l) ? l : 0.0f;
        const float clean_r = std::isfinite(r) ? r : 0.0f;
        hist_l_[0] = hist_l_[1];
        hist_l_[1] = hist_l_[2];
        hist_l_[2] = hist_l_[3];
        hist_l_[3] = clean_l;

        hist_r_[0] = hist_r_[1];
        hist_r_[1] = hist_r_[2];
        hist_r_[2] = hist_r_[3];
        hist_r_[3] = clean_r;
    }

    [[nodiscard]] static float hermite4(float ym1, float y0, float y1, float y2, float t) noexcept {
        const float c0 = y0;
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    double nominal_in_rate_{48000.0};
    double nominal_out_rate_{48000.0};
    double nominal_ratio_{1.0};
    double current_ratio_{1.0};
    double ratio_offset_{0.0};
    double integral_error_{0.0};
    double phase_{0.0};

    std::array<float, 4> hist_l_{};
    std::array<float, 4> hist_r_{};
};

} // namespace atriswave::dsp
