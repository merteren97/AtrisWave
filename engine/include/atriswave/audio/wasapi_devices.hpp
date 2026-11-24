#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atriswave::audio {

struct AudioDeviceInfo {
    std::string id;
    std::string name;
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
};

} // namespace atriswave::audio
