#pragma once

#include "atriswave/dsp/compressor.hpp"
#include "atriswave/dsp/denoiser.hpp"
#include "atriswave/dsp/dsp_math.hpp"
#include "atriswave/dsp/limiter.hpp"
#include "atriswave/dsp/noise_gate.hpp"
#include "atriswave/dsp/parametric_eq.hpp"
#include "atriswave/dsp/reverb.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace atriswave::dsp {

/// Plain snapshot of all Microphone DSP parameters.
struct MicDspParams {
    float pre_gain_db{0.0f};
    bool direct_monitor{false};
    float monitor_gain{1.0f};

    bool denoise_enabled{true};
    float denoise_strength{0.75f};

    bool gate_enabled{true};
    float gate_threshold_db{-40.0f};
    float gate_close_threshold_db{-46.0f};

    bool eq_enabled{true};
    float eq_hpf_hz{80.0f};
    float eq_low_db{0.0f};
    float eq_mid1_hz{500.0f};
    float eq_mid1_db{0.0f};
    float eq_mid2_hz{2500.0f};
    float eq_mid2_db{0.0f};
    float eq_high_db{0.0f};

    bool comp_enabled{true};
    float comp_threshold_db{-18.0f};
    float comp_ratio{4.0f};
    float comp_makeup_db{2.0f};

    bool ducking_enabled{false};
    float ducking_depth_db{-12.0f};

    bool reverb_enabled{false};
    float reverb_wet{0.18f};
    float reverb_room{0.45f};
    float reverb_damp{0.55f};

    bool limiter_enabled{true};
    float limiter_ceiling_db{-1.0f};
};

/// Lock-free atomic parameter bridge between control thread and RT audio thread.
struct MicDspParamsAtomic {
    std::atomic<float> pre_gain_db{0.0f};
    std::atomic<bool> direct_monitor{false};
    std::atomic<float> monitor_gain{1.0f};

    std::atomic<bool> denoise_enabled{true};
    std::atomic<float> denoise_strength{0.75f};

    std::atomic<bool> gate_enabled{true};
    std::atomic<float> gate_threshold_db{-40.0f};
    std::atomic<float> gate_close_threshold_db{-46.0f};

    std::atomic<bool> eq_enabled{true};
    std::atomic<float> eq_hpf_hz{80.0f};
    std::atomic<float> eq_low_db{0.0f};
    std::atomic<float> eq_mid1_hz{500.0f};
    std::atomic<float> eq_mid1_db{0.0f};
    std::atomic<float> eq_mid2_hz{2500.0f};
    std::atomic<float> eq_mid2_db{0.0f};
    std::atomic<float> eq_high_db{0.0f};

    std::atomic<bool> comp_enabled{true};
    std::atomic<float> comp_threshold_db{-18.0f};
    std::atomic<float> comp_ratio{4.0f};
    std::atomic<float> comp_makeup_db{2.0f};

    std::atomic<bool> ducking_enabled{false};
    std::atomic<float> ducking_depth_db{-12.0f};

    std::atomic<bool> reverb_enabled{false};
    std::atomic<float> reverb_wet{0.18f};
    std::atomic<float> reverb_room{0.45f};
    std::atomic<float> reverb_damp{0.55f};

    std::atomic<bool> limiter_enabled{true};
    std::atomic<float> limiter_ceiling_db{-1.0f};

    std::atomic<std::uint64_t> version{1};

    void store(const MicDspParams& p) noexcept {
        pre_gain_db.store(p.pre_gain_db, std::memory_order_relaxed);
        direct_monitor.store(p.direct_monitor, std::memory_order_relaxed);
        monitor_gain.store(p.monitor_gain, std::memory_order_relaxed);
        denoise_enabled.store(p.denoise_enabled, std::memory_order_relaxed);
        denoise_strength.store(p.denoise_strength, std::memory_order_relaxed);
        gate_enabled.store(p.gate_enabled, std::memory_order_relaxed);
        gate_threshold_db.store(p.gate_threshold_db, std::memory_order_relaxed);
        gate_close_threshold_db.store(p.gate_close_threshold_db, std::memory_order_relaxed);
        eq_enabled.store(p.eq_enabled, std::memory_order_relaxed);
        eq_hpf_hz.store(p.eq_hpf_hz, std::memory_order_relaxed);
        eq_low_db.store(p.eq_low_db, std::memory_order_relaxed);
        eq_mid1_hz.store(p.eq_mid1_hz, std::memory_order_relaxed);
        eq_mid1_db.store(p.eq_mid1_db, std::memory_order_relaxed);
        eq_mid2_hz.store(p.eq_mid2_hz, std::memory_order_relaxed);
        eq_mid2_db.store(p.eq_mid2_db, std::memory_order_relaxed);
        eq_high_db.store(p.eq_high_db, std::memory_order_relaxed);
        comp_enabled.store(p.comp_enabled, std::memory_order_relaxed);
        comp_threshold_db.store(p.comp_threshold_db, std::memory_order_relaxed);
        comp_ratio.store(p.comp_ratio, std::memory_order_relaxed);
        comp_makeup_db.store(p.comp_makeup_db, std::memory_order_relaxed);
        ducking_enabled.store(p.ducking_enabled, std::memory_order_relaxed);
        ducking_depth_db.store(p.ducking_depth_db, std::memory_order_relaxed);
        reverb_enabled.store(p.reverb_enabled, std::memory_order_relaxed);
        reverb_wet.store(p.reverb_wet, std::memory_order_relaxed);
        reverb_room.store(p.reverb_room, std::memory_order_relaxed);
        reverb_damp.store(p.reverb_damp, std::memory_order_relaxed);
        limiter_enabled.store(p.limiter_enabled, std::memory_order_relaxed);
        limiter_ceiling_db.store(p.limiter_ceiling_db, std::memory_order_relaxed);
        version.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] MicDspParams load() const noexcept {
        MicDspParams p;
        p.pre_gain_db = pre_gain_db.load(std::memory_order_relaxed);
        p.direct_monitor = direct_monitor.load(std::memory_order_relaxed);
        p.monitor_gain = monitor_gain.load(std::memory_order_relaxed);
        p.denoise_enabled = denoise_enabled.load(std::memory_order_relaxed);
        p.denoise_strength = denoise_strength.load(std::memory_order_relaxed);
        p.gate_enabled = gate_enabled.load(std::memory_order_relaxed);
        p.gate_threshold_db = gate_threshold_db.load(std::memory_order_relaxed);
        p.gate_close_threshold_db = gate_close_threshold_db.load(std::memory_order_relaxed);
        p.eq_enabled = eq_enabled.load(std::memory_order_relaxed);
        p.eq_hpf_hz = eq_hpf_hz.load(std::memory_order_relaxed);
        p.eq_low_db = eq_low_db.load(std::memory_order_relaxed);
        p.eq_mid1_hz = eq_mid1_hz.load(std::memory_order_relaxed);
        p.eq_mid1_db = eq_mid1_db.load(std::memory_order_relaxed);
        p.eq_mid2_hz = eq_mid2_hz.load(std::memory_order_relaxed);
        p.eq_mid2_db = eq_mid2_db.load(std::memory_order_relaxed);
        p.eq_high_db = eq_high_db.load(std::memory_order_relaxed);
        p.comp_enabled = comp_enabled.load(std::memory_order_relaxed);
        p.comp_threshold_db = comp_threshold_db.load(std::memory_order_relaxed);
        p.comp_ratio = comp_ratio.load(std::memory_order_relaxed);
        p.comp_makeup_db = comp_makeup_db.load(std::memory_order_relaxed);
        p.ducking_enabled = ducking_enabled.load(std::memory_order_relaxed);
        p.ducking_depth_db = ducking_depth_db.load(std::memory_order_relaxed);
        p.reverb_enabled = reverb_enabled.load(std::memory_order_relaxed);
        p.reverb_wet = reverb_wet.load(std::memory_order_relaxed);
        p.reverb_room = reverb_room.load(std::memory_order_relaxed);
        p.reverb_damp = reverb_damp.load(std::memory_order_relaxed);
        p.limiter_enabled = limiter_enabled.load(std::memory_order_relaxed);
        p.limiter_ceiling_db = limiter_ceiling_db.load(std::memory_order_relaxed);
        return p;
    }
};

/// Dual-Path Microphone DSP Graph:
/// 1) Direct Monitor Path (low-latency sidetone): Pre-Gain -> EQ -> zero-lookahead Comp
///    (bypasses SpectralDenoiser, NoiseGate lookahead, StereoReverb, and LookaheadLimiter).
/// 2) AI Broadcast Path: Pre-Gain -> SpectralDenoiser -> NoiseGate -> ParametricEq5Band
///    -> DynamicsCompressor -> StereoReverb -> LookaheadLimiter.
class MicPipeline final {
public:
    MicPipeline() {
        prepare(48000.0, 1024);
    }

    void prepare(double sample_rate, std::size_t max_block_size = 1024) {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 192000.0);
        pre_gain_smoother_.prepare(sample_rate_, 10.0f);
        monitor_gain_smoother_.prepare(sample_rate_, 10.0f);

        denoiser_.prepare(sample_rate_);
        gate_.prepare(sample_rate_, max_block_size);
        broadcast_eq_.prepare(sample_rate_);
        monitor_eq_.prepare(sample_rate_);
        broadcast_comp_.prepare(sample_rate_);
        monitor_comp_.prepare(sample_rate_);
        reverb_.prepare(sample_rate_);
        limiter_.prepare(sample_rate_);

        env_attack_coeff_ = compute_one_pole_coeff(sample_rate_, 5.0f);
        env_release_coeff_ = compute_one_pole_coeff(sample_rate_, 150.0f);

        reset();
    }

    void reset() noexcept {
        pre_gain_smoother_.reset(1.0f);
        monitor_gain_smoother_.reset(1.0f);
        denoiser_.reset();
        gate_.reset();
        broadcast_eq_.reset();
        monitor_eq_.reset();
        broadcast_comp_.reset();
        monitor_comp_.reset();
        reverb_.reset();
        limiter_.reset();

        peak_ = 0.0f;
        rms_ = 0.0f;
        gain_reduction_db_ = 0.0f;
        mic_envelope_ = 0.0f;
    }

    void set_params(const MicDspParams& params) noexcept {
        atomic_params_.store(params);
    }

    [[nodiscard]] MicDspParamsAtomic& atomic_params() noexcept { return atomic_params_; }
    [[nodiscard]] const MicDspParamsAtomic& atomic_params() const noexcept { return atomic_params_; }

    void process(const float* mic_in,
                 float* monitor_out_l,
                 float* monitor_out_r,
                 float* broadcast_out_l,
                 float* broadcast_out_r,
                 std::size_t num_frames) noexcept {
        if (mic_in == nullptr || num_frames == 0) {
            return;
        }

        const std::uint64_t ver = atomic_params_.version.load(std::memory_order_acquire);
        if (ver != applied_version_) {
            cached_params_ = atomic_params_.load();
            apply_params(cached_params_);
            applied_version_ = ver;
        }
        const MicDspParams& p = cached_params_;

        float block_peak = 0.0f;
        double block_sq_sum = 0.0;

        for (std::size_t i = 0; i < num_frames; ++i) {
            const float raw = std::isfinite(mic_in[i]) ? mic_in[i] : 0.0f;
            const float pre = raw * pre_gain_smoother_.next();

            // 1. Direct Monitor Path (zero denoiser/reverb/lookahead latency)
            const float mon_gain = monitor_gain_smoother_.next();
            if (p.direct_monitor) {
                float mon = pre;
                if (p.eq_enabled) {
                    mon = monitor_eq_.process_sample(mon);
                }
                if (p.comp_enabled) {
                    mon = monitor_comp_.process_sample(mon);
                }
                mon *= mon_gain;
                if (monitor_out_l != nullptr) {
                    monitor_out_l[i] = mon;
                }
                if (monitor_out_r != nullptr) {
                    monitor_out_r[i] = mon;
                }
            } else {
                if (monitor_out_l != nullptr) {
                    monitor_out_l[i] = 0.0f;
                }
                if (monitor_out_r != nullptr) {
                    monitor_out_r[i] = 0.0f;
                }
            }

            // 2. AI Broadcast Path
            float bc_l = pre;
            if (p.denoise_enabled) {
                bc_l = denoiser_.process_sample(bc_l);
            }
            if (p.gate_enabled) {
                bc_l = gate_.process_sample(bc_l);
            }
            if (p.eq_enabled) {
                bc_l = broadcast_eq_.process_sample(bc_l);
            }
            if (p.comp_enabled) {
                bc_l = broadcast_comp_.process_sample(bc_l);
            }

            float bc_r = bc_l;
            if (p.reverb_enabled) {
                reverb_.process_stereo(bc_l, bc_r);
            }
            if (p.limiter_enabled) {
                limiter_.process_stereo(bc_l, bc_r);
            }

            if (broadcast_out_l != nullptr) {
                broadcast_out_l[i] = bc_l;
            }
            if (broadcast_out_r != nullptr) {
                broadcast_out_r[i] = bc_r;
            }

            // Telemetry & sidechain envelope
            const float abs_bc = std::max(std::abs(bc_l), std::abs(bc_r));
            block_peak = std::max(block_peak, abs_bc);
            block_sq_sum += 0.5 * (static_cast<double>(bc_l) * bc_l + static_cast<double>(bc_r) * bc_r);

            const float env_coeff = (abs_bc > mic_envelope_) ? env_attack_coeff_ : env_release_coeff_;
            mic_envelope_ = flush_denormal(abs_bc + env_coeff * (mic_envelope_ - abs_bc));
        }

        peak_ = block_peak;
        rms_ = static_cast<float>(std::sqrt(block_sq_sum / static_cast<double>(num_frames)));
        gain_reduction_db_ = (p.comp_enabled ? broadcast_comp_.gain_reduction_db() : 0.0f) +
                             (p.limiter_enabled ? limiter_.gain_reduction_db() : 0.0f);
    }

    [[nodiscard]] float peak() const noexcept { return peak_; }
    [[nodiscard]] float rms() const noexcept { return rms_; }
    [[nodiscard]] float gain_reduction_db() const noexcept { return gain_reduction_db_; }
    [[nodiscard]] float mic_envelope() const noexcept { return mic_envelope_; }
    [[nodiscard]] SpectralDenoiser& denoiser() noexcept { return denoiser_; }

private:
    void apply_params(const MicDspParams& p) noexcept {
        pre_gain_smoother_.set_target(db_to_linear(p.pre_gain_db));
        monitor_gain_smoother_.set_target(std::clamp(p.monitor_gain, 0.0f, 2.0f));

        denoiser_.set_strength(p.denoise_strength);
        gate_.set_params(p.gate_threshold_db,
                         p.gate_close_threshold_db,
                         2.0f,
                         50.0f,
                         120.0f,
                         -80.0f,
                         1.5f);

        broadcast_eq_.set_params(p.eq_hpf_hz,
                                 p.eq_low_db,
                                 p.eq_mid1_hz,
                                 p.eq_mid1_db,
                                 p.eq_mid2_hz,
                                 p.eq_mid2_db,
                                 p.eq_high_db);
        monitor_eq_.set_params(p.eq_hpf_hz,
                               p.eq_low_db,
                               p.eq_mid1_hz,
                               p.eq_mid1_db,
                               p.eq_mid2_hz,
                               p.eq_mid2_db,
                               p.eq_high_db);

        broadcast_comp_.set_params(p.comp_threshold_db, p.comp_ratio, 6.0f, 5.0f, 80.0f, p.comp_makeup_db);
        monitor_comp_.set_params(p.comp_threshold_db, p.comp_ratio, 6.0f, 5.0f, 80.0f, p.comp_makeup_db);

        reverb_.set_params(p.reverb_room, p.reverb_damp, p.reverb_wet, 1.0f - p.reverb_wet * 0.5f, 10.0f);
        limiter_.set_params(p.limiter_ceiling_db, 50.0f, 2.5f);
    }

    double sample_rate_{48000.0};
    MicDspParamsAtomic atomic_params_{};
    MicDspParams cached_params_{};
    std::uint64_t applied_version_{0};

    OnePoleSmoother pre_gain_smoother_{};
    OnePoleSmoother monitor_gain_smoother_{};

    SpectralDenoiser denoiser_{};
    NoiseGate gate_{};
    ParametricEq5Band broadcast_eq_{};
    ParametricEq5Band monitor_eq_{};
    DynamicsCompressor broadcast_comp_{};
    DynamicsCompressor monitor_comp_{};
    StereoReverb reverb_{};
    LookaheadLimiter limiter_{};

    float env_attack_coeff_{0.0f};
    float env_release_coeff_{0.0f};
    float peak_{0.0f};
    float rms_{0.0f};
    float gain_reduction_db_{0.0f};
    float mic_envelope_{0.0f};
};

} // namespace atriswave::dsp
