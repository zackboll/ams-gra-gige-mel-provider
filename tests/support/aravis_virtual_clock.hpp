#pragma once

// TEST-ONLY VIRTUAL INTEGRATION-START CLOCK (GIGE-MEL-010). Never installed,
// never linked into the provider, the adapter library or the diagnostic, and
// not reachable from any production factory, environment variable or setting.
//
// Purpose: exercise MEL Image publication of GENUINE Aravis SDK frames through
// the unchanged common provider. It does NOT qualify Aravis or camera time.
//
// A decorator around an ICameraBackend (in practice the real-SDK Aravis Fake
// backend). Every configuration and lifecycle call is forwarded unchanged.
// Frames are obtained from the wrapped IFrameStream::receive(); the returned
// descriptor is copied unchanged EXCEPT its timestamp, which is replaced by a
// TEST MODEL value:
//
//   domain = ClockDomain::device, event = TimestampEvent::integration_start,
//   valid = true, clock_generation = fixed per run (set by the test),
//   value  = virtual_epoch + sequence * virtual_frame_period (checked),
//
// where `sequence` counts the frames this decorated stream received from the
// wrapped stream since its start(). These are TEST MODEL values: not camera
// time, not host receive time, not PTP and not UTC. The SDK buffer timestamp
// (arv_buffer_get_timestamp) is never read or written here, no exposure is
// subtracted from anything, and frame completion is not relabelled.
//
// Ownership: the decorated FrameLease shares the ORIGINAL lease through a
// shared owner (SourceHold), so the same immutable SDK bytes stay valid and
// the SDK buffer is released/requeued exactly once, by the original lease's
// owner, only after the provider dropped the decorated lease.

#include <gige_mel/backend.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace aravis_virtual_clock {

namespace backend = gige_mel::backend;

// Declared virtual clock parameters (TEST MODEL values).
struct Model {
    std::chrono::nanoseconds epoch{std::chrono::seconds{1000}};
    std::chrono::nanoseconds period{std::chrono::milliseconds{10}};
    std::uint64_t clock_generation{1};
};

// Checked: nullopt when epoch + sequence * period is not representable as a
// nonnegative signed 64-bit nanosecond count, or the model is invalid.
std::optional<std::chrono::nanoseconds> value_at(const Model& model, std::uint64_t sequence);
// The declared valid integration-start timestamp for `sequence`, or nullopt.
std::optional<backend::FrameTimestamp> nominal(const Model& model, std::uint64_t sequence);

// Per-frame timestamp plan. Default: the nominal declared timestamp. A test
// may return a different value to exercise fail-closed provider behavior.
// `original` is the timestamp the wrapped stream returned (the production
// Aravis adapter always returns FrameTimestamp{}).
using Plan = std::function<backend::FrameTimestamp(std::uint64_t sequence, const backend::FrameTimestamp& nominal,
                                                   const backend::FrameTimestamp& original)>;

class Clock {
public:
    void set_model(const Model& model);
    Model model() const;
    void set_plan(Plan plan); // empty: nominal
    // The timestamp to deliver, or nullopt on checked overflow.
    std::optional<backend::FrameTimestamp> assign(std::uint64_t sequence, const backend::FrameTimestamp& original) const;

private:
    mutable std::mutex mutex_;
    Model model_;
    std::shared_ptr<const Plan> plan_;
};

// One frame that passed through a decorated stream.
struct SourceFrame {
    std::uint64_t run{0};      // 1-based ordinal of the decorated stream start()
    std::uint64_t sequence{0}; // per-run sequence used by the clock
    backend::FrameDescriptor original;  // exactly as the wrapped stream returned it
    backend::FrameDescriptor delivered; // as returned to the provider
    const std::byte* original_data{nullptr};
    std::size_t original_size{0};
    const std::byte* delivered_data{nullptr};
    std::size_t delivered_size{0};
    bool substituted{false}; // false for pass-through (no clock)
};

// Observations of the decorator (test thread and provider worker; locked).
class Ledger {
public:
    std::size_t frames() const;
    std::uint64_t runs() const;
    std::size_t released() const;    // decorated leases whose source lease has ended
    std::size_t outstanding() const; // decorated leases still held downstream
    std::size_t peak_outstanding() const;
    std::optional<SourceFrame> by_frame_id(std::uint64_t native_frame_id) const;
    std::vector<SourceFrame> all() const;
    // Bounded waits; false on expiry.
    bool wait_frames(std::size_t at_least, std::chrono::milliseconds bound) const;
    bool wait_outstanding(std::size_t at_most, std::chrono::milliseconds bound) const;

    // Decorator side.
    std::uint64_t begin_run();
    void record(SourceFrame frame);
    void note_release();

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<SourceFrame> frames_;
    std::map<std::uint64_t, std::size_t> by_id_;
    std::uint64_t runs_{0};
    std::size_t created_{0}, released_{0}, peak_{0};
};

// The decorator. `clock` null => pure pass-through (timestamps untouched),
// used to show that removing the virtual clock restores rejection.
class Backend final : public backend::ICameraBackend {
public:
    Backend(std::shared_ptr<backend::ICameraBackend> inner, std::shared_ptr<const Clock> clock,
            std::shared_ptr<Ledger> ledger);
    std::string_view id() const noexcept override;
    backend::Result<std::vector<backend::DeviceInfo>> enumerate() override;
    backend::Result<std::shared_ptr<backend::ICameraDevice>> open(std::string_view device_id) override;
    // The wrapped (real adapter) device most recently opened, for SDK-level
    // test arrangements (aravis::with_native_device_for_testing).
    std::shared_ptr<backend::ICameraDevice> last_opened_inner() const;

private:
    std::shared_ptr<backend::ICameraBackend> inner_;
    std::shared_ptr<const Clock> clock_;
    std::shared_ptr<Ledger> ledger_;
    mutable std::mutex mutex_;
    std::weak_ptr<backend::ICameraDevice> last_;
};

} // namespace aravis_virtual_clock
