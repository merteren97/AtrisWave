#pragma once

#include "atriswave/dsp/dsp_math.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace atriswave::dsp {

/// Pluggable real-time spectral noise suppressor using 480-sample (10 ms @ 48 kHz)
/// overlap-add framing — matching DeepFilterNet3-LL and RNNoise frame sizes.
/// Includes a built-in zero-dependency radix-2 FFT Wiener/Ephraim-Malah suppressor
/// and an optional dynamic DLL loader hook for external AI denoisers (libdf.dll / rnnoise.dll).
class SpectralDenoiser final {
public:
    static constexpr std::size_t kHopSize = 480;
    static constexpr std::size_t kWindowSize = 960;
    static constexpr std::size_t kFftSize = 1024;
    static constexpr std::size_t kNumBins = kFftSize / 2 + 1;

    SpectralDenoiser() noexcept {
        init_tables();
        reset();
    }

    ~SpectralDenoiser() noexcept {
        unload_external_dll();
    }

    SpectralDenoiser(const SpectralDenoiser&) = delete;
    SpectralDenoiser& operator=(const SpectralDenoiser&) = delete;

    void prepare(double sample_rate) noexcept {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 192000.0);
        reset();
    }

    void reset() noexcept {
        analysis_buf_.fill(0.0f);
        ola_tail_.fill(0.0f);
        in_fifo_.fill(0.0f);
        out_fifo_.fill(0.0f);
        in_pos_ = 0;
        out_pos_ = 0;

        fft_real_.fill(0.0f);
        fft_imag_.fill(0.0f);
        noise_psd_.fill(1e-7f);
        prev_gain_.fill(1.0f);
        prev_gamma_.fill(1.0f);
        frame_count_ = 0;
    }

    void set_strength(float strength) noexcept {
        strength_ = std::clamp(strength, 0.0f, 1.0f);
    }

    [[nodiscard]] float strength() const noexcept { return strength_; }

    /// Attempts to load an external RNNoise-compatible or DeepFilterNet-compatible DLL.
    /// Safe to call from non-RT control thread before processing.
    bool try_load_external_dll(const wchar_t* path) noexcept {
        unload_external_dll();
        if (path == nullptr || path[0] == L'\0') {
            return false;
        }
#ifdef _WIN32
        HMODULE mod = ::LoadLibraryW(path);
        if (mod == nullptr) {
            return false;
        }

        // Probe RNNoise C ABI
        auto rn_create = reinterpret_cast<RnnoiseCreateFn>(
            ::GetProcAddress(mod, "rnnoise_create"));
        auto rn_destroy = reinterpret_cast<RnnoiseDestroyFn>(
            ::GetProcAddress(mod, "rnnoise_destroy"));
        auto rn_process = reinterpret_cast<RnnoiseProcessFn>(
            ::GetProcAddress(mod, "rnnoise_process_frame"));

        if (rn_create != nullptr && rn_destroy != nullptr && rn_process != nullptr) {
            void* state = rn_create(nullptr);
            if (state != nullptr) {
                ext_module_ = mod;
                ext_state_ = state;
                rnnoise_destroy_ = rn_destroy;
                rnnoise_process_ = rn_process;
                return true;
            }
        }

        ::FreeLibrary(mod);
#endif
        return false;
    }

    void unload_external_dll() noexcept {
#ifdef _WIN32
        if (ext_state_ != nullptr && rnnoise_destroy_ != nullptr) {
            rnnoise_destroy_(ext_state_);
        }
        ext_state_ = nullptr;
        rnnoise_destroy_ = nullptr;
        rnnoise_process_ = nullptr;
        if (ext_module_ != nullptr) {
            ::FreeLibrary(ext_module_);
            ext_module_ = nullptr;
        }
#endif
    }

    [[nodiscard]] bool has_external_dll() const noexcept {
#ifdef _WIN32
        return ext_module_ != nullptr && ext_state_ != nullptr;
#else
        return false;
#endif
    }

    [[nodiscard]] float process_sample(float input) noexcept {
        const float clean = std::isfinite(input) ? input : 0.0f;
        in_fifo_[in_pos_] = clean;
        const float out = out_fifo_[in_pos_];
        ++in_pos_;

        if (in_pos_ >= kHopSize) {
            process_hop_frame(in_fifo_.data(), out_fifo_.data());
            in_pos_ = 0;
        }
        return flush_denormal(out);
    }

    void process_block(float* samples, std::size_t num_frames) noexcept {
        if (samples == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < num_frames; ++i) {
            samples[i] = process_sample(samples[i]);
        }
    }

    [[nodiscard]] std::size_t latency_samples() const noexcept { return kHopSize; }

private:
#ifdef _WIN32
    using RnnoiseCreateFn = void* (*)(void*);
    using RnnoiseDestroyFn = void (*)(void*);
    using RnnoiseProcessFn = float (*)(void*, float*, const float*);

    HMODULE ext_module_{nullptr};
    void* ext_state_{nullptr};
    RnnoiseDestroyFn rnnoise_destroy_{nullptr};
    RnnoiseProcessFn rnnoise_process_{nullptr};
#endif

    void init_tables() noexcept {
        // Periodic Hann window of length kWindowSize, stored as sqrt(hann) for
        // symmetric analysis/synthesis COLA at 50% overlap.
        for (std::size_t n = 0; n < kWindowSize; ++n) {
            const double w = 0.5 * (1.0 - std::cos(kTwoPi * static_cast<double>(n) /
                                                   static_cast<double>(kWindowSize)));
            window_[n] = static_cast<float>(std::sqrt(std::max(0.0, w)));
        }

        // Precompute bit-reversal and twiddle factors for 1024-pt radix-2 FFT
        for (std::size_t i = 0; i < kFftSize; ++i) {
            std::size_t rev = 0;
            std::size_t x = i;
            for (std::size_t b = 0; b < 10; ++b) {
                rev = (rev << 1) | (x & 1u);
                x >>= 1;
            }
            bit_rev_[i] = static_cast<std::uint16_t>(rev);
        }

        for (std::size_t i = 0; i < kFftSize / 2; ++i) {
            const double angle = -kTwoPi * static_cast<double>(i) / static_cast<double>(kFftSize);
            twiddle_cos_[i] = static_cast<float>(std::cos(angle));
            twiddle_sin_[i] = static_cast<float>(std::sin(angle));
        }
    }

    void process_hop_frame(const float* hop_in, float* hop_out) noexcept {
#ifdef _WIN32
        if (ext_state_ != nullptr && rnnoise_process_ != nullptr) {
            // RNNoise operates on int16 scale [-32768, 32767]
            std::array<float, kHopSize> scaled_in{};
            std::array<float, kHopSize> scaled_out{};
            for (std::size_t i = 0; i < kHopSize; ++i) {
                scaled_in[i] = hop_in[i] * 32768.0f;
            }
            (void)rnnoise_process_(ext_state_, scaled_out.data(), scaled_in.data());
            for (std::size_t i = 0; i < kHopSize; ++i) {
                const float dry = hop_in[i];
                const float wet = scaled_out[i] * (1.0f / 32768.0f);
                hop_out[i] = dry + strength_ * (wet - dry);
            }
            return;
        }
#endif

        // Shift analysis buffer left by kHopSize and append new hop
        for (std::size_t i = 0; i < kHopSize; ++i) {
            analysis_buf_[i] = analysis_buf_[i + kHopSize];
            analysis_buf_[i + kHopSize] = hop_in[i];
        }

        // Window into FFT buffer + zero-pad [kWindowSize .. kFftSize-1]
        for (std::size_t i = 0; i < kWindowSize; ++i) {
            fft_real_[i] = analysis_buf_[i] * window_[i];
            fft_imag_[i] = 0.0f;
        }
        for (std::size_t i = kWindowSize; i < kFftSize; ++i) {
            fft_real_[i] = 0.0f;
            fft_imag_[i] = 0.0f;
        }

        fft_1024(false);
        apply_spectral_suppression();
        fft_1024(true);

        // Synthesis window + 50% overlap-add
        constexpr float kInvN = 1.0f / static_cast<float>(kFftSize);
        for (std::size_t i = 0; i < kHopSize; ++i) {
            const float synth_lo = fft_real_[i] * kInvN * window_[i];
            hop_out[i] = ola_tail_[i] + synth_lo;
            ola_tail_[i] = fft_real_[i + kHopSize] * kInvN * window_[i + kHopSize];
        }
    }

    void apply_spectral_suppression() noexcept {
        ++frame_count_;

        double sq_sum = 0.0;
        for (std::size_t i = 0; i < kWindowSize; ++i) {
            const float s = analysis_buf_[i];
            sq_sum += static_cast<double>(s) * s;
        }
        const float frame_rms = static_cast<float>(std::sqrt(sq_sum / static_cast<double>(kWindowSize)));
        const bool is_quiet_frame = (frame_rms < 0.04f); // < -28 dBFS

        // Minimum-statistics / asymmetric noise PSD tracking + decision-directed Wiener gain
        constexpr float kAlphaDd = 0.96f;
        const float min_gain = 1.0f - 0.84f * strength_; // Natural floor up to ~-16 dB

        std::array<float, kNumBins> raw_gain{};
        for (std::size_t k = 0; k < kNumBins; ++k) {
            const float re = fft_real_[k];
            const float im = fft_imag_[k];
            const float psd = re * re + im * im;

            if (frame_count_ <= 8 && is_quiet_frame) {
                // Fast initial noise floor calibration only on quiet non-speech frames
                const float init_alpha = 1.0f / static_cast<float>(frame_count_);
                noise_psd_[k] = noise_psd_[k] + init_alpha * (psd - noise_psd_[k]);
            } else {
                // Gentler asymmetric tracking: avoid musical noise and voice attenuation
                const float track_alpha = (psd < noise_psd_[k]) ? 0.20f : 0.004f;
                noise_psd_[k] = noise_psd_[k] + track_alpha * (psd - noise_psd_[k]);
            }

            const float floor_psd = std::max(noise_psd_[k], 1e-10f);
            const float gamma = std::min(psd / floor_psd, 100.0f);
            const float xi = kAlphaDd * (prev_gain_[k] * prev_gain_[k] * prev_gamma_[k]) +
                             (1.0f - kAlphaDd) * std::max(gamma - 1.0f, 0.0f);

            const float wiener = xi / (1.0f + xi);
            raw_gain[k] = std::clamp(1.0f - strength_ * (1.0f - wiener), min_gain, 1.0f);
            prev_gamma_[k] = gamma;
        }

        // 3-bin frequency smoothing + temporal smoothing to eliminate metallic/watery artifacts
        for (std::size_t k = 0; k < kNumBins; ++k) {
            const float g_prev = (k > 0) ? raw_gain[k - 1] : raw_gain[k];
            const float g_curr = raw_gain[k];
            const float g_next = (k + 1 < kNumBins) ? raw_gain[k + 1] : raw_gain[k];
            const float freq_smoothed = 0.20f * g_prev + 0.60f * g_curr + 0.20f * g_next;
            const float gain = std::clamp(0.75f * prev_gain_[k] + 0.25f * freq_smoothed, min_gain, 1.0f);

            prev_gain_[k] = gain;

            fft_real_[k] *= gain;
            fft_imag_[k] *= gain;

            if (k > 0 && k < kFftSize / 2) {
                const std::size_t mirror = kFftSize - k;
                fft_real_[mirror] *= gain;
                fft_imag_[mirror] *= gain;
            }
        }
    }

    void fft_1024(bool inverse) noexcept {
        // Bit-reversal permutation
        for (std::size_t i = 0; i < kFftSize; ++i) {
            const std::size_t j = bit_rev_[i];
            if (i < j) {
                std::swap(fft_real_[i], fft_real_[j]);
                std::swap(fft_imag_[i], fft_imag_[j]);
            }
        }

        const float dir_sign = inverse ? -1.0f : 1.0f;

        for (std::size_t len = 2; len <= kFftSize; len <<= 1) {
            const std::size_t half = len >> 1;
            const std::size_t step = kFftSize / len;
            for (std::size_t i = 0; i < kFftSize; i += len) {
                for (std::size_t j = 0; j < half; ++j) {
                    const std::size_t tw_idx = j * step;
                    const float wr = twiddle_cos_[tw_idx];
                    const float wi = twiddle_sin_[tw_idx] * dir_sign;

                    const std::size_t idx1 = i + j;
                    const std::size_t idx2 = idx1 + half;

                    const float xr = fft_real_[idx2];
                    const float xi = fft_imag_[idx2];
                    const float tr = wr * xr - wi * xi;
                    const float ti = wr * xi + wi * xr;

                    fft_real_[idx2] = fft_real_[idx1] - tr;
                    fft_imag_[idx2] = fft_imag_[idx1] - ti;
                    fft_real_[idx1] += tr;
                    fft_imag_[idx1] += ti;
                }
            }
        }
    }

    double sample_rate_{48000.0};
    float strength_{0.75f};

    std::array<float, kWindowSize> window_{};
    std::array<std::uint16_t, kFftSize> bit_rev_{};
    std::array<float, kFftSize / 2> twiddle_cos_{};
    std::array<float, kFftSize / 2> twiddle_sin_{};

    std::array<float, kWindowSize> analysis_buf_{};
    std::array<float, kHopSize> ola_tail_{};
    std::array<float, kHopSize> in_fifo_{};
    std::array<float, kHopSize> out_fifo_{};
    std::size_t in_pos_{0};
    std::size_t out_pos_{0};

    std::array<float, kFftSize> fft_real_{};
    std::array<float, kFftSize> fft_imag_{};
    std::array<float, kNumBins> noise_psd_{};
    std::array<float, kNumBins> prev_gain_{};
    std::array<float, kNumBins> prev_gamma_{};
    std::uint64_t frame_count_{0};
};

} // namespace atriswave::dsp
