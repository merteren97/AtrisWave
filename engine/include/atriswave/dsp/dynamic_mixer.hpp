#pragma once

#include "atriswave/dsp/compressor.hpp"
#include "atriswave/dsp/dsp_math.hpp"
#include "atriswave/dsp/limiter.hpp"
#include "atriswave/dsp/parametric_eq.hpp"
#include "atriswave/ipc_protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace atriswave::dsp {

inline constexpr std::size_t kDynamicMaxChannels = ATRISWAVE_MAX_CHANNELS;
inline constexpr std::size_t kDynamicMaxBuses = 4; // A1=0, A2=1, B1=2, B2=3

struct DynamicChannelMeterAtomic {
    std::atomic<float> peak_left{0.0f};
    std::atomic<float> peak_right{0.0f};
    std::atomic<float> rms_left{0.0f};
    std::atomic<float> rms_right{0.0f};
    std::atomic<float> gain_reduction_db{0.0f};
    std::atomic<std::uint32_t> flags{0u};
};

/// Dynamic 16-channel x 4-bus (A1, A2, B1, B2) real-time mixer.
/// Zero heap allocations and zero locks in `process()` and `fill_telemetry()`.
class DynamicMixer final {
public:
    DynamicMixer() noexcept {
        prepare(48000.0);
    }

    void prepare(double sample_rate) noexcept {
        sample_rate_ = std::clamp(sample_rate, 8000.0, 384000.0);
        for (auto& ch : channels_) {
            ch.gain_smoother.prepare(sample_rate_, 10.0f);
            ch.gain_smoother.reset(1.0f);
            ch.eq_l.prepare(sample_rate_);
            ch.eq_r.prepare(sample_rate_);
            ch.comp_l.prepare(sample_rate_);
            ch.comp_r.prepare(sample_rate_);
            ch.ducker.prepare(sample_rate_);
            ch.ducker.set_params(-32.0f, 10.0f, 6.0f, 15.0f, 250.0f, 0.0f, false);
            ch.last_envelope = 0.0f;
        }
        for (auto& bus : buses_) {
            bus.gain_smoother.prepare(sample_rate_, 10.0f);
            bus.gain_smoother.reset(1.0f);
            bus.safety_limiter.prepare(sample_rate_);
            bus.safety_limiter.set_params(-0.5f, 50.0f, 2.0f);
        }
        env_attack_ = compute_one_pole_coeff(sample_rate_, 5.0f);
        env_release_ = compute_one_pole_coeff(sample_rate_, 150.0f);
    }

    void set_channel_active(std::size_t ch, bool active) noexcept {
        if (ch < kDynamicMaxChannels) {
            channels_[ch].active.store(active, std::memory_order_release);
        }
    }

    void set_channel_mix(std::size_t ch,
                         float volume,
                         bool muted,
                         bool solo,
                         float pan = 0.0f) noexcept {
        if (ch >= kDynamicMaxChannels) {
            return;
        }
        channels_[ch].volume.store(std::clamp(volume, 0.0f, 2.0f), std::memory_order_relaxed);
        channels_[ch].muted.store(muted, std::memory_order_relaxed);
        channels_[ch].solo.store(solo, std::memory_order_relaxed);
        channels_[ch].pan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
    }

    void set_channel_send(std::size_t ch,
                          std::size_t bus,
                          bool enabled,
                          float send_gain = 1.0f) noexcept {
        if (ch >= kDynamicMaxChannels || bus >= kDynamicMaxBuses) {
            return;
        }
        channels_[ch].send_enabled[bus].store(enabled, std::memory_order_relaxed);
        channels_[ch].send_gain[bus].store(std::clamp(send_gain, 0.0f, 2.0f), std::memory_order_relaxed);
    }

    void set_channel_dsp(std::size_t ch,
                         bool eq_enabled,
                         float hpf_hz,
                         float low_db,
                         float mid1_hz,
                         float mid1_db,
                         float mid2_hz,
                         float mid2_db,
                         float high_db,
                         bool comp_enabled,
                         float comp_thresh_db,
                         float comp_ratio,
                         float comp_makeup_db,
                         std::int32_t duck_source_id,
                         float duck_depth_db) noexcept {
        if (ch >= kDynamicMaxChannels) {
            return;
        }
        auto& s = channels_[ch];
        s.eq_enabled.store(eq_enabled, std::memory_order_relaxed);
        s.eq_hpf_hz.store(hpf_hz, std::memory_order_relaxed);
        s.eq_low_db.store(low_db, std::memory_order_relaxed);
        s.eq_mid1_hz.store(mid1_hz, std::memory_order_relaxed);
        s.eq_mid1_db.store(mid1_db, std::memory_order_relaxed);
        s.eq_mid2_hz.store(mid2_hz, std::memory_order_relaxed);
        s.eq_mid2_db.store(mid2_db, std::memory_order_relaxed);
        s.eq_high_db.store(high_db, std::memory_order_relaxed);

        s.comp_enabled.store(comp_enabled, std::memory_order_relaxed);
        s.comp_threshold_db.store(comp_thresh_db, std::memory_order_relaxed);
        s.comp_ratio.store(comp_ratio, std::memory_order_relaxed);
        s.comp_makeup_db.store(comp_makeup_db, std::memory_order_relaxed);

        s.duck_source_id.store(duck_source_id, std::memory_order_relaxed);
        s.duck_depth_db.store(std::clamp(duck_depth_db, -48.0f, 0.0f), std::memory_order_relaxed);
        s.dsp_dirty.fetch_add(1u, std::memory_order_release);
    }

    void set_bus_mix(std::size_t bus, float volume, bool muted) noexcept {
        if (bus >= kDynamicMaxBuses) {
            return;
        }
        buses_[bus].volume.store(std::clamp(volume, 0.0f, 2.0f), std::memory_order_relaxed);
        buses_[bus].muted.store(muted, std::memory_order_relaxed);
    }

    void process(const std::array<const float*, kDynamicMaxChannels>& in_l,
                 const std::array<const float*, kDynamicMaxChannels>& in_r,
                 const std::array<float*, kDynamicMaxBuses>& bus_out_l,
                 const std::array<float*, kDynamicMaxBuses>& bus_out_r,
                 std::size_t num_frames) noexcept {
        if (num_frames == 0) {
            return;
        }

        bool any_solo = false;
        for (std::size_t ch = 0; ch < kDynamicMaxChannels; ++ch) {
            if (channels_[ch].active.load(std::memory_order_acquire) &&
                channels_[ch].solo.load(std::memory_order_relaxed)) {
                any_solo = true;
                break;
            }
        }

        std::array<bool, kDynamicMaxChannels> ch_active{};
        std::array<bool, kDynamicMaxChannels> ch_muted{};
        std::array<float, kDynamicMaxChannels> ch_pan_l{};
        std::array<float, kDynamicMaxChannels> ch_pan_r{};
        std::array<bool, kDynamicMaxChannels> ch_eq_on{};
        std::array<bool, kDynamicMaxChannels> ch_comp_on{};
        std::array<std::int32_t, kDynamicMaxChannels> ch_duck_src{};
        std::array<float, kDynamicMaxChannels> ch_min_duck{};
        std::array<std::array<float, kDynamicMaxBuses>, kDynamicMaxChannels> send_mat{};

        for (std::size_t ch = 0; ch < kDynamicMaxChannels; ++ch) {
            auto& s = channels_[ch];
            const bool act = s.active.load(std::memory_order_acquire);
            ch_active[ch] = act;
            if (!act) {
                continue;
            }

            const std::uint32_t ver = s.dsp_dirty.load(std::memory_order_acquire);
            if (ver != s.applied_dsp_version) {
                s.eq_l.set_params(
                    s.eq_hpf_hz.load(std::memory_order_relaxed),
                    s.eq_low_db.load(std::memory_order_relaxed),
                    s.eq_mid1_hz.load(std::memory_order_relaxed),
                    s.eq_mid1_db.load(std::memory_order_relaxed),
                    s.eq_mid2_hz.load(std::memory_order_relaxed),
                    s.eq_mid2_db.load(std::memory_order_relaxed),
                    s.eq_high_db.load(std::memory_order_relaxed));
                s.eq_r.set_params(
                    s.eq_hpf_hz.load(std::memory_order_relaxed),
                    s.eq_low_db.load(std::memory_order_relaxed),
                    s.eq_mid1_hz.load(std::memory_order_relaxed),
                    s.eq_mid1_db.load(std::memory_order_relaxed),
                    s.eq_mid2_hz.load(std::memory_order_relaxed),
                    s.eq_mid2_db.load(std::memory_order_relaxed),
                    s.eq_high_db.load(std::memory_order_relaxed));

                const float ct = s.comp_threshold_db.load(std::memory_order_relaxed);
                const float cr = s.comp_ratio.load(std::memory_order_relaxed);
                const float cm = s.comp_makeup_db.load(std::memory_order_relaxed);
                s.comp_l.set_params(ct, cr, 6.0f, 10.0f, 120.0f, cm, false);
                s.comp_r.set_params(ct, cr, 6.0f, 10.0f, 120.0f, cm, false);
                s.applied_dsp_version = ver;
            }

            const bool m = s.muted.load(std::memory_order_relaxed);
            const bool sl = s.solo.load(std::memory_order_relaxed);
            const bool eff_mute = m || (any_solo && !sl);
            ch_muted[ch] = eff_mute;
            s.gain_smoother.set_target(eff_mute ? 0.0f : s.volume.load(std::memory_order_relaxed));

            const float pan = s.pan.load(std::memory_order_relaxed);
            ch_pan_l[ch] = std::min(1.0f, 1.0f - pan);
            ch_pan_r[ch] = std::min(1.0f, 1.0f + pan);

            ch_eq_on[ch] = s.eq_enabled.load(std::memory_order_relaxed);
            ch_comp_on[ch] = s.comp_enabled.load(std::memory_order_relaxed);
            ch_duck_src[ch] = s.duck_source_id.load(std::memory_order_relaxed);
            ch_min_duck[ch] = db_to_linear(s.duck_depth_db.load(std::memory_order_relaxed));

            for (std::size_t b = 0; b < kDynamicMaxBuses; ++b) {
                const bool on = s.send_enabled[b].load(std::memory_order_relaxed);
                send_mat[ch][b] = on ? s.send_gain[b].load(std::memory_order_relaxed) : 0.0f;
            }
        }

        std::array<bool, kDynamicMaxBuses> bus_muted{};
        for (std::size_t b = 0; b < kDynamicMaxBuses; ++b) {
            const bool bm = buses_[b].muted.load(std::memory_order_relaxed);
            bus_muted[b] = bm;
            buses_[b].gain_smoother.set_target(bm ? 0.0f : buses_[b].volume.load(std::memory_order_relaxed));
        }

        std::array<float, kDynamicMaxChannels> ch_pk_l{};
        std::array<float, kDynamicMaxChannels> ch_pk_r{};
        std::array<double, kDynamicMaxChannels> ch_sq_l{};
        std::array<double, kDynamicMaxChannels> ch_sq_r{};

        std::array<float, kDynamicMaxBuses> bus_pk_l{};
        std::array<float, kDynamicMaxBuses> bus_pk_r{};
        std::array<double, kDynamicMaxBuses> bus_sq_l{};
        std::array<double, kDynamicMaxBuses> bus_sq_r{};

        for (std::size_t i = 0; i < num_frames; ++i) {
            std::array<float, kDynamicMaxBuses> bus_sum_l{};
            std::array<float, kDynamicMaxBuses> bus_sum_r{};

            for (std::size_t ch = 0; ch < kDynamicMaxChannels; ++ch) {
                if (!ch_active[ch]) {
                    continue;
                }
                auto& s = channels_[ch];

                float l = (in_l[ch] != nullptr && std::isfinite(in_l[ch][i])) ? in_l[ch][i] : 0.0f;
                float r = (in_r[ch] != nullptr && std::isfinite(in_r[ch][i])) ? in_r[ch][i] : l;

                if (ch_eq_on[ch]) {
                    l = s.eq_l.process_sample(l);
                    r = s.eq_r.process_sample(r);
                }
                if (ch_comp_on[ch]) {
                    l = s.comp_l.process_sample(l);
                    r = s.comp_r.process_sample(r);
                }

                // Cross-channel sidechain ducking
                const std::int32_t src_id = ch_duck_src[ch];
                if (src_id >= 0 && static_cast<std::size_t>(src_id) < kDynamicMaxChannels &&
                    static_cast<std::size_t>(src_id) != ch) {
                    const float sc_env = channels_[static_cast<std::size_t>(src_id)].last_envelope;
                    const float duck_gain = std::max(s.ducker.process_with_sidechain(1.0f, sc_env), ch_min_duck[ch]);
                    l *= duck_gain;
                    r *= duck_gain;
                }

                const float g = s.gain_smoother.next();
                l *= g * ch_pan_l[ch];
                r *= g * ch_pan_r[ch];

                const float abs_l = std::abs(l);
                const float abs_r = std::abs(r);
                const float peak = std::max(abs_l, abs_r);
                const float coeff = (peak > s.last_envelope) ? env_attack_ : env_release_;
                s.last_envelope = flush_denormal(s.last_envelope + coeff * (peak - s.last_envelope));

                ch_pk_l[ch] = std::max(ch_pk_l[ch], abs_l);
                ch_pk_r[ch] = std::max(ch_pk_r[ch], abs_r);
                ch_sq_l[ch] += static_cast<double>(l) * l;
                ch_sq_r[ch] += static_cast<double>(r) * r;

                for (std::size_t b = 0; b < kDynamicMaxBuses; ++b) {
                    const float sg = send_mat[ch][b];
                    if (sg > 0.0f) {
                        bus_sum_l[b] += l * sg;
                        bus_sum_r[b] += r * sg;
                    }
                }
            }

            for (std::size_t b = 0; b < kDynamicMaxBuses; ++b) {
                const float bg = buses_[b].gain_smoother.next();
                float bl = bus_sum_l[b] * bg;
                float br = bus_sum_r[b] * bg;
                buses_[b].safety_limiter.process_stereo(bl, br);

                if (bus_out_l[b] != nullptr) bus_out_l[b][i] = bl;
                if (bus_out_r[b] != nullptr) bus_out_r[b][i] = br;

                const float abl = std::abs(bl);
                const float abr = std::abs(br);
                bus_pk_l[b] = std::max(bus_pk_l[b], abl);
                bus_pk_r[b] = std::max(bus_pk_r[b], abr);
                bus_sq_l[b] += static_cast<double>(bl) * bl;
                bus_sq_r[b] += static_cast<double>(br) * br;
            }
        }

        const double inv_n = 1.0 / static_cast<double>(num_frames);
        for (std::size_t ch = 0; ch < kDynamicMaxChannels; ++ch) {
            if (!ch_active[ch]) {
                continue;
            }
            auto& m = channels_[ch].meter;
            m.peak_left.store(ch_pk_l[ch], std::memory_order_relaxed);
            m.peak_right.store(ch_pk_r[ch], std::memory_order_relaxed);
            m.rms_left.store(static_cast<float>(std::sqrt(ch_sq_l[ch] * inv_n)), std::memory_order_relaxed);
            m.rms_right.store(static_cast<float>(std::sqrt(ch_sq_r[ch] * inv_n)), std::memory_order_relaxed);
            const float gr = (ch_duck_src[ch] >= 0) ? channels_[ch].ducker.gain_reduction_db()
                                                    : channels_[ch].comp_l.gain_reduction_db();
            m.gain_reduction_db.store(gr, std::memory_order_relaxed);

            std::uint32_t flags = ATRISWAVE_CHANNEL_FLAG_NONE;
            if (ch_muted[ch]) flags |= ATRISWAVE_CHANNEL_FLAG_MUTED;
            if (ch_pk_l[ch] >= 0.99f || ch_pk_r[ch] >= 0.99f) flags |= ATRISWAVE_CHANNEL_FLAG_CLIPPING;
            if (ch_pk_l[ch] > 1e-5f || ch_pk_r[ch] > 1e-5f) flags |= ATRISWAVE_CHANNEL_FLAG_ACTIVE;
            m.flags.store(flags, std::memory_order_relaxed);
        }

        for (std::size_t b = 0; b < kDynamicMaxBuses; ++b) {
            auto& bm = buses_[b].meter;
            bm.peak_left.store(bus_pk_l[b], std::memory_order_relaxed);
            bm.peak_right.store(bus_pk_r[b], std::memory_order_relaxed);
            bm.rms_left.store(static_cast<float>(std::sqrt(bus_sq_l[b] * inv_n)), std::memory_order_relaxed);
            bm.rms_right.store(static_cast<float>(std::sqrt(bus_sq_r[b] * inv_n)), std::memory_order_relaxed);
            bm.gain_reduction_db.store(buses_[b].safety_limiter.gain_reduction_db(), std::memory_order_relaxed);

            std::uint32_t flags = ATRISWAVE_CHANNEL_FLAG_NONE;
            if (bus_muted[b]) flags |= ATRISWAVE_CHANNEL_FLAG_MUTED;
            if (bus_pk_l[b] >= 0.99f || bus_pk_r[b] >= 0.99f) flags |= ATRISWAVE_CHANNEL_FLAG_CLIPPING;
            if (bus_pk_l[b] > 1e-5f || bus_pk_r[b] > 1e-5f) flags |= ATRISWAVE_CHANNEL_FLAG_ACTIVE;
            bm.flags.store(flags, std::memory_order_relaxed);
        }
    }

    void fill_telemetry(ChannelMeterFrame* ch_out,
                        std::uint32_t& active_ch_count,
                        ChannelMeterFrame* bus_out) const noexcept {
        std::uint32_t count = 0;
        if (ch_out != nullptr) {
            for (std::size_t ch = 0; ch < kDynamicMaxChannels; ++ch) {
                if (!channels_[ch].active.load(std::memory_order_acquire)) {
                    ch_out[ch] = ChannelMeterFrame{};
                    ch_out[ch].channel_id = static_cast<std::uint32_t>(ch);
                    continue;
                }
                ++count;
                const auto& m = channels_[ch].meter;
                ch_out[ch].channel_id = static_cast<std::uint32_t>(ch);
                ch_out[ch].peak_left = m.peak_left.load(std::memory_order_relaxed);
                ch_out[ch].peak_right = m.peak_right.load(std::memory_order_relaxed);
                ch_out[ch].rms_left = m.rms_left.load(std::memory_order_relaxed);
                ch_out[ch].rms_right = m.rms_right.load(std::memory_order_relaxed);
                ch_out[ch].gain_reduction_db = m.gain_reduction_db.load(std::memory_order_relaxed);
                ch_out[ch].flags = m.flags.load(std::memory_order_relaxed);
                ch_out[ch].reserved = 0u;
            }
        }
        active_ch_count = count;

        if (bus_out != nullptr) {
            for (std::size_t b = 0; b < kDynamicMaxBuses; ++b) {
                const auto& bm = buses_[b].meter;
                bus_out[b].channel_id = static_cast<std::uint32_t>(b);
                bus_out[b].peak_left = bm.peak_left.load(std::memory_order_relaxed);
                bus_out[b].peak_right = bm.peak_right.load(std::memory_order_relaxed);
                bus_out[b].rms_left = bm.rms_left.load(std::memory_order_relaxed);
                bus_out[b].rms_right = bm.rms_right.load(std::memory_order_relaxed);
                bus_out[b].gain_reduction_db = bm.gain_reduction_db.load(std::memory_order_relaxed);
                bus_out[b].flags = bm.flags.load(std::memory_order_relaxed);
                bus_out[b].reserved = 0u;
            }
        }
    }

private:
    struct ChannelSlot {
        std::atomic<bool> active{false};
        std::atomic<float> volume{1.0f};
        std::atomic<bool> muted{false};
        std::atomic<bool> solo{false};
        std::atomic<float> pan{0.0f};
        std::array<std::atomic<bool>, kDynamicMaxBuses> send_enabled{};
        std::array<std::atomic<float>, kDynamicMaxBuses> send_gain{};

        std::atomic<bool> eq_enabled{false};
        std::atomic<float> eq_hpf_hz{30.0f};
        std::atomic<float> eq_low_db{0.0f};
        std::atomic<float> eq_mid1_hz{500.0f};
        std::atomic<float> eq_mid1_db{0.0f};
        std::atomic<float> eq_mid2_hz{2500.0f};
        std::atomic<float> eq_mid2_db{0.0f};
        std::atomic<float> eq_high_db{0.0f};

        std::atomic<bool> comp_enabled{false};
        std::atomic<float> comp_threshold_db{-18.0f};
        std::atomic<float> comp_ratio{3.0f};
        std::atomic<float> comp_makeup_db{0.0f};

        std::atomic<std::int32_t> duck_source_id{-1};
        std::atomic<float> duck_depth_db{-12.0f};

        std::atomic<std::uint32_t> dsp_dirty{0u};
        std::uint32_t applied_dsp_version{0u};

        OnePoleSmoother gain_smoother{};
        ParametricEq5Band eq_l{};
        ParametricEq5Band eq_r{};
        DynamicsCompressor comp_l{};
        DynamicsCompressor comp_r{};
        DynamicsCompressor ducker{};
        float last_envelope{0.0f};
        DynamicChannelMeterAtomic meter{};
    };

    struct OutputBusSlot {
        std::atomic<float> volume{1.0f};
        std::atomic<bool> muted{false};
        OnePoleSmoother gain_smoother{};
        LookaheadLimiter safety_limiter{};
        DynamicChannelMeterAtomic meter{};
    };

    double sample_rate_{48000.0};
    float env_attack_{0.01f};
    float env_release_{0.001f};
    std::array<ChannelSlot, kDynamicMaxChannels> channels_{};
    std::array<OutputBusSlot, kDynamicMaxBuses> buses_{};
};

} // namespace atriswave::dsp
