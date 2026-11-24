#pragma once

#include "atriswave/dsp/dsp_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace atriswave::dsp {

/// Real-time safe stereo/mono noise gate with hysteresis, attack/hold/release,
/// range floor attenuation, and a pre-allocated lookahead circular buffer (max 2 ms).
class NoiseGate final {
public:
    static constexpr float kMaxLookaheadMs = 2.0f;
    static constexpr std::size_t kMaxLookaheadSamples = 512; // Supports 2 ms up to 256 kHz

    NoiseGate() noexcept {
        prepare(48000.0);
    }

    void prepare(double sample_rate, std::size_t /*max_block_size*/ = 0) noexcept {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 256000.0);
        update_coefficients();
        reset();
    }

    void reset() noexcept {
        delay_left_.fill(0.0f);
        delay_right_.fill(0.0f);
        write_pos_ = 0;
        is_open_ = false;
        hold_counter_ = 0;
        gain_ = range_linear_;
        gain_reduction_db_ = -linear_to_db(std::max(gain_, 1e-6f));
    }

    void set_params(float open_threshold_db,
                    float close_threshold_db,
                    float attack_ms,
                    float hold_ms,
                    float release_ms,
                    float range_db = -80.0f,
                    float lookahead_ms = 1.5f) noexcept {
        open_threshold_db_ = std::clamp(open_threshold_db, -100.0f, 0.0f);
        close_threshold_db_ = std::clamp(close_threshold_db, -100.0f, open_threshold_db_);
        attack_ms_ = std::clamp(attack_ms, 0.05f, 500.0f);
        hold_ms_ = std::clamp(hold_ms, 0.0f, 2000.0f);
        release_ms_ = std::clamp(release_ms, 1.0f, 5000.0f);
        range_db_ = std::clamp(range_db, -120.0f, 0.0f);
        lookahead_ms_ = std::clamp(lookahead_ms, 0.0f, kMaxLookaheadMs);
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

        const float detector = std::max(std::abs(clean_l), std::abs(clean_r));
        const float detector_db = linear_to_db(detector);

        if (is_open_) {
            if (detector_db >= close_threshold_db_) {
                hold_counter_ = hold_samples_;
            } else if (hold_counter_ > 0) {
                --hold_counter_;
            } else {
                is_open_ = false;
            }
        } else {
            if (detector_db >= open_threshold_db_) {
                is_open_ = true;
                hold_counter_ = hold_samples_;
            }
        }

        const float target_gain = is_open_ ? 1.0f : range_linear_;
        const float coeff = (target_gain > gain_) ? attack_coeff_ : release_coeff_;
        gain_ = target_gain + coeff * (gain_ - target_gain);
        gain_ = flush_denormal(std::clamp(gain_, range_linear_, 1.0f));

        gain_reduction_db_ = std::max(0.0f, -linear_to_db(std::max(gain_, 1e-6f)));

        if (lookahead_samples_ == 0) {
            left = clean_l * gain_;
            right = clean_r * gain_;
            return;
        }

        const std::size_t read_pos = (write_pos_ + kMaxLookaheadSamples - lookahead_samples_) % kMaxLookaheadSamples;
        const float delayed_l = delay_left_[read_pos];
        const float delayed_r = delay_right_[read_pos];

        delay_left_[write_pos_] = clean_l;
        delay_right_[write_pos_] = clean_r;
        write_pos_ = (write_pos_ + 1) % kMaxLookaheadSamples;

        left = delayed_l * gain_;
        right = delayed_r * gain_;
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

    [[nodiscard]] float gain_reduction_db() const noexcept { return gain_reduction_db_; }
    [[nodiscard]] bool is_open() const noexcept { return is_open_; }
    [[nodiscard]] std::size_t latency_samples() const noexcept { return lookahead_samples_; }

private:
    void update_coefficients() noexcept {
        attack_coeff_ = compute_one_pole_coeff(sample_rate_, attack_ms_);
        release_coeff_ = compute_one_pole_coeff(sample_rate_, release_ms_);
        hold_samples_ = static_cast<std::size_t>(std::round(static_cast<double>(hold_ms_) * 0.001 * sample_rate_));
        range_linear_ = db_to_linear(range_db_);
        const auto requested_lookahead = static_cast<std::size_t>(
            std::round(static_cast<double>(lookahead_ms_) * 0.001 * sample_rate_));
        lookahead_samples_ = std::min(requested_lookahead, kMaxLookaheadSamples - 1);
    }

    double sample_rate_{48000.0};
    float open_threshold_db_{-35.0f};
    float close_threshold_db_{-42.0f};
    float attack_ms_{2.0f};
    float hold_ms_{50.0f};
    float release_ms_{120.0f};
    float range_db_{-80.0f};
    float lookahead_ms_{1.5f};

    float attack_coeff_{0.0f};
    float release_coeff_{0.0f};
    float range_linear_{0.0001f};
    std::size_t hold_samples_{2400};
    std::size_t lookahead_samples_{72};

    std::array<float, kMaxLookaheadSamples> delay_left_{};
    std::array<float, kMaxLookaheadSamples> delay_right_{};
    std::size_t write_pos_{0};

    bool is_open_{false};
    std::size_t hold_counter_{0};
    float gain_{1.0f};
    float gain_reduction_db_{0.0f};
};

} // namespace atriswave::dsp
