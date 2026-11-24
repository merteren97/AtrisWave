#pragma once

#include "atriswave/dsp/dsp_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace atriswave::dsp {

enum class IrPreset : std::uint32_t {
    Algorithmic = 0,
    StudioBooth = 1,
    BroadcastRoom = 2,
    ConcertHall = 3
};

/// Allocation-free (during process) partitioned overlap-add FFT Convolution Reverb.
/// Uses 512-sample partitions, 1024-point radix-2 FFT, and up to 16 partitions
/// (~170 ms tail @ 48 kHz) with 3 built-in procedural stereo IR presets and custom IR loading.
class ConvolutionReverb final {
public:
    static constexpr std::size_t kPartitionSize = 512;
    static constexpr std::size_t kFftSize = 1024;
    static constexpr std::size_t kMaxPartitions = 16;
    static constexpr std::size_t kMaxIrSamples = kPartitionSize * kMaxPartitions;

    ConvolutionReverb() {
        init_fft_tables();
        allocate_buffers();
        prepare(48000.0);
    }

    void prepare(double sample_rate) {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 192000.0);
        load_preset(preset_);
        reset();
    }

    void reset() noexcept {
        std::fill(in_fifo_l_.begin(), in_fifo_l_.end(), 0.0f);
        std::fill(in_fifo_r_.begin(), in_fifo_r_.end(), 0.0f);
        std::fill(out_fifo_l_.begin(), out_fifo_l_.end(), 0.0f);
        std::fill(out_fifo_r_.begin(), out_fifo_r_.end(), 0.0f);
        std::fill(ola_tail_l_.begin(), ola_tail_l_.end(), 0.0f);
        std::fill(ola_tail_r_.begin(), ola_tail_r_.end(), 0.0f);
        std::fill(fdl_l_re_.begin(), fdl_l_re_.end(), 0.0f);
        std::fill(fdl_l_im_.begin(), fdl_l_im_.end(), 0.0f);
        std::fill(fdl_r_re_.begin(), fdl_r_re_.end(), 0.0f);
        std::fill(fdl_r_im_.begin(), fdl_r_im_.end(), 0.0f);
        fifo_pos_ = 0;
        fdl_head_ = 0;
    }

    void set_mix(float wet, float dry) noexcept {
        wet_ = std::clamp(wet, 0.0f, 1.0f);
        dry_ = std::clamp(dry, 0.0f, 1.0f);
    }

    void set_preset(IrPreset preset) {
        if (preset == preset_ && active_partitions_ > 0) {
            return;
        }
        load_preset(preset);
    }

    [[nodiscard]] IrPreset preset() const noexcept { return preset_; }

    /// Loads a custom stereo impulse response (truncated to kMaxIrSamples).
    /// Call from non-RT thread or during prepare().
    void load_custom_ir(const float* left, const float* right, std::size_t len) {
        if (left == nullptr || len == 0) {
            return;
        }
        const std::size_t clamped_len = std::min(len, kMaxIrSamples);
        active_partitions_ = (clamped_len + kPartitionSize - 1) / kPartitionSize;

        std::fill(ir_l_re_.begin(), ir_l_re_.end(), 0.0f);
        std::fill(ir_l_im_.begin(), ir_l_im_.end(), 0.0f);
        std::fill(ir_r_re_.begin(), ir_r_re_.end(), 0.0f);
        std::fill(ir_r_im_.begin(), ir_r_im_.end(), 0.0f);

        for (std::size_t p = 0; p < active_partitions_; ++p) {
            const std::size_t offset = p * kPartitionSize;
            const std::size_t count = std::min(kPartitionSize, clamped_len - offset);

            // Left partition FFT
            std::fill(work_re_.begin(), work_re_.end(), 0.0f);
            std::fill(work_im_.begin(), work_im_.end(), 0.0f);
            for (std::size_t i = 0; i < count; ++i) {
                work_re_[i] = std::isfinite(left[offset + i]) ? left[offset + i] : 0.0f;
            }
            fft_1024(work_re_.data(), work_im_.data(), false);
            std::copy(work_re_.begin(), work_re_.end(), ir_l_re_.begin() + static_cast<std::ptrdiff_t>(p * kFftSize));
            std::copy(work_im_.begin(), work_im_.end(), ir_l_im_.begin() + static_cast<std::ptrdiff_t>(p * kFftSize));

            // Right partition FFT
            std::fill(work_re_.begin(), work_re_.end(), 0.0f);
            std::fill(work_im_.begin(), work_im_.end(), 0.0f);
            const float* src_r = (right != nullptr) ? right : left;
            for (std::size_t i = 0; i < count; ++i) {
                work_re_[i] = std::isfinite(src_r[offset + i]) ? src_r[offset + i] : 0.0f;
            }
            fft_1024(work_re_.data(), work_im_.data(), false);
            std::copy(work_re_.begin(), work_re_.end(), ir_r_re_.begin() + static_cast<std::ptrdiff_t>(p * kFftSize));
            std::copy(work_im_.begin(), work_im_.end(), ir_r_im_.begin() + static_cast<std::ptrdiff_t>(p * kFftSize));
        }
    }

    void process_stereo(float& left, float& right) noexcept {
        const float in_l = std::isfinite(left) ? left : 0.0f;
        const float in_r = std::isfinite(right) ? right : 0.0f;

        in_fifo_l_[fifo_pos_] = in_l;
        in_fifo_r_[fifo_pos_] = in_r;

        const float conv_l = out_fifo_l_[fifo_pos_];
        const float conv_r = out_fifo_r_[fifo_pos_];
        ++fifo_pos_;

        if (fifo_pos_ >= kPartitionSize) {
            process_partition();
            fifo_pos_ = 0;
        }

        left = flush_denormal(in_l * dry_ + conv_l * wet_);
        right = flush_denormal(in_r * dry_ + conv_r * wet_);
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
    void allocate_buffers() {
        in_fifo_l_.assign(kPartitionSize, 0.0f);
        in_fifo_r_.assign(kPartitionSize, 0.0f);
        out_fifo_l_.assign(kPartitionSize, 0.0f);
        out_fifo_r_.assign(kPartitionSize, 0.0f);
        ola_tail_l_.assign(kPartitionSize, 0.0f);
        ola_tail_r_.assign(kPartitionSize, 0.0f);

        ir_l_re_.assign(kMaxPartitions * kFftSize, 0.0f);
        ir_l_im_.assign(kMaxPartitions * kFftSize, 0.0f);
        ir_r_re_.assign(kMaxPartitions * kFftSize, 0.0f);
        ir_r_im_.assign(kMaxPartitions * kFftSize, 0.0f);

        fdl_l_re_.assign(kMaxPartitions * kFftSize, 0.0f);
        fdl_l_im_.assign(kMaxPartitions * kFftSize, 0.0f);
        fdl_r_re_.assign(kMaxPartitions * kFftSize, 0.0f);
        fdl_r_im_.assign(kMaxPartitions * kFftSize, 0.0f);

        work_re_.assign(kFftSize, 0.0f);
        work_im_.assign(kFftSize, 0.0f);
        accum_re_.assign(kFftSize, 0.0f);
        accum_im_.assign(kFftSize, 0.0f);
        gen_ir_l_.assign(kMaxIrSamples, 0.0f);
        gen_ir_r_.assign(kMaxIrSamples, 0.0f);
    }

    void init_fft_tables() noexcept {
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

    void load_preset(IrPreset preset) {
        preset_ = preset;

        float decay_ms = 65.0f;
        float pre_delay_ms = 2.0f;
        float hf_damp = 0.35f;
        std::uint32_t seed = 0x51A7B007u;

        switch (preset) {
        case IrPreset::StudioBooth:
            decay_ms = 55.0f;
            pre_delay_ms = 1.5f;
            hf_damp = 0.45f;
            seed = 0x13572468u;
            break;
        case IrPreset::BroadcastRoom:
            decay_ms = 105.0f;
            pre_delay_ms = 4.0f;
            hf_damp = 0.30f;
            seed = 0x24681357u;
            break;
        case IrPreset::ConcertHall:
        case IrPreset::Algorithmic:
        default:
            decay_ms = 165.0f;
            pre_delay_ms = 8.0f;
            hf_damp = 0.20f;
            seed = 0x9E3779B9u;
            break;
        }

        std::fill(gen_ir_l_.begin(), gen_ir_l_.end(), 0.0f);
        std::fill(gen_ir_r_.begin(), gen_ir_r_.end(), 0.0f);

        const std::size_t ir_len = std::min<std::size_t>(
            kMaxIrSamples,
            static_cast<std::size_t>(std::round(static_cast<double>(decay_ms) * 0.001 * sample_rate_)));
        const std::size_t pd_samples = static_cast<std::size_t>(
            std::round(static_cast<double>(pre_delay_ms) * 0.001 * sample_rate_));

        auto next_rand = [](std::uint32_t& s) -> float {
            s = s * 1664525u + 1013904223u;
            const float u = static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
            return u * 2.0f - 1.0f;
        };

        std::uint32_t seed_l = seed;
        std::uint32_t seed_r = seed ^ 0xA5A55A5Au;
        float lp_l = 0.0f;
        float lp_r = 0.0f;
        double energy = 0.0;

        for (std::size_t i = pd_samples; i < ir_len; ++i) {
            const float t_norm = static_cast<float>(i - pd_samples) / static_cast<float>(std::max<std::size_t>(1, ir_len - pd_samples));
            const float env = std::exp(-5.5f * t_norm);
            const float nl = next_rand(seed_l) * env;
            const float nr = next_rand(seed_r) * env;
            lp_l = nl + hf_damp * (lp_l - nl);
            lp_r = nr + hf_damp * (lp_r - nr);
            gen_ir_l_[i] = lp_l;
            gen_ir_r_[i] = lp_r;
            energy += 0.5 * (static_cast<double>(lp_l) * lp_l + static_cast<double>(lp_r) * lp_r);
        }

        // Normalize IR energy so wet=1.0 stays well-behaved
        const float norm = (energy > 1e-8) ? static_cast<float>(0.35 / std::sqrt(energy)) : 1.0f;
        for (std::size_t i = 0; i < ir_len; ++i) {
            gen_ir_l_[i] *= norm;
            gen_ir_r_[i] *= norm;
        }
        if (ir_len > 0) {
            gen_ir_l_[0] += 0.25f;
            gen_ir_r_[0] += 0.25f;
        }

        load_custom_ir(gen_ir_l_.data(), gen_ir_r_.data(), std::max<std::size_t>(1, ir_len));
    }

    void process_partition() noexcept {
        // 1. FFT of new 512-sample input block (zero-padded to 1024) for L and R
        process_channel_partition(in_fifo_l_.data(),
                                  fdl_l_re_.data(),
                                  fdl_l_im_.data(),
                                  ir_l_re_.data(),
                                  ir_l_im_.data(),
                                  out_fifo_l_.data(),
                                  ola_tail_l_.data());

        process_channel_partition(in_fifo_r_.data(),
                                  fdl_r_re_.data(),
                                  fdl_r_im_.data(),
                                  ir_r_re_.data(),
                                  ir_r_im_.data(),
                                  out_fifo_r_.data(),
                                  ola_tail_r_.data());

        fdl_head_ = (fdl_head_ + 1) % kMaxPartitions;
    }

    void process_channel_partition(const float* in_block,
                                   float* fdl_re,
                                   float* fdl_im,
                                   const float* ir_re,
                                   const float* ir_im,
                                   float* out_block,
                                   float* ola_tail) noexcept {
        float* head_re = fdl_re + fdl_head_ * kFftSize;
        float* head_im = fdl_im + fdl_head_ * kFftSize;

        for (std::size_t i = 0; i < kPartitionSize; ++i) {
            head_re[i] = in_block[i];
            head_im[i] = 0.0f;
        }
        for (std::size_t i = kPartitionSize; i < kFftSize; ++i) {
            head_re[i] = 0.0f;
            head_im[i] = 0.0f;
        }
        fft_1024(head_re, head_im, false);

        std::fill(accum_re_.begin(), accum_re_.end(), 0.0f);
        std::fill(accum_im_.begin(), accum_im_.end(), 0.0f);

        for (std::size_t p = 0; p < active_partitions_; ++p) {
            const std::size_t slot = (fdl_head_ + kMaxPartitions - p) % kMaxPartitions;
            const float* x_re = fdl_re + slot * kFftSize;
            const float* x_im = fdl_im + slot * kFftSize;
            const float* h_re = ir_re + p * kFftSize;
            const float* h_im = ir_im + p * kFftSize;

            for (std::size_t k = 0; k < kFftSize; ++k) {
                accum_re_[k] += x_re[k] * h_re[k] - x_im[k] * h_im[k];
                accum_im_[k] += x_re[k] * h_im[k] + x_im[k] * h_re[k];
            }
        }

        fft_1024(accum_re_.data(), accum_im_.data(), true);

        constexpr float kInvN = 1.0f / static_cast<float>(kFftSize);
        for (std::size_t i = 0; i < kPartitionSize; ++i) {
            out_block[i] = ola_tail[i] + accum_re_[i] * kInvN;
            ola_tail[i] = accum_re_[i + kPartitionSize] * kInvN;
        }
    }

    void fft_1024(float* real, float* imag, bool inverse) const noexcept {
        for (std::size_t i = 0; i < kFftSize; ++i) {
            const std::size_t j = bit_rev_[i];
            if (i < j) {
                std::swap(real[i], real[j]);
                std::swap(imag[i], imag[j]);
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

                    const float xr = real[idx2];
                    const float xi = imag[idx2];
                    const float tr = wr * xr - wi * xi;
                    const float ti = wr * xi + wi * xr;

                    real[idx2] = real[idx1] - tr;
                    imag[idx2] = imag[idx1] - ti;
                    real[idx1] += tr;
                    imag[idx1] += ti;
                }
            }
        }
    }

    double sample_rate_{48000.0};
    IrPreset preset_{IrPreset::StudioBooth};
    float wet_{0.25f};
    float dry_{0.85f};
    std::size_t active_partitions_{1};
    std::size_t fifo_pos_{0};
    std::size_t fdl_head_{0};

    std::array<std::uint16_t, kFftSize> bit_rev_{};
    std::array<float, kFftSize / 2> twiddle_cos_{};
    std::array<float, kFftSize / 2> twiddle_sin_{};

    std::vector<float> in_fifo_l_;
    std::vector<float> in_fifo_r_;
    std::vector<float> out_fifo_l_;
    std::vector<float> out_fifo_r_;
    std::vector<float> ola_tail_l_;
    std::vector<float> ola_tail_r_;

    std::vector<float> ir_l_re_;
    std::vector<float> ir_l_im_;
    std::vector<float> ir_r_re_;
    std::vector<float> ir_r_im_;

    std::vector<float> fdl_l_re_;
    std::vector<float> fdl_l_im_;
    std::vector<float> fdl_r_re_;
    std::vector<float> fdl_r_im_;

    std::vector<float> work_re_;
    std::vector<float> work_im_;
    std::vector<float> accum_re_;
    std::vector<float> accum_im_;
    std::vector<float> gen_ir_l_;
    std::vector<float> gen_ir_r_;
};

} // namespace atriswave::dsp
