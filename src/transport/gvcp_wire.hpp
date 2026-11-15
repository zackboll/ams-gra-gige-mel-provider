#pragma once
#include "controlled_gvcp.hpp"

namespace gige_mel::controlled_gvcp::wire {
// Wire reference: Aravis 0.8.36 arvgvcpprivate.h / arvgvcp.c, not SDK linkage.
inline constexpr std::uint16_t read_register = 0x0080, write_register = 0x0082,
    read_memory = 0x0084, write_memory = 0x0086, pending_ack = 0x0089;
inline std::uint16_t u16(std::span<const std::byte> p, std::size_t i) {
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(p[i]) << 8) |
                                      std::to_integer<unsigned>(p[i + 1]));
}
inline std::uint32_t u32(std::span<const std::byte> p, std::size_t i) {
    return (static_cast<std::uint32_t>(u16(p, i)) << 16) | u16(p, i + 2);
}
inline void put16(std::span<std::byte> p, std::size_t i, std::uint16_t v) {
    p[i] = std::byte(v >> 8); p[i + 1] = std::byte(v & 255);
}
inline void put32(std::span<std::byte> p, std::size_t i, std::uint32_t v) {
    put16(p, i, static_cast<std::uint16_t>(v >> 16));
    put16(p, i + 2, static_cast<std::uint16_t>(v & 65535));
}
enum class Kind { ignore, success, pending, device_error };
struct Parsed { Kind kind{Kind::ignore}; std::uint16_t status{}; };
inline Parsed parse(std::span<const std::byte> p, std::uint16_t id,
                    std::uint16_t command, std::uint32_t address, std::size_t size) {
    if (size == 0 || size > maximum_transfer || size % 4 != 0 || address % 4 != 0 ||
        size - 1 > UINT32_MAX - address) return {};
    if (p.size() < 8 || p.size() > 520 || u16(p, 4) != p.size() - 8 ||
        id == 0 || u16(p, 6) != id) return {};
    const auto status = u16(p, 0), ack = u16(p, 2);
    if (status == 0 && ack == pending_ack && p.size() == 12) return {Kind::pending};
    if (ack != command + 1) return {};
    // Pinned error type/enum: 0x8001..0x8017, generic 0x80ff or 0x8fff. Unknown codes
    // remain malformed/unqualified, never silently interpreted as success.
    if ((status >= 0x8001 && status <= 0x8017) || status == 0x80ff || status == 0x8fff) {
        if (p.size() != 8) return {};
        return {Kind::device_error, status};
    }
    if (status != 0) return {};
    if (command != read_register && command != write_register &&
        command != read_memory && command != write_memory) return {};
    const auto expected = command == read_memory ? 12 + size : 12;
    if (p.size() != expected) return {};
    if ((command == read_memory || command == write_memory) && u32(p, 8) != address) return {};
    // Single register ACK's reserved/index word: pinned Fake sends index 1.
    if (command == write_register && u32(p, 8) != 1) return {};
    return {Kind::success};
}
} // namespace gige_mel::controlled_gvcp::wire
