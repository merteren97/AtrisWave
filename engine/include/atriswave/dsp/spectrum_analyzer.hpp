#pragma once

#include <atriswave/dsp/dsp_math.hpp>
#include <atriswave/ipc_protocol.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace atriswave::dsp {

/// Allocation-free 1024-pt Hann-windowed radix-2 FFT 32-band log-frequency
/// spectrum analyzer (30 Hz .. 18 kHz) with smooth attack/decay ballistics.
class SpectrumAnalyzer final {
public:
    static constexpr std::size_t kFftSize = 1024;
    static constexpr std::size_t kHopSize = 512;
    static constexpr std::size_t kBins = ATRISWAVE_SPECTRUM_BINS; // 32
    static constexpr float kMinDb = -90.0f;
    static constexpr float kMaxDb = 0.0f;

    SpectrumAnalyzer() noexcept {
        prepare(48000.0);
    }

    void prepare(double sample_rate) noexcept {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 384000.0);
        for (std::size_t i = 0; i < kFftSize; ++i) {
            window_[i] = static_cast<float>(
                0.5 * (1.0 - std::cos(static_cast<double>(kTwoPi) * static_cast<double>(i) / static_cast<double>(kFftSize - 1))));
        }

        // Log-spaced band edges from 30 Hz to 18 kHz
        constexpr double f_min = 30.0;
        const double f_max = std::min(18000.0, 0.48 * sample_rate_);
        const double ratio = std::pow(f_max / f_min, 1.0 / static_cast<double>(kBins));
        const double bin_hz = sample_rate_ / static_cast<double>(kFftSize);

        for (std::size_t b = 0; b < kBins; ++b) {
            const double f0 = f_min * std::pow(ratio, static_cast<double>(b));
            const double f1 = f_min * std::pow(ratio, static_cast<double>(b + 1));
            std::size_t k0 = static_cast<std::size_t>(std::floor(f0 / bin_hz));
            std::size_t k1 = static_cast<std::size_t>(std::ceil(f1 / bin_hz));
            k0 = std::clamp<std::size_t>(k0, 1, kFftSize / 2 - 1);
            k1 = std::clamp<std::size_t>(std::max(k1, k0 + 1), k0 + 1, kFftSize / 2);
            band_start_[b] = k0;
            band_end_[b] = k1;
        }
        reset();
    }

    void reset() noexcept {
        ring_.fill(0.0f);
        ring_pos_ = 0;
        hop_counter_ = 0;
        smoothed_db_.fill(kMinDb);
        for (auto& b : bins_db_) {
            b.store(kMinDb, std::memory_order_relaxed);
        }
    }

    void push_block(const float* left, const float* right, std::size_t frames) noexcept {
        if (left == nullptr || frames == 0) {
            return;
        }
        for (std::size_t i = 0; i < frames; ++i) {
            const float l = std::isfinite(left[i]) ? left[i] : 0.0f;
            const float r = (right != nullptr && std::isfinite(right[i])) ? right[i] : l;
            ring_[ring_pos_] = 0.5f * (l + r);
            ring_pos_ = (ring_pos_ + 1) & (kFftSize - 1);
            if (++hop_counter_ >= kHopSize) {
                hop_counter_ = 0;
                compute_fft();
            }
        }
    }

    void read_bins(float* out_bins, std::size_t count = kBins) const noexcept {
        if (out_bins == nullptr) {
            return;
        }
        const std::size_t n = std::min(count, kBins);
        for (std::size_t i = 0; i < n; ++i) {
            out_bins[i] = bins_db_[i].load(std::memory_order_relaxed);
        }
    }

private:
    void compute_fft() noexcept {
        for (std::size_t i = 0; i < kFftSize; ++i) {
            const std::size_t idx = (ring_pos_ + i) & (kFftSize - 1);
            fft_re_[i] = ring_[idx] * window_[i];
            fft_im_[i] = 0.0f;
        }

        // Bit-reversal permutation
        for (std::size_t i = 1, j = 0; i < kFftSize; ++i) {
            std::size_t bit = kFftSize >> 1;
            for (; (j & bit) != 0; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) {
                std::swap(fft_re_[i], fft_re_[j]);
                std::swap(fft_im_[i], fft_im_[j]);
            }
        }

        // Radix-2 Cooley-Tukey FFT
        for (std::size_t len = 2; len <= kFftSize; len <<= 1) {
            const double ang = -2.0 * static_cast<double>(kPi) / static_cast<double>(len);
            const float wlen_re = static_cast<float>(std::cos(ang));
            const float wlen_im = static_cast<float>(std::sin(ang));
            for (std::size_t i = 0; i < kFftSize; i += len) {
                float w_re = 1.0f;
                float w_im = 0.0f;
                const std::size_t half = len >> 1;
                for (std::size_t j = 0; j < half; ++j) {
                    const std::size_t u = i + j;
                    const std::size_t v = u + half;
                    const float t_re = fft_re_[v] * w_re - fft_im_[v] * w_im;
                    const float t_im = fft_re_[v] * w_im + fft_im_[v] * w_re;
                    fft_re_[v] = fft_re_[u] - t_re;
                    fft_im_[v] = fft_im_[u] - t_im;
                    fft_re_[u] += t_re;
                    fft_im_[u] += t_im;
                    const float next_w_re = w_re * wlen_re - w_im * wlen_im;
                    w_im = w_re * wlen_im + w_im * wlen_re;
                    w_re = next_w_re;
                }
            }
        }

        // Coherent Hann window normalization: 2 / (N * 0.5) = 4 / N
        constexpr float kNorm = 4.0f / static_cast<float>(kFftSize);
        for (std::size_t b = 0; b < kBins; ++b) {
            float peak_mag = 0.0f;
            for (std::size_t k = band_start_[b]; k < band_end_[b]; ++k) {
                const float mag = std::sqrt(fft_re_[k] * fft_re_[k] + fft_im_[k] * fft_im_[k]) * kNorm;
                if (mag > peak_mag) {
                    peak_mag = mag;
                }
            }
            float db = (peak_mag > 1e-6f) ? (20.0f * std::log10(peak_mag)) : kMinDb;
            if (!std::isfinite(db)) {
                db = kMinDb;
            }
            db = std::clamp(db, kMinDb, kMaxDb);

            const float coeff = (db > smoothed_db_[b]) ? 0.65f : 0.20f;
            smoothed_db_[b] = smoothed_db_[b] + coeff * (db - smoothed_db_[b]);
            bins_db_[b].store(smoothed_db_[b], std::memory_order_relaxed);
        }
    }

    double sample_rate_{48000.0};
    std::array<float, kFftSize> window_{};
    std::array<float, kFftSize> ring_{};
    std::array<float, kFftSize> fft_re_{};
    std::array<float, kFftSize> fft_im_{};
    std::size_t ring_pos_{0};
    std::size_t hop_counter_{0};

    std::array<std::size_t, kBins> band_start_{};
    std::array<std::size_t, kBins> band_end_{};
    std::array<float, kBins> smoothed_db_{};
    std::array<std::atomic<float>, kBins> bins_db_{};
};

} // namespace atriswave::dsp
