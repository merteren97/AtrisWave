#include <atriswave/audio/driver_bridge.hpp>
#include <atriswave/audio/routing_rules.hpp>
#include <atriswave/driver_ioctl.h>
#include <atriswave/ring_buffer_validate.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

namespace {

int g_failures = 0;

#define ATRIS_CHECK(cond, msg)                                                      \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " " << (msg)   \
                      << "\n";                                                      \
            ++g_failures;                                                           \
        }                                                                           \
    } while (false)

AtrisWaveVadRingHeader make_valid_header(uint32_t capacity = 256, uint32_t channels = 2) {
    AtrisWaveVadRingHeader hdr{};
    hdr.magic = ATRISWAVE_VAD_RING_MAGIC;
    hdr.abi_version = ATRISWAVE_DRIVER_ABI_VERSION;
    hdr.endpoint_id = static_cast<uint32_t>(AtrisWaveVadEndpointId::Game);
    hdr.sample_rate = 48000;
    hdr.channels = channels;
    hdr.capacity_frames = capacity;
    hdr.write_frame_idx = 0;
    hdr.read_frame_idx = 0;
    hdr.xrun_count = 0;
    hdr.state = ATRISWAVE_VAD_RING_STATE_RUNNING;
    return hdr;
}

void test_driver_ioctl_abi() {
    ATRIS_CHECK(sizeof(AtrisWaveVadVersionInfo) == 24, "VersionInfo size == 24");
    ATRIS_CHECK(sizeof(AtrisWaveVadRingHeader) == 64, "RingHeader size == 64");
    ATRIS_CHECK(sizeof(AtrisWaveVadEndpointStats) == 32, "EndpointStats size == 32");
    ATRIS_CHECK(sizeof(AtrisWaveVadEndpointQueryRequest) == 8, "QueryRequest size == 8");
    ATRIS_CHECK(sizeof(AtrisWaveVadEndpointFormatRequest) == 16, "FormatRequest size == 16");
    ATRIS_CHECK(sizeof(AtrisWaveVadRingMapInfo) == 32, "RingMapInfo size == 32");
    ATRIS_CHECK(offsetof(AtrisWaveVadRingHeader, write_frame_idx) == 24, "write_frame_idx offset == 24");
    ATRIS_CHECK(offsetof(AtrisWaveVadRingHeader, read_frame_idx) == 32, "read_frame_idx offset == 32");
}

void test_hostile_ring_headers() {
    ATRIS_CHECK(!atriswave_vad_validate_header(nullptr), "nullptr header rejected");

    {
        auto hdr = make_valid_header();
        hdr.magic = 0xDEADBEEFu;
        ATRIS_CHECK(!atriswave_vad_validate_header(&hdr), "Bad magic rejected");
    }
    {
        auto hdr = make_valid_header();
        hdr.abi_version = 99u;
        ATRIS_CHECK(!atriswave_vad_validate_header(&hdr), "Wrong ABI version rejected");
    }
    {
        auto hdr = make_valid_header();
        hdr.endpoint_id = ATRISWAVE_VAD_ENDPOINT_COUNT; // >= 6
        ATRIS_CHECK(!atriswave_vad_validate_header(&hdr), "endpoint_id >= 6 rejected");
    }
    {
        auto hdr = make_valid_header();
        hdr.capacity_frames = 300u; // non-power-of-two
        ATRIS_CHECK(!atriswave_vad_validate_header(&hdr), "Non-power-of-two capacity rejected");
    }
    {
        auto hdr = make_valid_header();
        hdr.capacity_frames = 128u; // below min 256
        ATRIS_CHECK(!atriswave_vad_validate_header(&hdr), "Below-min capacity rejected");
    }
    {
        auto hdr = make_valid_header();
        hdr.channels = 0u;
        ATRIS_CHECK(!atriswave_vad_validate_header(&hdr), "0 channels rejected");
    }
    {
        auto hdr = make_valid_header();
        hdr.sample_rate = 384000u;
        ATRIS_CHECK(!atriswave_vad_validate_header(&hdr), "Unsupported sample rate rejected");
    }

    // Underflow: write < read
    {
        auto hdr = make_valid_header();
        hdr.write_frame_idx = 10;
        hdr.read_frame_idx = 20;
        ATRIS_CHECK(atriswave_vad_available_read(&hdr, nullptr, nullptr) == 0u,
                    "write < read underflow rejected on read");
        ATRIS_CHECK(atriswave_vad_available_write(&hdr, nullptr, nullptr) == 0u,
                    "write < read underflow rejected on write");
    }

    // Overflow: (write - read) > capacity
    {
        auto hdr = make_valid_header(256, 2);
        hdr.read_frame_idx = 100;
        hdr.write_frame_idx = 400; // diff = 300 > 256
        ATRIS_CHECK(atriswave_vad_available_read(&hdr, nullptr, nullptr) == 0u,
                    "(write - read) > capacity rejected on read");
        ATRIS_CHECK(atriswave_vad_available_write(&hdr, nullptr, nullptr) == 0u,
                    "(write - read) > capacity rejected on write");
    }

    // UINT64_MAX - 10 boundary: safe when monotonic without u64 wrap, and safe rejection if wrapped
    {
        auto hdr = make_valid_header(256, 2);
        hdr.read_frame_idx = UINT64_MAX - 10ULL;
        hdr.write_frame_idx = UINT64_MAX - 5ULL;
        ATRIS_CHECK(atriswave_vad_available_read(&hdr, nullptr, nullptr) == 5u,
                    "UINT64_MAX - 10 monotonic diff == 5");
        ATRIS_CHECK(atriswave_vad_available_write(&hdr, nullptr, nullptr) == 251u,
                    "UINT64_MAX - 10 available write == 251");

        // Writing past UINT64_MAX must clamp/avoid u64 overflow wrap
        std::vector<float> pcm(256u * 2u, 0.0f);
        std::vector<float> src(20u * 2u, 0.5f);
        const uint32_t wrote = atriswave_vad_write_frames(&hdr, pcm.data(), src.data(), 20u);
        ATRIS_CHECK(wrote <= 5u && hdr.write_frame_idx >= hdr.read_frame_idx,
                    "Near-UINT64_MAX write does not wrap write_frame_idx");

        // Wrapped u64 (write < read) rejected safely
        hdr.write_frame_idx = 4ULL;
        ATRIS_CHECK(atriswave_vad_available_read(&hdr, nullptr, nullptr) == 0u,
                    "Wrapped u64 rejected safely");
    }
}

void test_vad_ring_session_wraparound() {
    constexpr uint32_t kCap = 256;
    constexpr uint32_t kCh = 2;
    auto hdr = make_valid_header(kCap, kCh);
    // Start near wrap-around boundary of capacity (frame 240)
    hdr.read_frame_idx = 240;
    hdr.write_frame_idx = 240;

    std::vector<float> pcm(static_cast<size_t>(kCap) * kCh, 0.0f);

    atriswave::audio::VadRingSession session;
    ATRIS_CHECK(session.attach(&hdr, pcm.data()), "VadRingSession attach succeeds");
    ATRIS_CHECK(session.available_write() == kCap, "Initial available_write == 256");

    // Write 64 frames across index 240 -> 304 (wraps around 256!)
    constexpr uint32_t kBatch = 64;
    std::vector<float> tx(static_cast<size_t>(kBatch) * kCh, 0.0f);
    for (uint32_t i = 0; i < kBatch; ++i) {
        tx[i * 2] = static_cast<float>(i) * 0.01f;
        tx[i * 2 + 1] = -static_cast<float>(i) * 0.01f;
    }

    const uint32_t written = session.write_frames(tx.data(), kBatch);
    ATRIS_CHECK(written == kBatch, "Multi-chunk wrap-around write == 64 frames");
    ATRIS_CHECK(session.available_read() == kBatch, "available_read == 64 frames");

    std::vector<float> rx(static_cast<size_t>(kBatch) * kCh, 0.0f);
    const uint32_t read = session.read_frames(rx.data(), kBatch);
    ATRIS_CHECK(read == kBatch, "Multi-chunk wrap-around read == 64 frames");
    ATRIS_CHECK(session.available_read() == 0u, "Empty after draining");

    for (size_t i = 0; i < tx.size(); ++i) {
        ATRIS_CHECK(std::fabs(tx[i] - rx[i]) < 1e-6f, "PCM sample preserved across wrap-around");
    }

    session.detach();
    ATRIS_CHECK(!session.is_attached(), "Detached cleanly");
}

void test_driver_bridge_offline() {
    const auto status = atriswave::audio::DriverBridge::probe_driver();
    ATRIS_CHECK(!status.driver_installed, "Driver not installed in test env");
    ATRIS_CHECK(status.abi_version == 0u, "Offline abi_version == 0");
    ATRIS_CHECK(status.endpoint_count == ATRISWAVE_VAD_ENDPOINT_COUNT, "endpoint_count == 6");
    ATRIS_CHECK(status.fallback_mode == "wasapi_vbcable", "fallback_mode == wasapi_vbcable");
}

void test_routing_rules_store() {
    const std::filesystem::path temp_file =
        std::filesystem::temp_directory_path() / "atriswave_routing_rules_test.json";
    std::error_code ec;
    std::filesystem::remove(temp_file, ec);

    {
        atriswave::audio::RoutingRulesStore store(temp_file);
        ATRIS_CHECK(store.list_rules().empty(), "Fresh store is empty");

        const std::string wasapi_dev_id = "{0.0.0.00000000}.{3713cb20-17e2-4673-8615-f26d528d8255}";
        atriswave::audio::RoutingRule r1{"Valorant.exe", 1, wasapi_dev_id, true};
        atriswave::audio::RoutingRule r2{"Discord.exe", 2, "", false};
        ATRIS_CHECK(store.set_rule(r1), "Save r1 succeeds");
        ATRIS_CHECK(store.set_rule(r2), "Save r2 succeeds");

        const auto found = store.find_rule("valorant.exe"); // case-insensitive
        ATRIS_CHECK(found.has_value(), "Case-insensitive find_rule succeeds");
        if (found.has_value()) {
            ATRIS_CHECK(found->channel_id == 1, "Found channel_id == 1");
            ATRIS_CHECK(found->target_device_id == wasapi_dev_id, "Found target_device_id");
            ATRIS_CHECK(found->loopback_capture, "Found loopback_capture == true");
        }
    }

    // Reload from disk in a new store instance
    {
        atriswave::audio::RoutingRulesStore reloaded(temp_file);
        const auto rules = reloaded.list_rules();
        ATRIS_CHECK(rules.size() == 2, "Reloaded 2 rules from disk");
        const auto reloaded_r1 = reloaded.find_rule("Valorant.exe");
        ATRIS_CHECK(reloaded_r1.has_value(), "Reloaded Valorant.exe rule");
        if (reloaded_r1.has_value()) {
            ATRIS_CHECK(
                reloaded_r1->target_device_id == "{0.0.0.00000000}.{3713cb20-17e2-4673-8615-f26d528d8255}",
                "Real WASAPI device ID with braces survived JSON reload");
            ATRIS_CHECK(reloaded_r1->loopback_capture, "Reloaded loopback_capture == true");
        }
        ATRIS_CHECK(reloaded.delete_rule("VALORANT.EXE"), "Case-insensitive delete succeeds");
        ATRIS_CHECK(reloaded.list_rules().size() == 1, "1 rule remains after delete");
        ATRIS_CHECK(!reloaded.delete_rule("nonexistent.exe"), "Delete missing returns false");
    }

    std::filesystem::remove(temp_file, ec);
}

} // namespace

int main() {
    test_driver_ioctl_abi();
    test_hostile_ring_headers();
    test_vad_ring_session_wraparound();
    test_driver_bridge_offline();
    test_routing_rules_store();

    if (g_failures == 0) {
        std::cout << "[atriswave_driver_bridge_tests] ALL TESTS PASSED\n";
        return 0;
    }

    std::cerr << "[atriswave_driver_bridge_tests] " << g_failures << " FAILURE(S)\n";
    return 1;
}
