#pragma once

#include "atriswave/dsp/dsp_math.hpp"
#include "atriswave/simper_svf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>

namespace atriswave::dsp {

struct Eq5BandParams {
    bool hpf_enabled{true};
    float hpf_hz{80.0f};
    float hpf_q{0.70710678f};

    float low_hz{120.0f};
    float low_db{0.0f};
    float low_q{0.70710678f};

    float mid1_hz{500.0f};
    float mid1_db{0.0f};
    float mid1_q{1.0f};

    float mid2_hz{2500.0f};
    float mid2_db{0.0f};
    float mid2_q{1.0f};

    float high_hz{8000.0f};
    float high_db{0.0f};
    float high_q{0.70710678f};
};

/// 5-Band Parametric EQ built on Cytomic/Simper ZDF State Variable Filters:
/// Band 0: HighPass
/// Band 1: LowShelf (default 120 Hz)
/// Band 2: Peak Mid1
/// Band 3: Peak Mid2
/// Band 4: HighShelf (default 8 kHz)
class ParametricEq5Band final {
public:
    ParametricEq5Band() noexcept {
        prepare(48000.0);
    }

    void prepare(double sample_rate) noexcept {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 384000.0);
        update_filters();
        reset();
    }

    void reset() noexcept {
        for (auto& band : left_) {
            band.reset();
        }
        for (auto& band : right_) {
            band.reset();
        }
    }

    void set_params(const Eq5BandParams& params) noexcept {
        params_ = params;
        update_filters();
    }

    void set_params(float hpf_hz,
                    float low_db,
                    float mid1_hz,
                    float mid1_db,
                    float mid2_hz,
                    float mid2_db,
                    float high_db,
                    float mid1_q = 1.0f,
                    float mid2_q = 1.0f,
                    float low_hz = 120.0f,
                    float high_hz = 8000.0f,
                    bool hpf_enabled = true) noexcept {
        params_.hpf_enabled = hpf_enabled;
        params_.hpf_hz = hpf_hz;
        params_.low_hz = low_hz;
        params_.low_db = low_db;
        params_.mid1_hz = mid1_hz;
        params_.mid1_db = mid1_db;
        params_.mid1_q = mid1_q;
        params_.mid2_hz = mid2_hz;
        params_.mid2_db = mid2_db;
        params_.mid2_q = mid2_q;
        params_.high_hz = high_hz;
        params_.high_db = high_db;
        update_filters();
    }

    [[nodiscard]] float process_sample(float input) noexcept {
        float x = std::isfinite(input) ? input : 0.0f;
        if (params_.hpf_enabled) {
            x = left_[0].process_sample(x);
        }
        for (std::size_t i = 1; i < 5; ++i) {
            x = left_[i].process_sample(x);
        }
        return flush_denormal(x);
    }

    void process_stereo(float& left, float& right) noexcept {
        float l = std::isfinite(left) ? left : 0.0f;
        float r = std::isfinite(right) ? right : 0.0f;
        if (params_.hpf_enabled) {
            l = left_[0].process_sample(l);
            r = right_[0].process_sample(r);
        }
        for (std::size_t i = 1; i < 5; ++i) {
            l = left_[i].process_sample(l);
            r = right_[i].process_sample(r);
        }
        left = flush_denormal(l);
        right = flush_denormal(r);
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

    /// Analytical combined magnitude response in dB at the given frequency,
    /// matching the exact bilinear-transformed Simper SVF transfer functions.
    [[nodiscard]] double magnitude_db(double freq_hz) const noexcept {
        if (freq_hz <= 0.0 || freq_hz >= 0.5 * sample_rate_) {
            return -120.0;
        }
        const double tan_f = std::tan(kPi * freq_hz / sample_rate_);
        double total_db = 0.0;

        if (params_.hpf_enabled) {
            total_db += band_magnitude_db(left_[0], tan_f);
        }
        for (std::size_t i = 1; i < 5; ++i) {
            total_db += band_magnitude_db(left_[i], tan_f);
        }
        return total_db;
    }

    [[nodiscard]] const Eq5BandParams& params() const noexcept { return params_; }

private:
    void update_filters() noexcept {
        const float sr = static_cast<float>(sample_rate_);
        left_[0].set_parameters(SimperSvf::Mode::HighPass, sr, params_.hpf_hz, params_.hpf_q, 0.0f);
        left_[1].set_parameters(SimperSvf::Mode::LowShelf, sr, params_.low_hz, params_.low_q, params_.low_db);
        left_[2].set_parameters(SimperSvf::Mode::Peak, sr, params_.mid1_hz, params_.mid1_q, params_.mid1_db);
        left_[3].set_parameters(SimperSvf::Mode::Peak, sr, params_.mid2_hz, params_.mid2_q, params_.mid2_db);
        left_[4].set_parameters(SimperSvf::Mode::HighShelf, sr, params_.high_hz, params_.high_q, params_.high_db);

        for (std::size_t i = 0; i < 5; ++i) {
            right_[i].set_parameters(left_[i].mode(),
                                     sr,
                                     static_cast<float>(left_[i].cutoff_hz()),
                                     static_cast<float>(left_[i].q()),
                                     static_cast<float>(left_[i].gain_db()));
        }
    }

    [[nodiscard]] static double band_magnitude_db(const SimperSvf& svf, double tan_f) noexcept {
        const double sr = svf.sample_rate();
        const double w = kPi * svf.cutoff_hz() / sr;
        const double tan_w = std::tan(w);
        const double A = std::pow(10.0, svf.gain_db() / 40.0);

        double g = tan_w;
        double k = 1.0 / svf.q();
        double m0 = 0.0;
        double m1 = 0.0;
        double m2 = 1.0;

        switch (svf.mode()) {
        case SimperSvf::Mode::LowPass:
            m0 = 0.0;
            m1 = 0.0;
            m2 = 1.0;
            break;
        case SimperSvf::Mode::HighPass:
            m0 = 1.0;
            m1 = -k;
            m2 = -1.0;
            break;
        case SimperSvf::Mode::BandPass:
            m0 = 0.0;
            m1 = 1.0;
            m2 = 0.0;
            break;
        case SimperSvf::Mode::Peak:
            k = 1.0 / (svf.q() * A);
            m0 = 1.0;
            m1 = k * (A * A - 1.0);
            m2 = 0.0;
            break;
        case SimperSvf::Mode::LowShelf:
            g = tan_w / std::sqrt(A);
            m0 = 1.0;
            m1 = k * (A - 1.0);
            m2 = A * A - 1.0;
            break;
        case SimperSvf::Mode::HighShelf:
            g = tan_w * std::sqrt(A);
            m0 = A * A;
            m1 = k * (1.0 - A) * A;
            m2 = 1.0 - A * A;
            break;
        }

        // Bilinear transform s = j * tan_f
        const double u2 = -(tan_f * tan_f);
        const double g2 = g * g;
        const std::complex<double> denom(u2 + g2, g * k * tan_f);
        const std::complex<double> numer =
            m0 * denom + std::complex<double>(m2 * g2, m1 * g * tan_f);

        const double mag = std::abs(numer / denom);
        return 20.0 * std::log10(std::max(mag, 1e-12));
    }

    double sample_rate_{48000.0};
    Eq5BandParams params_{};
    std::array<SimperSvf, 5> left_{};
    std::array<SimperSvf, 5> right_{};
};

} // namespace atriswave::dsp
