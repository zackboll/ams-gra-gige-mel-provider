// GIGE-MEL-042-R1: deterministic CommandStatus admission versus detach.
//
// Built only as an uninstalled test executable that compiles the provider
// sources with GIGE_MEL_PROVIDER_TEST_SEAMS. A thread-local hook holds one
// unsupported C2 submission either before admission or after its CommandStatus
// callback has been reserved, so each ordering with a concurrent detach is
// produced by barriers, not by timing. Every wait is bounded by a watchdog.
// Contract: docs/tasks/GIGE-MEL-042-design.md, section 3.6.
#include <gige_mel/provider.hpp>
#include "provider_test_seams.hpp"

#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <irmel/library/irmel-types/Control.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
namespace seams = gige_mel::test_seams;
using namespace std::chrono_literals;

namespace {

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression)) {                                                                         \
            throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) +           \
                                     ": " #expression);                                              \
        }                                                                                            \
    } while (false)

// Watchdogs bound a hang; they never decide a pass.
constexpr auto watchdog = 5s;
// Bound for a detach or helper that can legitimately finish.
constexpr auto prompt = 2s;
// Detach stays pending this long while the reservation is held. A correct
// provider can never finish detach in that interval, at any scheduling.
constexpr auto held_interval = 100ms;

using StatusCallback = std::function<void(ir::Channel&, const ir::CommandStatus*)>;

enum class Kind { calibration_configuration, calibration_status, camera_protect, erase, system_track };
constexpr std::array<Kind, 5> kinds{Kind::calibration_configuration, Kind::calibration_status,
    Kind::camera_protect, Kind::erase, Kind::system_track};

const char* name(Kind kind) {
    switch (kind) {
    case Kind::calibration_configuration: return "CalibrationConfigurationCmd";
    case Kind::calibration_status: return "CalibrationStatusCmd";
    case Kind::camera_protect: return "CameraProtectCmd";
    case Kind::erase: return "EraseCommand";
    case Kind::system_track: return "SystemTrackDataResponse";
    }
    return "unknown";
}

// The documented descriptions (design section 3.2), shared by future and status.
std::string description(Kind kind) {
    switch (kind) {
    case Kind::calibration_configuration:
        return "CalibrationConfigurationCmd is unsupported: calibration configuration is not available in the experimental image-stream profile";
    case Kind::calibration_status:
        return "CalibrationStatusCmd is unsupported: calibration status is not available in the experimental image-stream profile";
    case Kind::camera_protect:
        return "CameraProtectCmd is unsupported: camera protection is not available in the experimental image-stream profile";
    case Kind::erase:
        return "EraseCommand is unsupported: erase and zeroize are not available in the experimental image-stream profile";
    case Kind::system_track:
        return "SystemTrackDataResponse is unsupported: system track data input is not available in the experimental image-stream profile";
    }
    return {};
}

struct Settled {
    bool ready{false}, has_value{false};
    mel::ErrorCode code{mel::ErrorCode::None};
    std::string description;
};

// Readiness is sampled with a zero timeout before get(): an unresolved future
// is reported as not ready instead of blocking the test.
template <class T> Settled settle(mel::RequestFor<T> future) {
    Settled out;
    out.ready = future.valid() && future.wait_for(0ms) == std::future_status::ready;
    if (!out.ready) return out;
    auto result = future.get();
    out.has_value = static_cast<bool>(result);
    if (!out.has_value) {
        out.code = result.getError().getCode();
        out.description = result.getError().getDescription();
    }
    return out;
}

// Exact pinned overloads and typed futures for all five commands.
Settled send(ir::C2Channel& c2, Kind kind, std::uint32_t id) {
    switch (kind) {
    case Kind::calibration_configuration: { ir::CalibrationConfigurationCmd c; c.setCommandID(id); return settle<mel::CalibrationConfiguration>(c2.send(c)); }
    case Kind::calibration_status: { ir::CalibrationStatusCmd c; c.setCommandID(id); return settle<mel::CalibrationStatus>(c2.send(c)); }
    case Kind::camera_protect: { ir::CameraProtectCmd c; c.setCommandID(id); return settle<ir::CameraProtectCmdResp>(c2.send(c)); }
    case Kind::erase: { ir::EraseCommand c; c.setCommandID(id); return settle<ir::EraseCommandType>(c2.send(c)); }
    case Kind::system_track: { ir::SystemTrackDataResponse c; c.setCommandID(id); return settle<ir::CommandStatus>(c2.send(c)); }
    }
    throw std::logic_error("unknown kind");
}

void check_unsupported(const Settled& settled, Kind kind) {
    CHECK(settled.ready && !settled.has_value);
    CHECK(settled.code == mel::ErrorCode::Unsupported);
    CHECK(settled.description == description(kind));
}

void check_detached(const Settled& settled) {
    CHECK(settled.ready && !settled.has_value);
    CHECK(settled.code == mel::ErrorCode::InvalidState);
    CHECK(settled.description == "channel is detached");
}

void check_rejection(const ir::CommandStatus& status, std::uint32_t id, Kind kind) {
    CHECK(status.getCommandID() == id);
    CHECK(status.getState() == ir::CommandState::Rejected);
    CHECK(status.getReasonID() == ir::CannotComply::CapabilityUnavailable);
    CHECK(status.getReasonDescription() == description(kind));
}

// Holds a submission at exactly one seam point. Every point it reaches is
// counted, so each scenario also proves which side of admission it was on.
struct Hold {
    explicit Hold(seams::UnsupportedStatusPoint value) : point(value) {}
    const seams::UnsupportedStatusPoint point;
    std::mutex mutex;
    std::condition_variable changed;
    bool arrived{false}, released{false}, expired{false};
    unsigned before_admission{0}, after_reservation{0};

    static void hook(seams::UnsupportedStatusPoint reached, void* context) noexcept {
        auto& self = *static_cast<Hold*>(context);
        std::unique_lock lock(self.mutex);
        ++(reached == seams::UnsupportedStatusPoint::before_admission ? self.before_admission : self.after_reservation);
        if (reached != self.point) return;
        self.arrived = true;
        self.changed.notify_all();
        if (!self.changed.wait_for(lock, watchdog, [&] { return self.released; })) self.expired = true;
    }
    void wait_for_arrival() {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, watchdog, [&] { return arrived; }));
    }
    void release() {
        { std::lock_guard lock(mutex); released = true; }
        changed.notify_all();
    }
    void check(unsigned expected_before, unsigned expected_after) {
        std::lock_guard lock(mutex);
        CHECK(arrived && !expired);
        CHECK(before_admission == expected_before && after_reservation == expected_after);
    }
};

// Releases on every exit so a failing CHECK never strands the held sender.
struct ReleaseOnExit {
    Hold& hold;
    ~ReleaseOnExit() { hold.release(); }
};

// The hook is thread-local: it is installed on the submitting thread only.
Settled send_held(Hold& hold, ir::C2Channel& c2, Kind kind, std::uint32_t id) {
    seams::set_unsupported_status_hook(&Hold::hook, &hold);
    struct Clear { ~Clear() { seams::set_unsupported_status_hook(nullptr, nullptr); } } clear;
    return send(c2, kind, id);
}

struct Recorder {
    std::mutex mutex;
    std::vector<ir::CommandStatus> statuses;
    std::vector<std::thread::id> threads;
    StatusCallback callback() {
        return [this](auto&, auto* status) {
            std::lock_guard lock(mutex);
            statuses.push_back(*status);
            threads.push_back(std::this_thread::get_id());
        };
    }
    std::size_t count() { std::lock_guard lock(mutex); return statuses.size(); }
};

struct Fixture {
    std::shared_ptr<API_Manager> manager{getAPI_Manager("synthetic")};
    std::shared_ptr<ir::Control> control;
    std::shared_ptr<ir::C2Channel> c2;

    Fixture() {
        CHECK(manager);
        control = ir::getControl("synthetic", manager);
        CHECK(control && control->init() == ir::Return::Success);
        c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(config(ir::ChannelType::CommandAndControl)));
        CHECK(c2);
    }
    ~Fixture() {
        // Scenarios detach explicitly; this is only a defensive fallback.
        try { if (control && c2) (void)control->detachChannel(c2); } catch (...) {}
    }
    ir::Config config(ir::ChannelType type) const {
        for (const auto& capability : control->getCapabilities()) {
            if (capability.getChannelTypes() != std::vector{type}) continue;
            ir::Config selected;
            selected.setChanID(capability.getChanID());
            selected.setChannelType(type);
            return selected;
        }
        throw std::runtime_error("missing channel capability");
    }
    // Lock-free detached observation (keep-alive reads only the detached flag).
    bool detached() { return !settle<ir::Return>(c2->sendKeepAliveRep()).has_value; }
    void wait_until_detach_begins() {
        const auto deadline = std::chrono::steady_clock::now() + watchdog;
        while (!detached()) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    }
};

// Admission wins. The request passes admission and reserves its registered
// callback, then is held before invoking it. While held, no provider lock is
// owned. Another thread begins detach, which must not finish while the
// reservation is active. After release: exactly one correlated rejection, on
// the submitting thread, then detach completes. A request that arrives during
// the drain is rejected before admission and publishes nothing.
void admission_wins(Kind kind) {
    Recorder recorder;
    Fixture f;
    CHECK(f.c2->registerMetadataCallback(recorder.callback()) == ir::Return::Success);
    const std::uint32_t id = 0xC0DE0000U + static_cast<std::uint32_t>(kind);
    Hold hold(seams::UnsupportedStatusPoint::after_reservation);
    auto sender = std::async(std::launch::async, [&] {
        auto settled = send_held(hold, *f.c2, kind, id);
        return std::make_pair(settled, std::this_thread::get_id());
    });
    ReleaseOnExit release_on_exit{hold};
    hold.wait_for_arrival();

    // Neither Runtime::operations (attach/detach) nor the channel callback mutex
    // (duplicate registration) is held across the reserved interval.
    auto unlocked = std::async(std::launch::async, [&] {
        auto health = f.control->attachChannel(f.config(ir::ChannelType::HealthAndStatus));
        const bool attached = static_cast<bool>(health);
        const bool detached = attached && f.control->detachChannel(health) == ir::Return::Success;
        const bool duplicate = f.c2->registerMetadataCallback(StatusCallback{[](auto&, auto*) {}}) == ir::Return::Fail;
        return attached && detached && duplicate;
    });
    CHECK(unlocked.wait_for(prompt) == std::future_status::ready && unlocked.get());
    CHECK(recorder.count() == 0); // Reserved, not yet invoked.

    auto detach = std::async(std::launch::async, [&] { return f.control->detachChannel(f.c2); });
    f.wait_until_detach_begins();
    auto late = std::async(std::launch::async, [&] { return send(*f.c2, Kind::erase, id + 100U); });
    CHECK(detach.wait_for(held_interval) == std::future_status::timeout); // Drain waits for the reservation.
    CHECK(recorder.count() == 0);

    hold.release();
    CHECK(sender.wait_for(watchdog) == std::future_status::ready);
    const auto [settled, sender_thread] = sender.get();
    check_unsupported(settled, kind);
    CHECK(detach.wait_for(prompt) == std::future_status::ready && detach.get() == ir::Return::Success);
    CHECK(late.wait_for(prompt) == std::future_status::ready);
    check_detached(late.get());
    hold.check(1, 1);

    std::lock_guard lock(recorder.mutex);
    CHECK(recorder.statuses.size() == 1);
    check_rejection(recorder.statuses.front(), id, kind);
    CHECK(recorder.threads.front() == sender_thread);
}

// Detach wins. The request is held after its callback-context and Control
// checks but before admission. Detach completes promptly because nothing is
// reserved. The released request, and one sent after detach, both return ready
// InvalidState, and no CommandStatus is published.
void detach_wins(Kind kind) {
    Recorder recorder;
    Fixture f;
    CHECK(f.c2->registerMetadataCallback(recorder.callback()) == ir::Return::Success);
    Hold hold(seams::UnsupportedStatusPoint::before_admission);
    auto sender = std::async(std::launch::async, [&] { return send_held(hold, *f.c2, kind, 0xD00D0000U); });
    ReleaseOnExit release_on_exit{hold};
    hold.wait_for_arrival();
    auto detach = std::async(std::launch::async, [&] { return f.control->detachChannel(f.c2); });
    CHECK(detach.wait_for(prompt) == std::future_status::ready && detach.get() == ir::Return::Success);
    hold.release();
    CHECK(sender.wait_for(watchdog) == std::future_status::ready);
    check_detached(sender.get());
    hold.check(1, 0); // Never reached the reservation point.
    check_detached(send(*f.c2, kind, 0xD00D0001U));
    CHECK(recorder.count() == 0);
}

// A throwing callback reserved before detach began is contained (counted as a
// callback failure) and still releases its reservation, so detach completes.
void reserved_callback_exception_releases_detach() {
    Fixture f;
    unsigned invoked = 0;
    CHECK(f.c2->registerMetadataCallback(StatusCallback{[&](auto&, auto* status) {
        ++invoked;
        check_rejection(*status, 77, Kind::camera_protect);
        throw std::runtime_error("consumer status callback failure");
    }}) == ir::Return::Success);
    const auto before = gige_mel::provider_statistics(f.manager).callback_failures;
    Hold hold(seams::UnsupportedStatusPoint::after_reservation);
    auto sender = std::async(std::launch::async, [&] { return send_held(hold, *f.c2, Kind::camera_protect, 77); });
    ReleaseOnExit release_on_exit{hold};
    hold.wait_for_arrival();
    auto detach = std::async(std::launch::async, [&] { return f.control->detachChannel(f.c2); });
    f.wait_until_detach_begins();
    CHECK(detach.wait_for(held_interval) == std::future_status::timeout);
    hold.release();
    CHECK(sender.wait_for(watchdog) == std::future_status::ready);
    check_unsupported(sender.get(), Kind::camera_protect);
    CHECK(detach.wait_for(prompt) == std::future_status::ready && detach.get() == ir::Return::Success);
    hold.check(1, 1);
    CHECK(invoked == 1);
    CHECK(gige_mel::provider_statistics(f.manager).callback_failures == before + 1);
}

// Admitted with no registered callback: nothing is reserved, so a request held
// after admission does not delay detach and still returns its Unsupported future.
void unregistered_reserves_nothing() {
    Fixture f;
    Hold hold(seams::UnsupportedStatusPoint::after_reservation);
    auto sender = std::async(std::launch::async, [&] { return send_held(hold, *f.c2, Kind::system_track, 5); });
    ReleaseOnExit release_on_exit{hold};
    hold.wait_for_arrival();
    auto detach = std::async(std::launch::async, [&] { return f.control->detachChannel(f.c2); });
    CHECK(detach.wait_for(prompt) == std::future_status::ready && detach.get() == ir::Return::Success);
    hold.release();
    CHECK(sender.wait_for(watchdog) == std::future_status::ready);
    check_unsupported(sender.get(), Kind::system_track);
    hold.check(1, 1);
}

} // namespace

int main() {
    std::vector<std::pair<std::string, std::function<void()>>> tests;
    for (auto kind : kinds) {
        tests.emplace_back(std::string("admission wins over concurrent detach: ") + name(kind), [kind] { admission_wins(kind); });
        tests.emplace_back(std::string("detach wins before admission: ") + name(kind), [kind] { detach_wins(kind); });
    }
    tests.emplace_back("reserved throwing callback releases pending detach", reserved_callback_exception_releases_detach);
    tests.emplace_back("admitted without a callback reserves nothing", unregistered_reserves_nothing);
    for (const auto& [label, test] : tests) {
        try {
            test();
            std::cout << "PASS " << label << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << label << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "PASS " << tests.size() << " CommandStatus admission scenarios\n";
    return 0;
}
