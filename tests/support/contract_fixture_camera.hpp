#pragma once

// Test-only, independently implemented camera backend ("contract fixture").
// It exists to qualify the common MEL provider's backend-injection seam with a
// second implementation whose identities, limits, layout, frame IDs, clock and
// configuration semantics deliberately differ from the synthetic backend. It is
// compiled only for testing, is never installed and is not a factory selection.
// It uses only the project backend contract and the C++ standard library: no
// MEL types, no provider.hpp and no vendor SDK.
//
// Documented fixture semantics (fixture-specific, NOT universal requirements):
// - Identity: backend "contract-fixture", single device "fixture-camera-17".
//   open() of another ID returns not_found; a second open while the device is
//   not logically closed returns busy.
// - Capabilities: virtual 384x256 Mono8 sensor; width increment 4, height
//   increment 2, X/Y offset increments 2; integer 10-90 Hz; exposure at least
//   100 us and no longer than one frame period. No trigger, no direct buffers.
// - Initial configuration: 80x60 at offset (4,6), 40 Hz, 1,250,000 ns.
// - apply() writes virtual features in order ROI -> frame rate -> exposure,
//   validating each group immediately before writing it. A later failure
//   returns invalid_argument and leaves earlier groups written (truthful
//   partial application). The actual state is always available via
//   configuration().
// - Frame layout: row stride = round_up(width, 16) + 16 bytes; every padding
//   byte is 0xA5. The lease span is exactly stride * height bytes.
// - Pixels: value(sx, sy, n) = (3*sx + 5*sy + 11*(n & 0xff)) & 0xff, where
//   sx/sy are SENSOR coordinates (ROI offset + local x/y) and n is the native
//   frame ID (only its low 8 bits contribute).
// - Native frame IDs: device-lifetime sequence starting at 0x2'FFFF'FFF0 with
//   step 7 (low 32 bits wrap after three frames).
// - Generations: stream generation 900 for the first start, +1 per start;
//   configuration generation is exactly the value supplied to start().
// - Time: ClockDomain::device, TimestampEvent::integration_start, clock
//   generation 3 for the device lifetime. A VIRTUAL device clock starts at
//   7,200 s and advances by exactly one frame period (1e9 / rate ns, integer
//   division) per generated frame. It is not host time, UTC or PTP.
// - Pacing: frames become due one exposure after start and then every frame
//   period of real steady time, generated synchronously inside receive().
// - Storage: each start() allocates one pool of `source_slots` slots (default
//   3). A leased slot is never written until its lease is destroyed. With all
//   slots leased, receive() waits for release/stop/close or its timeout and
//   counts one pool-starvation event per receive call. start() is refused with
//   resource_exhausted while leases from the previous pool are outstanding.
// - Ownership: every lease owner retains its pool (bytes and lease flags) and
//   the device's shared state (mutex/condition variable used to publish the
//   release). Logical close, stream stop and destruction of stream/device/
//   backend never free a leased slot; the pool and shared state retire when
//   their last owner, possibly a lease on any thread, is released.
// - Receive: bounded (timeouts above 24 h are clamped); request_stop()/stop()
//   wake it with stopped, close() wakes it with disconnected. One receiving
//   thread per stream. stop/request_stop/release/destructors do not throw.

#include <gige_mel/backend.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace contract_fixture {

struct Options {
    std::size_t source_slots{3};
};

// Observation counters only. Fixture objects hold this; it holds nothing of
// theirs, so observing never keeps a supposedly released resource alive.
struct Observation {
    std::atomic<unsigned> pools_created{0};
    std::atomic<unsigned> pools_retired{0};
    std::atomic<unsigned> shared_states_retired{0};
    std::atomic<unsigned> streams_retired{0};
    std::atomic<unsigned> devices_retired{0};
    std::atomic<unsigned> backends_retired{0};
    // Incremented under the device mutex immediately before a receive blocks.
    std::atomic<unsigned> receive_waits{0};
};

// Documented fixture constants, exposed for the fixture-specific checks.
inline constexpr const char* backend_id = "contract-fixture";
inline constexpr const char* device_id = "fixture-camera-17";
inline constexpr std::uint64_t first_native_frame_id = 0x2'FFFF'FFF0ULL;
inline constexpr std::uint64_t native_frame_id_step = 7;
inline constexpr std::uint64_t first_stream_generation = 900;
inline constexpr std::uint64_t clock_generation = 3;
inline constexpr std::int64_t virtual_clock_origin_ns = 7'200'000'000'000LL;
inline constexpr std::byte padding_byte{0xA5};

constexpr std::size_t row_stride(std::uint32_t width) noexcept {
    return (static_cast<std::size_t>(width) + 15U) / 16U * 16U + 16U;
}
constexpr std::uint8_t pixel(std::uint32_t sensor_x, std::uint32_t sensor_y, std::uint64_t native_id) noexcept {
    return static_cast<std::uint8_t>((3ULL * sensor_x + 5ULL * sensor_y + 11ULL * (native_id & 0xffU)) & 0xffU);
}

std::shared_ptr<gige_mel::backend::ICameraBackend> make_backend(
    Options options = {}, std::shared_ptr<Observation> observation = {});

} // namespace contract_fixture
