#include "atriswave/dsp/compressor.hpp"
#include "atriswave/dsp/convolution_reverb.hpp"
#include "atriswave/dsp/denoiser.hpp"
#include "atriswave/dsp/dsp_math.hpp"
#include "atriswave/dsp/limiter.hpp"
#include "atriswave/dsp/mic_pipeline.hpp"
#include "atriswave/dsp/mixer_bus.hpp"
#include "atriswave/dsp/noise_gate.hpp"
#include "atriswave/dsp/parametric_eq.hpp"
#include "atriswave/dsp/pitch_shifter.hpp"
#include "atriswave/dsp/pll_resampler.hpp"
#include "atriswave/dsp/reverb.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* expr, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, expr);
        ++g_failures;
    }
}

#define ATRIS_CHECK(cond) check((cond), #cond, __LINE__)

float compute_rms(const std::vector<float>& buf, std::size_t start = 0) {
    if (start >= buf.size()) {
        return 0.0f;
    }
    double sum = 0.0;
    const std::size_t n = buf.size() - start;
    for (std::size_t i = start; i < buf.size(); ++i) {
        sum += static_cast<double>(buf[i]) * static_cast<double>(buf[i]);
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(n)));
}

std::vector<float> make_sine(std::size_t frames, double freq_hz, double sample_rate, float amp) {
    std::vector<float> out(frames, 0.0f);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = amp * static_cast<float>(std::sin(
                           atriswave::dsp::kTwoPi * freq_hz * static_cast<double>(i) / sample_rate));
    }
    return out;
}

void test_dsp_math() {
    using namespace atriswave::dsp;
    ATRIS_CHECK(std::abs(db_to_linear(0.0f) - 1.0f) < 1e-5f);
    ATRIS_CHECK(std::abs(db_to_linear(-6.0206f) - 0.5f) < 1e-3f);
    ATRIS_CHECK(db_to_linear(-140.0f) == 0.0f);
    ATRIS_CHECK(std::abs(linear_to_db(0.5f) - (-6.0206f)) < 1e-2f);
    ATRIS_CHECK(linear_to_db(0.0f) <= kMinDb);
    ATRIS_CHECK(flush_denormal(1e-30f) == 0.0f);
    ATRIS_CHECK(flush_denormal(0.5f) == 0.5f);

    OnePoleSmoother smoother;
    smoother.prepare(48000.0, 5.0f);
    smoother.reset(0.0f);
    smoother.set_target(1.0f);
    float last = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        last = smoother.next();
    }
    ATRIS_CHECK(std::abs(last - 1.0f) < 1e-3f);
}

void test_noise_gate() {
    using namespace atriswave::dsp;
    constexpr double kSr = 48000.0;
    NoiseGate gate;
    gate.prepare(kSr);
    gate.set_params(-38.0f, -44.0f, 1.0f, 20.0f, 50.0f, -80.0f, 1.5f);

    // -60 dBFS noise should be silenced
    const float noise_amp = db_to_linear(-60.0f);
    auto low_sig = make_sine(9600, 1000.0, kSr, noise_amp);
    gate.process_block(low_sig.data(), low_sig.size());
    const float out_low_rms = compute_rms(low_sig, 4800);
    ATRIS_CHECK(out_low_rms < noise_amp * 0.05f);
    ATRIS_CHECK(gate.gain_reduction_db() > 40.0f);

    // -10 dBFS signal should pass cleanly
    const float high_amp = db_to_linear(-10.0f);
    auto high_sig = make_sine(9600, 1000.0, kSr, high_amp);
    const float in_high_rms = compute_rms(high_sig, 4800);
    gate.process_block(high_sig.data(), high_sig.size());
    const float out_high_rms = compute_rms(high_sig, 4800);
    ATRIS_CHECK(std::abs(out_high_rms - in_high_rms) < in_high_rms * 0.05f);
    ATRIS_CHECK(gate.gain_reduction_db() < 0.5f);
}

void test_parametric_eq() {
    using namespace atriswave::dsp;
    constexpr double kSr = 48000.0;
    ParametricEq5Band eq;
    eq.prepare(kSr);

    // Flat 0 dB passthrough (HPF disabled)
    eq.set_params(80.0f, 0.0f, 500.0f, 0.0f, 2500.0f, 0.0f, 0.0f, 1.0f, 1.0f, 120.0f, 8000.0f, false);
    ATRIS_CHECK(std::abs(eq.magnitude_db(1000.0)) < 0.01);

    auto flat_in = make_sine(4800, 1000.0, kSr, 0.5f);
    const float flat_in_rms = compute_rms(flat_in, 2400);
    eq.process_block(flat_in.data(), flat_in.size());
    const float flat_out_rms = compute_rms(flat_in, 2400);
    ATRIS_CHECK(std::abs(flat_out_rms - flat_in_rms) < 1e-4f);

    // +6 dB peak at 1000 Hz
    eq.reset();
    eq.set_params(80.0f, 0.0f, 1000.0f, 6.0f, 2500.0f, 0.0f, 0.0f, 2.0f, 1.0f, 120.0f, 8000.0f, false);
    ATRIS_CHECK(std::abs(eq.magnitude_db(1000.0) - 6.0) < 0.1);

    auto peak_sig = make_sine(9600, 1000.0, kSr, 0.25f);
    const float peak_in_rms = compute_rms(peak_sig, 4800);
    eq.process_block(peak_sig.data(), peak_sig.size());
    const float peak_out_rms = compute_rms(peak_sig, 4800);
    const float measured_gain_db = linear_to_db(peak_out_rms / peak_in_rms);
    ATRIS_CHECK(std::abs(measured_gain_db - 6.0f) < 0.25f);

    // 100 Hz HPF attenuates 20 Hz significantly
    eq.reset();
    eq.set_params(100.0f, 0.0f, 1000.0f, 0.0f, 2500.0f, 0.0f, 0.0f, 1.0f, 1.0f, 120.0f, 8000.0f, true);
    ATRIS_CHECK(eq.magnitude_db(20.0) < -20.0);

    auto sub_sig = make_sine(24000, 20.0, kSr, 0.5f);
    const float sub_in_rms = compute_rms(sub_sig, 12000);
    eq.process_block(sub_sig.data(), sub_sig.size());
    const float sub_out_rms = compute_rms(sub_sig, 12000);
    ATRIS_CHECK(linear_to_db(sub_out_rms / sub_in_rms) < -20.0f);
}

void test_dynamics_compressor() {
    using namespace atriswave::dsp;
    constexpr double kSr = 48000.0;
    DynamicsCompressor comp;
    comp.prepare(kSr);
    comp.set_params(-18.0f, 4.0f, 0.0f, 2.0f, 50.0f, 0.0f, false);

    // -6 dBFS sine above -18 dB threshold at 4:1 ratio -> 12 dB over -> 9 dB gain reduction
    const float amp = db_to_linear(-6.0f);
    auto sig = make_sine(24000, 1000.0, kSr, amp);
    comp.process_block(sig.data(), sig.size());
    const float gr = comp.gain_reduction_db();
    ATRIS_CHECK(gr > 7.5f && gr < 10.5f);

    // Sidechain ducking: music attenuated when mic sidechain is present
    DynamicsCompressor ducker;
    ducker.prepare(kSr);
    ducker.set_params(-30.0f, 8.0f, 4.0f, 5.0f, 100.0f, 0.0f, false);

    auto music = make_sine(9600, 440.0, kSr, 0.5f);
    auto mic_sc = make_sine(9600, 300.0, kSr, 0.4f);
    for (std::size_t i = 0; i < music.size(); ++i) {
        music[i] = ducker.process_with_sidechain(music[i], mic_sc[i]);
    }
    const float ducked_rms = compute_rms(music, 4800);
    ATRIS_CHECK(ducked_rms < 0.25f);
    ATRIS_CHECK(ducker.gain_reduction_db() > 6.0f);
}

void test_stereo_reverb() {
    using namespace atriswave::dsp;
    StereoReverb reverb;
    reverb.prepare(48000.0);
    reverb.set_params(0.7f, 0.5f, 0.5f, 0.5f, 5.0f, 1.0f);

    constexpr std::size_t kFrames = 48000;
    std::vector<float> left(kFrames, 0.0f);
    std::vector<float> right(kFrames, 0.0f);
    left[0] = 1.0f;
    right[0] = 1.0f;

    reverb.process_block(left.data(), right.data(), kFrames);

    bool all_finite = true;
    for (std::size_t i = 0; i < kFrames; ++i) {
        if (!std::isfinite(left[i]) || !std::isfinite(right[i])) {
            all_finite = false;
            break;
        }
    }
    ATRIS_CHECK(all_finite);

    const float early_rms = compute_rms(std::vector<float>(left.begin() + 1000, left.begin() + 6000));
    const float late_rms = compute_rms(std::vector<float>(left.begin() + 36000, left.begin() + 48000));
    ATRIS_CHECK(early_rms > 0.005f);
    ATRIS_CHECK(late_rms < early_rms * 0.5f);
}

void test_lookahead_limiter() {
    using namespace atriswave::dsp;
    constexpr double kSr = 48000.0;
    LookaheadLimiter limiter;
    limiter.prepare(kSr);
    limiter.set_params(-1.0f, 50.0f, 2.5f);

    const float hot_amp = db_to_linear(6.0f); // +6 dBFS
    auto left = make_sine(9600, 1000.0, kSr, hot_amp);
    auto right = make_sine(9600, 1000.0, kSr, hot_amp);
    limiter.process_block(left.data(), right.data(), left.size());

    const float ceiling = limiter.ceiling_linear();
    float max_abs = 0.0f;
    for (std::size_t i = 0; i < left.size(); ++i) {
        max_abs = std::max(max_abs, std::max(std::abs(left[i]), std::abs(right[i])));
    }
    ATRIS_CHECK(max_abs <= ceiling + 1e-4f);
    ATRIS_CHECK(limiter.gain_reduction_db() > 5.0f);
}

void test_spectral_denoiser() {
    using namespace atriswave::dsp;
    constexpr double kSr = 48000.0;
    SpectralDenoiser denoiser;
    denoiser.prepare(kSr);
    denoiser.set_strength(1.0f);

    // Deterministic LCG white noise for stationary noise floor calibration
    std::uint32_t rng = 0x12345678u;
    auto next_noise = [&rng]() -> float {
        rng = rng * 1664525u + 1013904223u;
        const float u = static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
        return (u * 2.0f - 1.0f) * 0.05f;
    };

    // Feed 15 frames (7200 samples) of stationary noise
    std::vector<float> noise_only(7200);
    for (float& s : noise_only) {
        s = next_noise();
    }
    const float noise_in_rms = compute_rms(noise_only, 4800);
    denoiser.process_block(noise_only.data(), noise_only.size());
    const float noise_out_rms = compute_rms(noise_only, 4800);
    ATRIS_CHECK(noise_out_rms < noise_in_rms * 0.45f);

    // Feed speech-band 1 kHz sine + noise and verify tone is preserved
    std::vector<float> tone_plus_noise(9600);
    for (std::size_t i = 0; i < tone_plus_noise.size(); ++i) {
        tone_plus_noise[i] =
            0.35f * static_cast<float>(std::sin(kTwoPi * 1000.0 * static_cast<double>(i) / kSr)) +
            next_noise() * 0.25f;
    }
    denoiser.process_block(tone_plus_noise.data(), tone_plus_noise.size());
    const float tone_out_rms = compute_rms(tone_plus_noise, 4800);
    ATRIS_CHECK(tone_out_rms > 0.15f);
}

void test_pll_resampler() {
    using namespace atriswave::dsp;
    constexpr double kSr = 48000.0;

    for (double drift : {0.0005, -0.0005}) { // +500 ppm and -500 ppm
        AdaptivePllResampler resampler;
        resampler.prepare(kSr, kSr);
        resampler.update_pll(drift > 0 ? 0.8 : 0.2, 0.5);
        resampler.set_ratio_offset(drift);

        constexpr std::size_t kInFrames = 9600;
        auto in = make_sine(kInFrames, 440.0, kSr, 0.8f);
        std::vector<float> out(kInFrames + 256, 0.0f);

        std::size_t total_out = 0;
        std::size_t in_offset = 0;
        while (in_offset < kInFrames) {
            const std::size_t chunk_in = std::min<std::size_t>(480, kInFrames - in_offset);
            std::size_t produced = 0;
            const std::size_t consumed = resampler.process_mono(
                in.data() + in_offset, chunk_in, out.data() + total_out, 512, produced);
            in_offset += (consumed > 0) ? consumed : chunk_in;
            total_out += produced;
        }

        ATRIS_CHECK(total_out > 9000);

        // Verify waveform continuity (no clicks/spikes between samples)
        float max_diff = 0.0f;
        for (std::size_t i = 100; i + 1 < total_out; ++i) {
            max_diff = std::max(max_diff, std::abs(out[i + 1] - out[i]));
        }
        // 440 Hz @ 48 kHz max sample-to-sample delta is ~0.046
        ATRIS_CHECK(max_diff < 0.08f);
    }
}

void test_mic_pipeline_and_mixer_bus() {
    using namespace atriswave::dsp;
    constexpr double kSr = 48000.0;
    constexpr std::size_t kFrames = 2400;

    MicPipeline mic;
    mic.prepare(kSr, kFrames);
    MicDspParams params;
    params.direct_monitor = true;
    params.monitor_gain = 1.0f;
    params.denoise_enabled = true;
    params.denoise_strength = 0.8f;
    params.gate_enabled = false;
    params.eq_enabled = true;
    params.comp_enabled = true;
    params.reverb_enabled = true;
    params.reverb_wet = 0.3f;
    params.limiter_enabled = true;
    mic.set_params(params);

    // Impulse at sample 0: direct monitor path must respond immediately (no 480-sample denoiser latency!)
    std::vector<float> mic_in(kFrames, 0.0f);
    mic_in[0] = 0.5f;
    std::vector<float> mon_l(kFrames, 0.0f), mon_r(kFrames, 0.0f);
    std::vector<float> bc_l(kFrames, 0.0f), bc_r(kFrames, 0.0f);
    mic.process(mic_in.data(), mon_l.data(), mon_r.data(), bc_l.data(), bc_r.data(), kFrames);

    ATRIS_CHECK(std::abs(mon_l[0]) > 0.1f);
    // Broadcast path has 480-sample denoiser latency so sample 0 is still silent
    ATRIS_CHECK(std::abs(bc_l[0]) < 1e-4f);

    // Now test MixerBus solo/mute, sidechain ducking, and telemetry
    MixerBus mixer;
    mixer.prepare(kSr);
    mixer.set_channel(ChannelId::Master, 1.0f, false, false);
    mixer.set_channel(ChannelId::Game, 1.0f, false, false);
    mixer.set_channel(ChannelId::Media, 1.0f, false, false);
    mixer.set_channel(ChannelId::Mic, 1.0f, false, false);
    mixer.set_ducking(true, -12.0f, -30.0f);

    auto game_sig = make_sine(kFrames, 220.0, kSr, 0.5f);
    auto mic_sig = make_sine(kFrames, 500.0, kSr, 0.4f);

    std::array<const float*, kMixerChannelCount> in_l = {
        nullptr, game_sig.data(), nullptr, nullptr, nullptr, mic_sig.data()
    };
    std::array<const float*, kMixerChannelCount> in_r = {
        nullptr, game_sig.data(), nullptr, nullptr, nullptr, mic_sig.data()
    };
    std::vector<float> out_l(kFrames, 0.0f), out_r(kFrames, 0.0f);

    mixer.process(in_l, in_r, out_l.data(), out_r.data(), kFrames);
    ATRIS_CHECK(mixer.meter(ChannelId::Game).gain_reduction_db > 5.0f);

    // Solo Game -> Mic should be muted
    mixer.set_channel(ChannelId::Game, 1.0f, false, true);
    mixer.process(in_l, in_r, out_l.data(), out_r.data(), kFrames);
    ATRIS_CHECK(mixer.meter(ChannelId::Mic).muted);
    ATRIS_CHECK(!mixer.meter(ChannelId::Game).muted);

    std::array<ChannelMeterFrame, kMixerChannelCount> frames{};
    mixer.fill_telemetry(frames.data(), frames.size());
    ATRIS_CHECK(frames[1].channel_id == 1u);
    ATRIS_CHECK((frames[1].flags & ATRISWAVE_CHANNEL_FLAG_ACTIVE) != 0u);
    ATRIS_CHECK((frames[5].flags & ATRISWAVE_CHANNEL_FLAG_MUTED) != 0u);
}

void test_convolution_reverb() {
    using namespace atriswave::dsp;
    constexpr double kSr = 48000.0;
    constexpr std::size_t kFrames = 16384;

    for (IrPreset preset : {IrPreset::StudioBooth, IrPreset::BroadcastRoom, IrPreset::ConcertHall}) {
        ConvolutionReverb conv;
        conv.prepare(kSr);
        conv.set_preset(preset);
        conv.set_mix(1.0f, 0.0f);

        std::vector<float> left(kFrames, 0.0f);
        std::vector<float> right(kFrames, 0.0f);
        left[0] = 1.0f;
        right[0] = 1.0f;

        conv.process_block(left.data(), right.data(), kFrames);

        bool all_finite = true;
        for (std::size_t i = 0; i < kFrames; ++i) {
            if (!std::isfinite(left[i]) || !std::isfinite(right[i])) {
                all_finite = false;
                break;
            }
        }
        ATRIS_CHECK(all_finite);

        const float tail_rms = compute_rms(std::vector<float>(left.begin() + 512, left.begin() + 3000));
        const float late_rms = compute_rms(std::vector<float>(left.begin() + 10000, left.begin() + kFrames));
        ATRIS_CHECK(tail_rms > 1e-4f);
        ATRIS_CHECK(late_rms < tail_rms * 0.5f);
    }
}

void test_pitch_shifter() {
    using namespace atriswave::dsp;
    constexpr double kSr = 48000.0;
    constexpr std::size_t kFrames = 16384;

    PitchShifter shifter;
    shifter.prepare(kSr);

    // 0 semitones = unity passthrough
    shifter.set_params(0.0f, 1.0f);
    auto unity = make_sine(2048, 440.0, kSr, 0.5f);
    const auto orig = unity;
    shifter.process_block(unity.data(), unity.size());
    for (std::size_t i = 0; i < unity.size(); ++i) {
        ATRIS_CHECK(std::abs(unity[i] - orig[i]) < 1e-5f);
    }

    auto count_zero_crossings = [](const std::vector<float>& buf, std::size_t start) {
        std::size_t zc = 0;
        for (std::size_t i = start + 1; i < buf.size(); ++i) {
            if (buf[i - 1] <= 0.0f && buf[i] > 0.0f) {
                ++zc;
            }
        }
        return zc;
    };

    // +12 st shifts 440 Hz upward (~2x zero-crossings)
    shifter.reset();
    shifter.set_params(12.0f, 1.0f);
    auto up_sig = make_sine(kFrames, 440.0, kSr, 0.6f);
    const std::size_t base_zc = count_zero_crossings(up_sig, 4096);
    shifter.process_block(up_sig.data(), up_sig.size());
    const std::size_t up_zc = count_zero_crossings(up_sig, 4096);
    float up_peak = 0.0f;
    for (float s : up_sig) {
        ATRIS_CHECK(std::isfinite(s));
        up_peak = std::max(up_peak, std::abs(s));
    }
    ATRIS_CHECK(up_peak <= 0.8f);
    ATRIS_CHECK(up_zc > base_zc * 1.7);

    // -12 st shifts 440 Hz downward (~0.5x zero-crossings)
    shifter.reset();
    shifter.set_params(-12.0f, 1.0f);
    auto down_sig = make_sine(kFrames, 440.0, kSr, 0.6f);
    shifter.process_block(down_sig.data(), down_sig.size());
    const std::size_t down_zc = count_zero_crossings(down_sig, 4096);
    ATRIS_CHECK(down_zc < static_cast<std::size_t>(base_zc * 0.65));
}

} // namespace

int main() {
    test_dsp_math();
    test_noise_gate();
    test_parametric_eq();
    test_dynamics_compressor();
    test_stereo_reverb();
    test_lookahead_limiter();
    test_spectral_denoiser();
    test_pll_resampler();
    test_mic_pipeline_and_mixer_bus();
    test_convolution_reverb();
    test_pitch_shifter();

    if (g_failures == 0) {
        std::printf("ALL DSP TESTS PASSED\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "DSP TESTS FAILED: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
