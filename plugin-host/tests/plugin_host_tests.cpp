#include "atriswave/plugin_sandbox_client.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::wstring utf8_to_wide(const char* str) {
    if (str == nullptr || *str == '\0') {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring result(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str, -1, result.data(), needed);
    return result;
}

bool check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        return false;
    }
    return true;
}

bool test_remote_gain_processing(const std::wstring& host_exe, const std::wstring& plugin_dll) {
    atriswave::PluginSandboxClient client;
    if (!check(client.start(host_exe, plugin_dll, 48000u, 50u), "client.start() failed")) {
        return false;
    }
    if (!check(!client.is_bypassed(), "client should not be bypassed after start")) {
        return false;
    }
    if (!check(client.set_param(0u, 0.5f), "client.set_param(0, 0.5f) failed")) {
        return false;
    }

    constexpr uint32_t kFrames = 480u;
    std::vector<float> in_left(kFrames);
    std::vector<float> in_right(kFrames);
    std::vector<float> out_left(kFrames, 0.0f);
    std::vector<float> out_right(kFrames, 0.0f);

    for (uint32_t i = 0; i < kFrames; ++i) {
        const float phase = 2.0f * 3.14159265358979323846f * 440.0f * static_cast<float>(i) / 48000.0f;
        in_left[i] = std::sin(phase);
        in_right[i] = std::cos(phase);
    }

    client.process(in_left.data(), in_right.data(), out_left.data(), out_right.data(), kFrames);

    if (!check(!client.is_bypassed(), "client unexpectedly bypassed during healthy process")) {
        return false;
    }

    for (uint32_t i = 0; i < kFrames; ++i) {
        const float err_l = std::fabs(out_left[i] - 0.5f * in_left[i]);
        const float err_r = std::fabs(out_right[i] - 0.5f * in_right[i]);
        if (!check(err_l <= 1e-5f && err_r <= 1e-5f, "gain output mismatch")) {
            return false;
        }
    }

    client.stop();
    std::cout << "[PASS] test_remote_gain_processing\n";
    return true;
}

bool test_crash_isolation_and_crossfade_bypass(const std::wstring& host_exe, const std::wstring& plugin_dll) {
    atriswave::PluginSandboxClient client;
    if (!check(client.start(host_exe, plugin_dll, 48000u, 50u), "client.start() failed")) {
        return false;
    }
    if (!check(client.crossfade_samples() == 240u, "expected 240 crossfade samples at 48kHz")) {
        return false;
    }
    if (!check(client.set_param(0u, 0.5f), "client.set_param(0, 0.5f) failed")) {
        return false;
    }

    // Prime one healthy block so last_wet is non-zero before the crash.
    constexpr uint32_t kPrimeFrames = 120u;
    std::vector<float> prime_in_l(kPrimeFrames, 0.8f);
    std::vector<float> prime_in_r(kPrimeFrames, -0.6f);
    std::vector<float> prime_out_l(kPrimeFrames, 0.0f);
    std::vector<float> prime_out_r(kPrimeFrames, 0.0f);
    client.process(prime_in_l.data(), prime_in_r.data(), prime_out_l.data(), prime_out_r.data(), kPrimeFrames);

    if (!check(std::fabs(prime_out_l.back() - 0.4f) <= 1e-5f, "prime block left mismatch")) {
        return false;
    }

    // Trigger deterministic crash in child process.
    if (!check(client.simulate_crash(), "client.simulate_crash() did not detect child exit")) {
        return false;
    }
    if (!check(client.is_bypassed(), "client should be bypassed after child crash")) {
        return false;
    }
    if (!check(client.crash_count() == 1u, "crash_count should be 1 after crash")) {
        return false;
    }

    // Process 480 frames: first 240 frames crossfade smoothly from last wet (0.4 / -0.3) to dry input;
    // remaining 240 frames must be exact dry bypass (out == in).
    constexpr uint32_t kPostFrames = 480u;
    std::vector<float> in_l(kPostFrames);
    std::vector<float> in_r(kPostFrames);
    std::vector<float> out_l(kPostFrames, 0.0f);
    std::vector<float> out_r(kPostFrames, 0.0f);

    for (uint32_t i = 0; i < kPostFrames; ++i) {
        const float phase = 2.0f * 3.14159265358979323846f * 220.0f * static_cast<float>(i) / 48000.0f;
        in_l[i] = 0.8f * std::cos(phase);
        in_r[i] = -0.6f * std::cos(phase);
    }

    client.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), kPostFrames);

    float prev_l = prime_out_l.back();
    float prev_r = prime_out_r.back();
    for (uint32_t i = 0; i < kPostFrames; ++i) {
        if (!check(std::isfinite(out_l[i]) && std::isfinite(out_r[i]), "non-finite sample during bypass")) {
            return false;
        }
        const float step_l = std::fabs(out_l[i] - prev_l);
        const float step_r = std::fabs(out_r[i] - prev_r);
        if (!check(step_l <= 0.05f && step_r <= 0.05f, "discontinuity exceeded bound during crossfade")) {
            return false;
        }
        prev_l = out_l[i];
        prev_r = out_r[i];
    }

    for (uint32_t i = 240u; i < kPostFrames; ++i) {
        if (!check(std::fabs(out_l[i] - in_l[i]) <= 1e-6f && std::fabs(out_r[i] - in_r[i]) <= 1e-6f,
                   "dry bypass mismatch after 5ms crossfade")) {
            return false;
        }
    }

    client.stop();
    std::cout << "[PASS] test_crash_isolation_and_crossfade_bypass\n";
    return true;
}

} // namespace

int main() {
    const std::wstring host_exe = utf8_to_wide(ATRISWAVE_PLUGIN_HOST_EXE);
    const std::wstring plugin_dll = utf8_to_wide(ATRISWAVE_SAMPLE_PLUGIN_DLL);

    if (host_exe.empty() || plugin_dll.empty()) {
        std::cerr << "[FAIL] Missing ATRISWAVE_PLUGIN_HOST_EXE or ATRISWAVE_SAMPLE_PLUGIN_DLL\n";
        return 1;
    }

    if (!test_remote_gain_processing(host_exe, plugin_dll)) {
        return 1;
    }
    if (!test_crash_isolation_and_crossfade_bypass(host_exe, plugin_dll)) {
        return 1;
    }

    return 0;
}
