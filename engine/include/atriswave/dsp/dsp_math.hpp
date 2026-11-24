#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace atriswave::dsp {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 6.28318530717958647692;
inline constexpr float kMinDb = -120.0f;
inline constexpr float kDenormalThreshold = 1e-25f;

/// Flushes denormal or non-finite floats to zero.
[[nodiscard]] inline float flush_denormal(float value) noexcept {
    if (!std::isfinite(value) || std::abs(value) < kDenormalThreshold) {
        return 0.0f;
    }
    return value;
}

/// Flushes denormal or non-finite doubles to zero.
[[nodiscard]] inline double flush_denormal(double value) noexcept {
    if (!std::isfinite(value) || std::abs(value) < 1e-30) {
        return 0.0;
    }
    return value;
}

/// Converts decibels to linear amplitude, clamping <= kMinDb to 0.0f.
[[nodiscard]] inline float db_to_linear(float db) noexcept {
    if (!std::isfinite(db)) {
        return 0.0f;
    }
    if (db <= kMinDb) {
        return 0.0f;
    }
    const float clamped = std::clamp(db, kMinDb, 48.0f);
    // exp(db * ln(10) / 20)
    constexpr float kDbToLn = 0.1151292546497022842f;
    return std::exp(clamped * kDbToLn);
}

/// Converts linear amplitude to decibels, floored at kMinDb.
[[nodiscard]] inline float linear_to_db(float linear) noexcept {
    if (!std::isfinite(linear)) {
        return kMinDb;
    }
    const float abs_val = std::abs(linear);
    if (abs_val <= 1e-6f) {
        return kMinDb;
    }
    return std::max(kMinDb, 20.0f * std::log10(abs_val));
}

/// Computes a one-pole smoothing coefficient for a given time constant in ms.
[[nodiscard]] inline float compute_one_pole_coeff(double sample_rate, float time_ms) noexcept {
    if (time_ms <= 0.0f || sample_rate <= 0.0) {
        return 0.0f;
    }
    const double time_sec = static_cast<double>(time_ms) * 0.001;
    return static_cast<float>(std::exp(-1.0 / (time_sec * sample_rate)));
}

/// Click-free one-pole exponential parameter smoother.
class OnePoleSmoother final {
public:
    OnePoleSmoother() noexcept = default;

    void prepare(double sample_rate, float time_ms = 10.0f) noexcept {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 384000.0);
        set_time_ms(time_ms);
    }

    void set_time_ms(float time_ms) noexcept {
        time_ms_ = std::max(0.0f, time_ms);
        coeff_ = compute_one_pole_coeff(sample_rate_, time_ms_);
    }

    void reset(float value = 0.0f) noexcept {
        const float clean = std::isfinite(value) ? value : 0.0f;
        current_ = clean;
        target_ = clean;
    }

    void set_target(float target) noexcept {
        if (std::isfinite(target)) {
            target_ = target;
        }
    }

    [[nodiscard]] float next() noexcept {
        if (coeff_ <= 0.0f) {
            current_ = target_;
            return current_;
        }
        current_ = target_ + coeff_ * (current_ - target_);
        if (std::abs(current_ - target_) < 1e-7f) {
            current_ = target_;
        }
        return current_;
    }

    [[nodiscard]] float current() const noexcept { return current_; }
    [[nodiscard]] float target() const noexcept { return target_; }
    [[nodiscard]] bool is_smoothing() const noexcept { return current_ != target_; }

private:
    double sample_rate_{48000.0};
    float time_ms_{10.0f};
    float coeff_{0.0f};
    float current_{0.0f};
    float target_{0.0f};
};

} // namespace atriswave::dsp
