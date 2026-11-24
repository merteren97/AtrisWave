#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATRISWAVE_PLUGIN_IPC_MAGIC 0x41575048u /* "AWPH" */
#define ATRISWAVE_PLUGIN_IPC_ABI_VERSION 1u
#define ATRISWAVE_PLUGIN_MAX_FRAMES 2048u

#define ATRISWAVE_PLUGIN_GET_API_SYMBOL "atriswave_plugin_get_api"
#define ATRISWAVE_CLAP_ENTRY_SYMBOL "clap_entry"
#define ATRISWAVE_VST3_FACTORY_SYMBOL "GetPluginFactory"

enum AtrisWavePluginCommand {
    ATRISWAVE_PLUGIN_CMD_NONE = 0u,
    ATRISWAVE_PLUGIN_CMD_PROCESS = 1u,
    ATRISWAVE_PLUGIN_CMD_SET_PARAM = 2u,
    ATRISWAVE_PLUGIN_CMD_SHUTDOWN = 3u,
    ATRISWAVE_PLUGIN_CMD_SIMULATE_CRASH = 99u
};

enum AtrisWavePluginStatus {
    ATRISWAVE_PLUGIN_STATUS_IDLE = 0u,
    ATRISWAVE_PLUGIN_STATUS_READY = 1u,
    ATRISWAVE_PLUGIN_STATUS_ERROR = 2u
};

enum AtrisWavePluginFormat {
    ATRISWAVE_PLUGIN_FORMAT_UNKNOWN = 0u,
    ATRISWAVE_PLUGIN_FORMAT_ATRISWAVE_C = 1u,
    ATRISWAVE_PLUGIN_FORMAT_CLAP = 2u,
    ATRISWAVE_PLUGIN_FORMAT_VST3 = 3u
};

#pragma pack(push, 4)

typedef struct AtrisWavePluginSlotLayout {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t max_frames;
    uint32_t sample_rate;
    uint32_t num_frames;
    uint32_t command;
    uint32_t status;
    uint32_t param_index;
    float param_value;
    float in_left[ATRISWAVE_PLUGIN_MAX_FRAMES];
    float in_right[ATRISWAVE_PLUGIN_MAX_FRAMES];
    float out_left[ATRISWAVE_PLUGIN_MAX_FRAMES];
    float out_right[ATRISWAVE_PLUGIN_MAX_FRAMES];
} AtrisWavePluginSlotLayout;

#pragma pack(pop)

typedef struct AtrisWavePluginApi {
    uint32_t abi_version;
    const char* name;
    const char* vendor;
    uint32_t param_count;
    void* (*create)(uint32_t sample_rate, uint32_t max_frames);
    void (*destroy)(void* instance);
    void (*process)(void* instance,
                    const float* in_left,
                    const float* in_right,
                    float* out_left,
                    float* out_right,
                    uint32_t num_frames);
    void (*set_param)(void* instance, uint32_t param_index, float value);
    float (*get_param)(void* instance, uint32_t param_index);
} AtrisWavePluginApi;

typedef const AtrisWavePluginApi* (*AtrisWavePluginGetApiFn)(void);

typedef struct AtrisWavePluginExportDiscovery {
    uint32_t detected_format;
    uint8_t has_atriswave_c;
    uint8_t has_clap;
    uint8_t has_vst3;
    uint8_t reserved;
} AtrisWavePluginExportDiscovery;

#ifdef __cplusplus
} /* extern "C" */

static_assert(ATRISWAVE_PLUGIN_IPC_MAGIC == 0x41575048u, "magic constant mismatch");
static_assert(ATRISWAVE_PLUGIN_IPC_ABI_VERSION == 1u, "abi_version constant mismatch");
static_assert(ATRISWAVE_PLUGIN_MAX_FRAMES == 2048u, "max_frames constant mismatch");

static_assert(offsetof(AtrisWavePluginSlotLayout, magic) == 0, "magic offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, abi_version) == 4, "abi_version offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, max_frames) == 8, "max_frames offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, sample_rate) == 12, "sample_rate offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, num_frames) == 16, "num_frames offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, command) == 20, "command offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, status) == 24, "status offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, param_index) == 28, "param_index offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, param_value) == 32, "param_value offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, in_left) == 36, "in_left offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, in_right) == 36 + 2048 * 4, "in_right offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, out_left) == 36 + 2048 * 8, "out_left offset mismatch");
static_assert(offsetof(AtrisWavePluginSlotLayout, out_right) == 36 + 2048 * 12, "out_right offset mismatch");
static_assert(sizeof(AtrisWavePluginSlotLayout) == 36 + 2048 * 16, "AtrisWavePluginSlotLayout size mismatch");

namespace atriswave {

[[nodiscard]] inline AtrisWavePluginExportDiscovery classify_plugin_exports(
    bool has_atriswave_c,
    bool has_clap,
    bool has_vst3) noexcept {
    AtrisWavePluginExportDiscovery info{};
    info.has_atriswave_c = has_atriswave_c ? 1u : 0u;
    info.has_clap = has_clap ? 1u : 0u;
    info.has_vst3 = has_vst3 ? 1u : 0u;
    if (has_atriswave_c) {
        info.detected_format = ATRISWAVE_PLUGIN_FORMAT_ATRISWAVE_C;
    } else if (has_clap) {
        info.detected_format = ATRISWAVE_PLUGIN_FORMAT_CLAP;
    } else if (has_vst3) {
        info.detected_format = ATRISWAVE_PLUGIN_FORMAT_VST3;
    } else {
        info.detected_format = ATRISWAVE_PLUGIN_FORMAT_UNKNOWN;
    }
    return info;
}

#if defined(_WIN32) && defined(_WINDOWS_)
[[nodiscard]] inline AtrisWavePluginExportDiscovery discover_plugin_dll_exports(HMODULE module) noexcept {
    if (module == nullptr) {
        return classify_plugin_exports(false, false, false);
    }
    const bool has_atriswave_c = GetProcAddress(module, ATRISWAVE_PLUGIN_GET_API_SYMBOL) != nullptr;
    const bool has_clap = GetProcAddress(module, ATRISWAVE_CLAP_ENTRY_SYMBOL) != nullptr;
    const bool has_vst3 = GetProcAddress(module, ATRISWAVE_VST3_FACTORY_SYMBOL) != nullptr;
    return classify_plugin_exports(has_atriswave_c, has_clap, has_vst3);
}
#endif

} // namespace atriswave
#endif
