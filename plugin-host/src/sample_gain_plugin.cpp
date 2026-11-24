#include "atriswave/plugin_ipc.h"

#include <atomic>
#include <new>

namespace {

struct SampleGainInstance {
    uint32_t sample_rate{48000};
    uint32_t max_frames{ATRISWAVE_PLUGIN_MAX_FRAMES};
    std::atomic<float> gain{0.5f};
};

void* gain_create(uint32_t sample_rate, uint32_t max_frames) {
    auto* instance = new (std::nothrow) SampleGainInstance{};
    if (instance != nullptr) {
        instance->sample_rate = sample_rate;
        instance->max_frames = max_frames;
        instance->gain.store(0.5f, std::memory_order_relaxed);
    }
    return instance;
}

void gain_destroy(void* raw) {
    delete static_cast<SampleGainInstance*>(raw);
}

void gain_process(void* raw,
                  const float* in_left,
                  const float* in_right,
                  float* out_left,
                  float* out_right,
                  uint32_t num_frames) {
    auto* instance = static_cast<SampleGainInstance*>(raw);
    if (instance == nullptr || in_left == nullptr || in_right == nullptr ||
        out_left == nullptr || out_right == nullptr) {
        return;
    }

    const float g = instance->gain.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < num_frames; ++i) {
        out_left[i] = in_left[i] * g;
        out_right[i] = in_right[i] * g;
    }
}

void gain_set_param(void* raw, uint32_t param_index, float value) {
    auto* instance = static_cast<SampleGainInstance*>(raw);
    if (instance == nullptr || param_index != 0u) {
        return;
    }
    instance->gain.store(value, std::memory_order_relaxed);
}

float gain_get_param(void* raw, uint32_t param_index) {
    auto* instance = static_cast<SampleGainInstance*>(raw);
    if (instance == nullptr || param_index != 0u) {
        return 0.0f;
    }
    return instance->gain.load(std::memory_order_relaxed);
}

const AtrisWavePluginApi kSampleGainApi{
    ATRISWAVE_PLUGIN_IPC_ABI_VERSION,
    "AtrisWave Sample Gain",
    "MCorp",
    1u,
    &gain_create,
    &gain_destroy,
    &gain_process,
    &gain_set_param,
    &gain_get_param,
};

} // namespace

extern "C" __declspec(dllexport) const AtrisWavePluginApi* atriswave_plugin_get_api(void) {
    return &kSampleGainApi;
}
