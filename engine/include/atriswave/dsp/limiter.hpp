#pragma once

#include "atriswave/dsp/dsp_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace atriswave::dsp {

/// Brickwall lookahead peak limiter with pre-allocated circular delay (max 5 ms),
/// configurable ceiling (default -1.0 dBFS), fast release, and guaranteed output
/// |sample| <= ceiling_linear + 1e-4f.
class LookaheadLimiter final {
public:
    static constexpr float kMaxLookaheadMs = 5.0f;
    static constexpr std::size_t kMaxLookaheadSamples = 1024; // Supports 5 ms up to 204.8 kHz

    LookaheadLimiter() noexcept {
        prepare(48000.0);
    }

    void prepare(double sample_rate) noexcept {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 204800.0);
        update_coefficients();
        reset();
    }

    void reset() noexcept {
        delay_left_.fill(0.0f);
        delay_right_.fill(0.0f);
        peak_ring_.fill(0.0f);
        write_pos_ = 0;
        gain_ = 1.0f;
        gain_reduction_db_ = 0.0f;
    }

    void set_params(float ceiling_db = -1.0f,
                    float release_ms = 50.0f,
                    float lookahead_ms = 2.5f) noexcept {
        ceiling_db_ = std::clamp(ceiling_db, -24.0f, 0.0f);
        release_ms_ = std::clamp(release_ms, 5.0f, 1000.0f);
        lookahead_ms_ = std::clamp(lookahead_ms, 0.2f, kMaxLookaheadMs);
        update_coefficients();
    }

    [[nodiscard]] float process_sample(float input) noexcept {
        float left = input;
        float right = 0.0f;
        process_stereo(left, right);
        return left;
    }

    void process_stereo(float& left, float& right) noexcept {
        const float clean_l = std::isfinite(left) ? left : 0.0f;
        const float clean_r = std::isfinite(right) ? right : 0.0f;
        const float in_peak = std::max(std::abs(clean_l), std::abs(clean_r));

        // Push current sample into delay & peak rings
        delay_left_[write_pos_] = clean_l;
        delay_right_[write_pos_] = clean_r;
        peak_ring_[write_pos_] = in_peak;

        // Find maximum peak in the lookahead window [0 .. lookahead_samples_]
        float window_peak = 0.0f;
        for (std::size_t i = 0; i <= lookahead_samples_; ++i) {
            const std::size_t idx = (write_pos_ + kMaxLookaheadSamples - i) % kMaxLookaheadSamples;
            window_peak = std::max(window_peak, peak_ring_[idx]);
        }

        const float required_gain =
            (window_peak > ceiling_linear_) ? (ceiling_linear_ / window_peak) : 1.0f;

        if (required_gain < gain_) {
            // Smoothly ramp down over lookahead window or snap if needed
            const float step_coeff = attack_coeff_;
            gain_ = required_gain + step_coeff * (gain_ - required_gain);
            if (gain_ > required_gain) {
                // Ensure we never exceed required_gain for the exiting sample
                const std::size_t read_pos =
                    (write_pos_ + kMaxLookaheadSamples - lookahead_samples_) % kMaxLookaheadSamples;
                const float exiting_peak = std::max(std::abs(delay_left_[read_pos]),
                                                    std::abs(delay_right_[read_pos]));
                if (exiting_peak * gain_ > ceiling_linear_ && exiting_peak > 0.0f) {
                    gain_ = ceiling_linear_ / exiting_peak;
                }
            }
        } else {
            gain_ = required_gain + release_coeff_ * (gain_ - required_gain);
        }

        gain_ = std::clamp(gain_, 0.0f, 1.0f);
        gain_reduction_db_ = std::max(0.0f, -linear_to_db(std::max(gain_, 1e-6f)));

        const std::size_t read_pos =
            (write_pos_ + kMaxLookaheadSamples - lookahead_samples_) % kMaxLookaheadSamples;
        const float out_l = delay_left_[read_pos] * gain_;
        const float out_r = delay_right_[read_pos] * gain_;

        write_pos_ = (write_pos_ + 1) % kMaxLookaheadSamples;

        left = std::clamp(flush_denormal(out_l), -ceiling_linear_, ceiling_linear_);
        right = std::clamp(flush_denormal(out_r), -ceiling_linear_, ceiling_linear_);
    }

    void process_block(float* samples, std::size_t num_frames) noexcept {
        if (samples == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < num_frames; ++i) {
            samples[i] = process_sample(samples[i]);
        }
    }

    void process_block(float* left, float* right, std::size_t num_frames) noexcept {
        if (left == nullptr || right == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < num_frames; ++i) {
            process_stereo(left[i], right[i]);
        }
    }

    [[nodiscard]] float ceiling_linear() const noexcept { return ceiling_linear_; }
    [[nodiscard]] float gain_reduction_db() const noexcept { return gain_reduction_db_; }
    [[nodiscard]] std::size_t latency_samples() const noexcept { return lookahead_samples_; }

private:
    void update_coefficients() noexcept {
        ceiling_linear_ = db_to_linear(ceiling_db_);
        release_coeff_ = compute_one_pole_coeff(sample_rate_, release_ms_);
        // Fast attack ~1/3 of lookahead window
        attack_coeff_ = compute_one_pole_coeff(sample_rate_, std::max(0.05f, lookahead_ms_ * 0.33f));
        const auto requested = static_cast<std::size_t>(
            std::round(static_cast<double>(lookahead_ms_) * 0.001 * sample_rate_));
        lookahead_samples_ = std::clamp(requested, std::size_t{1}, kMaxLookaheadSamples - 1);
    }

    double sample_rate_{48000.0};
    float ceiling_db_{-1.0f};
    float ceiling_linear_{0.8912509f};
    float release_ms_{50.0f};
    float lookahead_ms_{2.5f};

    float attack_coeff_{0.0f};
    float release_coeff_{0.0f};
    std::size_t lookahead_samples_{120};

    std::array<float, kMaxLookaheadSamples> delay_left_{};
    std::array<float, kMaxLookaheadSamples> delay_right_{};
    std::array<float, kMaxLookaheadSamples> peak_ring_{};
    std::size_t write_pos_{0};

    float gain_{1.0f};
    float gain_reduction_db_{0.0f};
};

} // namespace atriswave::dsp
