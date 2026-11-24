#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atriswave::audio {

struct AudioDeviceInfo {
    std::string id;
    std::string name;
    std::string virtual_pair_group;
    bool is_capture{false};
    bool is_default{false};
    bool is_virtual{false};
    uint32_t sample_rate{48000};
    uint32_t channels{2};
};

class WasapiDeviceEnumerator final {
public:
    /// Enumerates all active WASAPI capture and render endpoints.
    [[nodiscard]] static std::vector<AudioDeviceInfo> enumerate_devices();

    /// Returns true if the friendly name matches known virtual audio drivers
    /// (VB-Cable, Virtual Audio, AtrisWave, VoiceMeeter).
    [[nodiscard]] static bool is_virtual_device_name(std::string_view name) noexcept;

    /// Returns a shared pair group identifier for paired virtual render/capture endpoints
    /// (e.g., "vb-cable-0", "vb-cable-a", "vb-cable-b", "atriswave-vad-0"), or empty if physical.
    [[nodiscard]] static std::string detect_virtual_pair_group(std::string_view name);
};

} // namespace atriswave::audio
