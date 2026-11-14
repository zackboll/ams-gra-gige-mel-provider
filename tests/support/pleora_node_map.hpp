#pragma once

// TEST ONLY. Original GenICam XML loaded by the actual GenApi runtime bundled
// with eBUS. This is a local byte-addressed port, not a camera, PvDevice, SDK
// replacement, network transport, or timestamp qualification. Production and
// tests use the same Features operations over native INodeMap nodes.

#include <PvGenApi.h>
#include <GenApi/GenApi.h>
#include <GenApi/NodeMapRef.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace pleora_node_map {

namespace native = PV_GENAPI_NS;

inline constexpr std::int64_t width = 0x00;
inline constexpr std::int64_t height = 0x08;
inline constexpr std::int64_t offset_x = 0x10;
inline constexpr std::int64_t offset_y = 0x18;
inline constexpr std::int64_t rate = 0x20;
inline constexpr std::int64_t exposure = 0x28;
inline constexpr std::int64_t exposure_implemented = 0x30;
inline constexpr std::int64_t rate_implemented = 0x38;
inline constexpr std::int64_t exposure_available = 0x40;
inline constexpr std::int64_t width_locked = 0x48;
inline constexpr std::int64_t pixel_format = 0x50;
inline constexpr std::int64_t mono8_available = 0x58;
inline constexpr std::int64_t frame_rate_enabled = 0x60;
inline constexpr std::int64_t trigger_mode = 0x68;
inline constexpr std::int64_t fixed_float = 0x70;
inline constexpr std::int64_t broken_implemented = 0x78;
inline constexpr std::int64_t signed_integer = 0x80;
inline constexpr std::int64_t write_only_integer = 0x88;

struct Access {
    bool write{false};
    std::int64_t address{0};
    std::int64_t length{0};
};

class Port final : public native::IPort {
public:
    native::EAccessMode GetAccessMode() const override { return native::RW; }
    void Read(void* buffer, std::int64_t address, std::int64_t length) override;
    void Write(const void* buffer, std::int64_t address, std::int64_t length) override;

    // Test arrangement/observation, separate from calls made by GenApi.
    void integer(std::int64_t address, std::int64_t value);
    std::int64_t integer(std::int64_t address) const;
    void real(std::int64_t address, double value);
    double real(std::int64_t address) const;
    std::size_t write_attempts(std::optional<std::int64_t> address = std::nullopt) const;

    std::vector<Access> calls;
    std::optional<std::int64_t> fail_read;
    std::optional<std::int64_t> fail_write;
    std::function<void(std::int64_t)> after_write;

private:
    std::array<std::byte, 256> bytes_{};
};

class Fixture {
public:
    Fixture();
    // Port outlives the native map that references it.
    Port port;
    native::CNodeMapRef map;
};

} // namespace pleora_node_map
