#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace atriswave {

/// Allocation-free Cytomic / Andrew Simper Zero-Delay Feedback (ZDF)
/// State Variable Filter with parameter clamping and numerical stability.
class SimperSvf final {
public:
    enum class Mode {
        LowPass,
        HighPass,
        BandPass,
        Peak,
        LowShelf,
        HighShelf
    };

    SimperSvf() noexcept {
        update_coefficients();
    }

    void reset() noexcept {
        ic1eq_ = 0.0;
        ic2eq_ = 0.0;
    }

    void set_parameters(Mode mode,
                        float sample_rate,
                        float cutoff_hz,
                        float q = 0.70710678f,
                        float gain_db = 0.0f) noexcept {
        mode_ = mode;
        sample_rate_ = std::clamp(static_cast<double>(sample_rate), 8000.0, 384000.0);
        const double max_cutoff = 0.49 * sample_rate_;
        cutoff_hz_ = std::clamp(static_cast<double>(cutoff_hz), 10.0, max_cutoff);
        q_ = std::clamp(static_cast<double>(q), 0.05, 40.0);
        gain_db_ = std::clamp(static_cast<double>(gain_db), -48.0, 48.0);
        update_coefficients();
    }

    [[nodiscard]] float process_sample(float input) noexcept {
        if (!std::isfinite(input)) {
            reset();
            return 0.0f;
        }

        const double v0 = static_cast<double>(input);
        const double v3 = v0 - ic2eq_;
        const double v1 = a1_ * ic1eq_ + a2_ * v3;
        const double v2 = ic2eq_ + a2_ * ic1eq_ + a3_ * v3;

        ic1eq_ = 2.0 * v1 - ic1eq_;
        ic2eq_ = 2.0 * v2 - ic2eq_;

        // Guard against any numerical blowup from extreme state values
        if (!std::isfinite(ic1eq_) || !std::isfinite(ic2eq_)) {
            reset();
            return 0.0f;
        }

        const double out = m0_ * v0 + m1_ * v1 + m2_ * v2;
        return static_cast<float>(out);
    }

    void process_block(float* samples, std::size_t count) noexcept {
        if (samples == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            samples[i] = process_sample(samples[i]);
        }
    }

    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] double sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] double cutoff_hz() const noexcept { return cutoff_hz_; }
    [[nodiscard]] double q() const noexcept { return q_; }
    [[nodiscard]] double gain_db() const noexcept { return gain_db_; }

private:
    void update_coefficients() noexcept {
        constexpr double kPi = 3.14159265358979323846;
        const double w = kPi * cutoff_hz_ / sample_rate_;
        const double tan_w = std::tan(w);
        const double A = std::pow(10.0, gain_db_ / 40.0);

        double g = tan_w;
        double k = 1.0 / q_;

        switch (mode_) {
        case Mode::LowPass:
            m0_ = 0.0;
            m1_ = 0.0;
            m2_ = 1.0;
            break;
        case Mode::HighPass:
            m0_ = 1.0;
            m1_ = -k;
            m2_ = -1.0;
            break;
        case Mode::BandPass:
            m0_ = 0.0;
            m1_ = 1.0;
            m2_ = 0.0;
            break;
        case Mode::Peak:
            k = 1.0 / (q_ * A);
            m0_ = 1.0;
            m1_ = k * (A * A - 1.0);
            m2_ = 0.0;
            break;
        case Mode::LowShelf:
            g = tan_w / std::sqrt(A);
            m0_ = 1.0;
            m1_ = k * (A - 1.0);
            m2_ = A * A - 1.0;
            break;
        case Mode::HighShelf:
            g = tan_w * std::sqrt(A);
            m0_ = A * A;
            m1_ = k * (1.0 - A) * A;
            m2_ = 1.0 - A * A;
            break;
        }

        a1_ = 1.0 / (1.0 + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    Mode mode_{Mode::LowPass};
    double sample_rate_{48000.0};
    double cutoff_hz_{1000.0};
    double q_{0.70710678};
    double gain_db_{0.0};

    double a1_{0.0};
    double a2_{0.0};
    double a3_{0.0};
    double m0_{0.0};
    double m1_{0.0};
    double m2_{1.0};

    double ic1eq_{0.0};
    double ic2eq_{0.0};
};

} // namespace atriswave
