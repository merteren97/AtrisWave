#pragma once

#include "atriswave/dsp/dsp_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace atriswave::dsp {

/// Allocation-free (during process) Freeverb-style stereo algorithmic reverb
/// using 8 parallel lowpass-damped comb filters and 4 series allpass filters
/// per channel with stereo spread and optional pre-delay.
class StereoReverb final {
public:
    static constexpr std::size_t kNumCombs = 8;
    static constexpr std::size_t kNumAllpasses = 4;

    StereoReverb() {
        prepare(48000.0);
    }

    void prepare(double sample_rate) {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 384000.0);
        const double scale = sample_rate_ / 44100.0;

        static constexpr std::array<std::size_t, kNumCombs> kCombTunings = {
            1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
        };
        static constexpr std::array<std::size_t, kNumAllpasses> kAllpassTunings = {
            556, 441, 341, 225
        };
        const std::size_t stereo_spread =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::round(23.0 * scale)));

        for (std::size_t i = 0; i < kNumCombs; ++i) {
            const std::size_t len_l =
                std::max<std::size_t>(16, static_cast<std::size_t>(std::round(kCombTunings[i] * scale)));
            const std::size_t len_r = len_l + stereo_spread;
            combs_l_[i].buffer.assign(len_l, 0.0f);
            combs_r_[i].buffer.assign(len_r, 0.0f);
            combs_l_[i].index = 0;
            combs_r_[i].index = 0;
            combs_l_[i].filter_store = 0.0f;
            combs_r_[i].filter_store = 0.0f;
        }

        for (std::size_t i = 0; i < kNumAllpasses; ++i) {
            const std::size_t len_l =
                std::max<std::size_t>(16, static_cast<std::size_t>(std::round(kAllpassTunings[i] * scale)));
            const std::size_t len_r = len_l + stereo_spread;
            allpasses_l_[i].buffer.assign(len_l, 0.0f);
            allpasses_r_[i].buffer.assign(len_r, 0.0f);
            allpasses_l_[i].index = 0;
            allpasses_r_[i].index = 0;
        }

        // Pre-allocate up to 150 ms pre-delay
        const std::size_t max_predelay =
            static_cast<std::size_t>(std::ceil(0.150 * sample_rate_)) + 1;
        pre_delay_l_.assign(max_predelay, 0.0f);
        pre_delay_r_.assign(max_predelay, 0.0f);
        pre_delay_write_ = 0;

        update_parameters();
        reset();
    }

    void reset() noexcept {
        for (auto& c : combs_l_) {
            std::fill(c.buffer.begin(), c.buffer.end(), 0.0f);
            c.index = 0;
            c.filter_store = 0.0f;
        }
        for (auto& c : combs_r_) {
            std::fill(c.buffer.begin(), c.buffer.end(), 0.0f);
            c.index = 0;
            c.filter_store = 0.0f;
        }
        for (auto& a : allpasses_l_) {
            std::fill(a.buffer.begin(), a.buffer.end(), 0.0f);
            a.index = 0;
        }
        for (auto& a : allpasses_r_) {
            std::fill(a.buffer.begin(), a.buffer.end(), 0.0f);
            a.index = 0;
        }
        std::fill(pre_delay_l_.begin(), pre_delay_l_.end(), 0.0f);
        std::fill(pre_delay_r_.begin(), pre_delay_r_.end(), 0.0f);
        pre_delay_write_ = 0;
    }

    void set_params(float room_size,
                    float damping,
                    float wet,
                    float dry,
                    float pre_delay_ms = 0.0f,
                    float width = 1.0f) noexcept {
        room_size_ = std::clamp(room_size, 0.0f, 1.0f);
        damping_ = std::clamp(damping, 0.0f, 1.0f);
        wet_ = std::clamp(wet, 0.0f, 1.0f);
        dry_ = std::clamp(dry, 0.0f, 1.0f);
        pre_delay_ms_ = std::clamp(pre_delay_ms, 0.0f, 120.0f);
        width_ = std::clamp(width, 0.0f, 1.0f);
        update_parameters();
    }

    void process_stereo(float& left, float& right) noexcept {
        const float in_l = std::isfinite(left) ? left : 0.0f;
        const float in_r = std::isfinite(right) ? right : 0.0f;

        float pd_l = in_l;
        float pd_r = in_r;

        if (!pre_delay_l_.empty() && pre_delay_samples_ > 0) {
            const std::size_t buf_size = pre_delay_l_.size();
            const std::size_t read_pos = (pre_delay_write_ + buf_size - pre_delay_samples_) % buf_size;
            pd_l = pre_delay_l_[read_pos];
            pd_r = pre_delay_r_[read_pos];
            pre_delay_l_[pre_delay_write_] = in_l;
            pre_delay_r_[pre_delay_write_] = in_r;
            pre_delay_write_ = (pre_delay_write_ + 1) % buf_size;
        }

        const float mono_in = (pd_l + pd_r) * kFixedGain;

        float out_l = 0.0f;
        float out_r = 0.0f;

        for (std::size_t i = 0; i < kNumCombs; ++i) {
            out_l += process_comb(combs_l_[i], mono_in);
            out_r += process_comb(combs_r_[i], mono_in);
        }

        for (std::size_t i = 0; i < kNumAllpasses; ++i) {
            out_l = process_allpass(allpasses_l_[i], out_l);
            out_r = process_allpass(allpasses_r_[i], out_r);
        }

        const float wet_l = out_l * wet1_ + out_r * wet2_;
        const float wet_r = out_r * wet1_ + out_l * wet2_;

        left = flush_denormal(wet_l + in_l * dry_);
        right = flush_denormal(wet_r + in_r * dry_);
    }

    void process_block(float* left, float* right, std::size_t num_frames) noexcept {
        if (left == nullptr || right == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < num_frames; ++i) {
            process_stereo(left[i], right[i]);
        }
    }

private:
    struct CombFilter {
        std::vector<float> buffer;
        std::size_t index{0};
        float filter_store{0.0f};
    };

    struct AllpassFilter {
        std::vector<float> buffer;
        std::size_t index{0};
    };

    static constexpr float kFixedGain = 0.015f;
    static constexpr float kScaleWet = 3.0f;
    static constexpr float kScaleDamp = 0.4f;
    static constexpr float kScaleRoom = 0.28f;
    static constexpr float kOffsetRoom = 0.7f;

    void update_parameters() noexcept {
        feedback_ = std::clamp(room_size_ * kScaleRoom + kOffsetRoom, 0.0f, 0.985f);
        damp1_ = damping_ * kScaleDamp;
        damp2_ = 1.0f - damp1_;

        const float scaled_wet = wet_ * kScaleWet;
        wet1_ = scaled_wet * (0.5f * width_ + 0.5f);
        wet2_ = scaled_wet * (0.5f * (1.0f - width_));

        if (!pre_delay_l_.empty()) {
            const auto req = static_cast<std::size_t>(
                std::round(static_cast<double>(pre_delay_ms_) * 0.001 * sample_rate_));
            pre_delay_samples_ = std::min(req, pre_delay_l_.size() - 1);
        } else {
            pre_delay_samples_ = 0;
        }
    }

    [[nodiscard]] float process_comb(CombFilter& comb, float input) noexcept {
        if (comb.buffer.empty()) {
            return 0.0f;
        }
        const float output = comb.buffer[comb.index];
        comb.filter_store = flush_denormal(output * damp2_ + comb.filter_store * damp1_);
        comb.buffer[comb.index] = flush_denormal(input + comb.filter_store * feedback_);
        comb.index = (comb.index + 1) % comb.buffer.size();
        return output;
    }

    [[nodiscard]] float process_allpass(AllpassFilter& allpass, float input) noexcept {
        if (allpass.buffer.empty()) {
            return input;
        }
        const float buf_out = allpass.buffer[allpass.index];
        const float output = flush_denormal(-input + buf_out);
        allpass.buffer[allpass.index] = flush_denormal(input + buf_out * 0.5f);
        allpass.index = (allpass.index + 1) % allpass.buffer.size();
        return output;
    }

    double sample_rate_{48000.0};
    float room_size_{0.5f};
    float damping_{0.5f};
    float wet_{0.2f};
    float dry_{1.0f};
    float pre_delay_ms_{0.0f};
    float width_{1.0f};

    float feedback_{0.84f};
    float damp1_{0.2f};
    float damp2_{0.8f};
    float wet1_{0.6f};
    float wet2_{0.0f};
    std::size_t pre_delay_samples_{0};

    std::array<CombFilter, kNumCombs> combs_l_{};
    std::array<CombFilter, kNumCombs> combs_r_{};
    std::array<AllpassFilter, kNumAllpasses> allpasses_l_{};
    std::array<AllpassFilter, kNumAllpasses> allpasses_r_{};

    std::vector<float> pre_delay_l_;
    std::vector<float> pre_delay_r_;
    std::size_t pre_delay_write_{0};
};

} // namespace atriswave::dsp
