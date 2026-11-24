#pragma once

#include "atriswave/dsp/dsp_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace atriswave::dsp {

/// Real-time safe dynamics compressor with Peak/RMS detector, soft knee,
/// attack/release ballistics, makeup gain, and external sidechain support.
class DynamicsCompressor final {
public:
    DynamicsCompressor() noexcept {
        prepare(48000.0);
    }

    void prepare(double sample_rate) noexcept {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 384000.0);
        update_coefficients();
        reset();
    }

    void reset() noexcept {
        rms_sq_ = 0.0f;
        smoothed_gr_db_ = 0.0f;
        gain_reduction_db_ = 0.0f;
    }

    void set_params(float threshold_db,
                    float ratio,
                    float knee_db,
                    float attack_ms,
                    float release_ms,
                    float makeup_db = 0.0f,
                    bool use_rms = false) noexcept {
        threshold_db_ = std::clamp(threshold_db, -80.0f, 0.0f);
        ratio_ = std::clamp(ratio, 1.0f, 100.0f);
        knee_db_ = std::clamp(knee_db, 0.0f, 36.0f);
        attack_ms_ = std::clamp(attack_ms, 0.05f, 500.0f);
        release_ms_ = std::clamp(release_ms, 1.0f, 5000.0f);
        makeup_db_ = std::clamp(makeup_db, -24.0f, 36.0f);
        use_rms_ = use_rms;
        update_coefficients();
    }

    [[nodiscard]] float process_sample(float input) noexcept {
        return process_with_sidechain(input, input);
    }

    [[nodiscard]] float process_with_sidechain(float input, float sidechain_sample) noexcept {
        const float clean_in = std::isfinite(input) ? input : 0.0f;
        const float clean_sc = std::isfinite(sidechain_sample) ? sidechain_sample : 0.0f;
        const float gain = compute_gain(std::abs(clean_sc));
        return flush_denormal(clean_in * gain);
    }

    void process_stereo(float& left, float& right) noexcept {
        const float clean_l = std::isfinite(left) ? left : 0.0f;
        const float clean_r = std::isfinite(right) ? right : 0.0f;
        const float detector = std::max(std::abs(clean_l), std::abs(clean_r));
        const float gain = compute_gain(detector);
        left = flush_denormal(clean_l * gain);
        right = flush_denormal(clean_r * gain);
    }

    void process_stereo_with_sidechain(float& left, float& right, float sidechain_sample) noexcept {
        const float clean_l = std::isfinite(left) ? left : 0.0f;
        const float clean_r = std::isfinite(right) ? right : 0.0f;
        const float clean_sc = std::isfinite(sidechain_sample) ? sidechain_sample : 0.0f;
        const float gain = compute_gain(std::abs(clean_sc));
        left = flush_denormal(clean_l * gain);
        right = flush_denormal(clean_r * gain);
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

private:
    void update_coefficients() noexcept {
        attack_coeff_ = compute_one_pole_coeff(sample_rate_, attack_ms_);
        release_coeff_ = compute_one_pole_coeff(sample_rate_, release_ms_);
        rms_coeff_ = compute_one_pole_coeff(sample_rate_, 10.0f);
        makeup_linear_ = db_to_linear(makeup_db_);
    }

    [[nodiscard]] float compute_gain(float abs_detector) noexcept {
        float level = abs_detector;
        if (use_rms_) {
            const float sq = abs_detector * abs_detector;
            rms_sq_ = sq + rms_coeff_ * (rms_sq_ - sq);
            rms_sq_ = flush_denormal(std::max(0.0f, rms_sq_));
            level = std::sqrt(rms_sq_);
        }

        const float env_db = linear_to_db(level);
        const float target_gr_db = compute_static_gr_db(env_db);

        const float coeff = (target_gr_db > smoothed_gr_db_) ? attack_coeff_ : release_coeff_;
        smoothed_gr_db_ = target_gr_db + coeff * (smoothed_gr_db_ - target_gr_db);
        smoothed_gr_db_ = flush_denormal(std::max(0.0f, smoothed_gr_db_));
        gain_reduction_db_ = smoothed_gr_db_;

        return db_to_linear(-smoothed_gr_db_) * makeup_linear_;
    }

    [[nodiscard]] float compute_static_gr_db(float env_db) const noexcept {
        if (ratio_ <= 1.0001f) {
            return 0.0f;
        }
        const float overshoot = env_db - threshold_db_;
        const float half_knee = 0.5f * knee_db_;

        float out_db = env_db;
        if (knee_db_ <= 0.001f) {
            if (overshoot > 0.0f) {
                out_db = threshold_db_ + overshoot / ratio_;
            }
        } else if (overshoot <= -half_knee) {
            out_db = env_db;
        } else if (overshoot < half_knee) {
            const float x = overshoot + half_knee;
            out_db = env_db + (1.0f / ratio_ - 1.0f) * (x * x) / (2.0f * knee_db_);
        } else {
            out_db = threshold_db_ + overshoot / ratio_;
        }

        return std::max(0.0f, env_db - out_db);
    }

    double sample_rate_{48000.0};
    float threshold_db_{-18.0f};
    float ratio_{4.0f};
    float knee_db_{6.0f};
    float attack_ms_{10.0f};
    float release_ms_{100.0f};
    float makeup_db_{0.0f};
    bool use_rms_{false};

    float attack_coeff_{0.0f};
    float release_coeff_{0.0f};
    float rms_coeff_{0.0f};
    float makeup_linear_{1.0f};

    float rms_sq_{0.0f};
    float smoothed_gr_db_{0.0f};
    float gain_reduction_db_{0.0f};
};

} // namespace atriswave::dsp
