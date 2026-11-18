#include <gige_mel/provider.hpp>
#include "support/recording_los_backend.hpp"
#include "join_observer.hpp"
#include "public_navigation_demo.hpp"

#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <irmel/library/irmel-types/ImageListener.h>
#include <irmel/library/irmel-types/FrameHeader.h>

#include <array>
#include <future>
#include <iostream>
#include <limits>
#include <thread>

namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
namespace b = gige_mel::backend;
namespace r = recording_los;
using namespace std::chrono_literals;
namespace {
#define CHECK(e) do { if (!(e)) throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) + ": " #e); } while (false)
template<class T> using Callback = std::function<void(ir::Channel&, const T*)>;

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false}, opened{false}, expired{false};
    void hold() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        if (!changed.wait_for(lock, 3s, [&] { return opened; })) expired = true;
    }
    void wait() {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 3s, [&] { return entered; }));
    }
    void open() { std::lock_guard lock(mutex); opened = true; changed.notify_all(); }
    void check() { std::lock_guard lock(mutex); CHECK(!expired); }
};
struct OpenOnExit { Gate& gate; ~OpenOnExit() { gate.open(); } };

struct Recorder {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<ir::LineOfSightEuler> eulers;
    std::vector<ir::LineOfSightReport> reports;
    void euler(const ir::LineOfSightEuler* v) { std::lock_guard lock(mutex); eulers.push_back(*v); changed.notify_all(); }
    void report(const ir::LineOfSightReport* v) { std::lock_guard lock(mutex); reports.push_back(*v); changed.notify_all(); }
    void wait(std::size_t e, std::size_t p) {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 3s, [&] { return eulers.size() >= e && reports.size() >= p; }));
    }
    std::pair<std::size_t, std::size_t> counts() {
        std::lock_guard lock(mutex); return {eulers.size(), reports.size()};
    }
    void expect(std::size_t e, std::size_t p) { CHECK((counts() == std::pair{e, p})); }
    // Independent oracle: constants do NOT read or convert the injected model.
    void exact(std::chrono::nanoseconds time) {
        std::lock_guard lock(mutex);
        const auto& e = eulers.back();
        CHECK(e.getSystemTime() == time);
        CHECK(e.getAttitude().getRoll() == 0.123456789012345);
        CHECK(e.getAttitude().getPitch() == -0.234567890123456);
        CHECK(e.getAttitude().getYaw() == 0.345678901234567);
        CHECK(e.getAttitudeRates().getRoll() == -0.456789012345678);
        CHECK(e.getAttitudeRates().getPitch() == 0.567890123456789);
        CHECK(e.getAttitudeRates().getYaw() == -0.678901234567890);
        const auto& p = reports.back();
        CHECK(p.getSystemTime() == time);
        CHECK(p.getPointingAngle().az == 0.789012345678901);
        CHECK(p.getPointingAngle().el == -0.890123456789012);
        CHECK(p.getPointingAngleRates().az == -0.901234567890123);
        CHECK(p.getPointingAngleRates().el == 1.012345678901234);
        CHECK(p.getImageRotation() == -1.123456789012345);
        CHECK(p.getValidityFlagBitfield() == 0);
        // Defaults are only storage; validity zero does not prove false status.
        CHECK(!p.getAtSpeed() && !p.getInTolerance());
        CHECK(p.getPlatformAttitude().getRoll() == 0);
        CHECK(p.getPlatformAttitude().getPitch() == 0);
        CHECK(p.getPlatformAttitude().getYaw() == 0);
    }
};

class Listener final : public ir::ImageListener {
public:
    std::function<void()> hook; // immutable while enabled
    std::mutex mutex;
    std::condition_variable changed;
    unsigned count{0};
    bool empty_navigation{true};
    void onImage(const ir::Channel&, const ir::FrameHeader& h, std::shared_ptr<ir::Buffer> buffer) override {
        if (hook) hook();
        const auto released = buffer->release();
        std::lock_guard lock(mutex);
        empty_navigation = empty_navigation && h.getSensorNavState().empty() && h.getSensorInertialState().empty() &&
            released == ir::Return::Success;
        ++count;
        changed.notify_all();
    }
    void wait() {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 3s, [&] { return count != 0; }));
        CHECK(empty_navigation);
    }
};

struct Fixture {
    // Consumer memory outlives channels/workers.
    std::array<std::byte, 64 * 48> storage{};
    std::shared_ptr<r::Recording> recording;
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::shared_ptr<ir::ImageChannel> image;
    std::shared_ptr<Listener> listener{std::make_shared<Listener>()};
    std::shared_ptr<ir::Buffer> buffer;
    explicit Fixture(std::shared_ptr<r::Recording> rec = std::make_shared<r::Recording>()) : recording(std::move(rec)) {
        auto backend = recording ? std::shared_ptr<b::ICameraBackend>{std::make_shared<r::Backend>(recording)} : b::make_synthetic_backend();
        manager = gige_mel::make_api_manager({}, backend);
        control = ir::getControl("synthetic", manager);
        CHECK(control && control->init() == ir::Return::Success);
        attach();
    }
    ~Fixture() {
        if (control && image) (void)control->detachChannel(image);
        control.reset();
        image.reset();
        manager.reset();
    }
    ir::Config config(ir::ChannelType type) {
        for (const auto& c : control->getCapabilities()) {
            if (c.getChannelTypes() != std::vector{type}) continue;
            ir::Config v;
            v.setChanID(c.getChanID()); v.setChannelType(type); v.setImgLstnr(listener);
            return v;
        }
        throw std::runtime_error("missing capability");
    }
    void attach() {
        image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(config(ir::ChannelType::IRSTImage)));
        CHECK(image);
        buffer = ir::getBuffer("synthetic", manager);
        CHECK(buffer->init(storage.data(), storage.size(), 43) == ir::Return::Success);
        CHECK(image->registerBuffer(buffer) == ir::Return::Success);
    }
    std::shared_ptr<r::Subscription> start() {
        CHECK(image->enable() == ir::Return::Success);
        CHECK(recording && recording->latest);
        recording->latest->wait_receives(1);
        return recording->latest;
    }
    void callbacks(Recorder& recorder) {
        CHECK(image->registerMetadataCallback(Callback<ir::LineOfSightEuler>{[&](auto&, auto* v) { recorder.euler(v); }}) == ir::Return::Success);
        CHECK(image->registerMetadataCallback(Callback<ir::LineOfSightReport>{[&](auto&, auto* v) { recorder.report(v); }}) == ir::Return::Success);
    }
};

void capabilities() {
    for (unsigned mask = 0; mask != 5; ++mask) {
        auto rec = mask == 4 ? std::shared_ptr<r::Recording>{} : std::make_shared<r::Recording>();
        const bool e = mask != 4 && (mask & 1), p = mask != 4 && (mask & 2);
        if (rec) rec->declarations = r::support(e, p);
        Recorder observed;
        Fixture f(rec);
        const auto metadata = f.image->getCapabilities().getChannelMetadataCapabilities();
        for (const auto& c : f.control->getCapabilities())
            if (c.getChannelTypes() == std::vector{ir::ChannelType::IRSTImage}) CHECK(metadata == c.getChannelMetadataCapabilities());
        CHECK(metadata.contains(ir::ChannelMetadataCapabilityType::LineOfSightEuler) == e);
        CHECK(metadata.contains(ir::ChannelMetadataCapabilityType::LineOfSightReport) == p);
        CHECK(!metadata.contains(ir::ChannelMetadataCapabilityType::LineOfSightQuaternion));
        CHECK(!metadata.contains(ir::ChannelMetadataCapabilityType::LOS3DKinematicsType));
        auto ec = Callback<ir::LineOfSightEuler>{[&](auto&, auto* v) { observed.euler(v); }};
        auto pc = Callback<ir::LineOfSightReport>{[&](auto&, auto* v) { observed.report(v); }};
        CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightEuler>{}) == (e ? ir::Return::BadPointer : ir::Return::NotSupported));
        CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightReport>{}) == (p ? ir::Return::BadPointer : ir::Return::NotSupported));
        CHECK(f.image->registerMetadataCallback(ec) == (e ? ir::Return::Success : ir::Return::NotSupported));
        CHECK(f.image->registerMetadataCallback(pc) == (p ? ir::Return::Success : ir::Return::NotSupported));
        CHECK(f.image->registerMetadataCallback(ec) == (e ? ir::Return::Fail : ir::Return::NotSupported));
        CHECK(f.image->registerMetadataCallback(pc) == (p ? ir::Return::Fail : ir::Return::NotSupported));
        CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightQuaternion>{}) == ir::Return::NotSupported);
        CHECK(f.image->registerMetadataCallback(Callback<ir::LOS3D_KinematicsType>{}) == ir::Return::NotSupported);
        CHECK(f.image->enable() == ir::Return::Success);
        if (rec) CHECK(rec->opens == (e || p ? 1U : 0U));
        if (e || p) {
            rec->latest->deliver(r::observation());
            observed.expect(e ? 1U : 0U, p ? 1U : 0U);
        }
        CHECK(f.image->disable() == ir::Return::Success);
    }
    for (unsigned invalid = 0; invalid != 6; ++invalid) {
        auto rec = std::make_shared<r::Recording>();
        switch (invalid) {
        case 0: rec->declarations.frame = b::LosFrame::unknown; break;
        case 1: rec->declarations.time_domain = b::LosTimeDomain::unknown; break;
        case 2: rec->declarations.epoch = b::LosEpoch::unknown; break;
        case 3: rec->declarations.source_id.clear(); break;
        case 4: rec->declarations.provenance.clear(); break;
        case 5: rec->declarations.epoch_meaning.clear(); break;
        }
        Fixture f(rec);
        auto metadata = f.image->getCapabilities().getChannelMetadataCapabilities();
        CHECK(!metadata.contains(ir::ChannelMetadataCapabilityType::LineOfSightEuler));
        CHECK(!metadata.contains(ir::ChannelMetadataCapabilityType::LineOfSightReport));
        CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightEuler>{}) == ir::Return::NotSupported);
        CHECK(f.image->enable() == ir::Return::Success);
        CHECK(rec->opens == 0);
    }
}

void payload_and_time() {
    Recorder recorder;
    Recorder attached;
    Fixture f;
    f.callbacks(recorder); // registration before enable; no initial replay
    recorder.expect(0, 0);
    auto s = f.start();
    recorder.expect(0, 0);
    CHECK(f.image->send(navigation_test_data()).get());
    s->deliver(b::LosObservation{}); // receive barrier; navigation is NOT LOS
    recorder.expect(0, 0);
    s->deliver(r::observation());
    recorder.expect(1, 1);
    recorder.exact(123456789012345ns);
    s->deliver(r::observation()); // repeat
    s->deliver(r::observation(123456789012344LL)); // regression
    recorder.expect(1, 1);

    // Whole observation failures must neither publish nor poison ordering.
    const std::vector<std::function<void(b::LosObservation&)>> malformed{
        [](auto& v) { v.time.reset(); }, [](auto& v) { v.time = 0ns; }, [](auto& v) { v.time = -1ns; },
        [](auto& v) { v.time_domain = b::LosTimeDomain::unknown; },
        [](auto& v) { v.epoch = b::LosEpoch::unknown; },
        [](auto& v) { v.frame = b::LosFrame::unknown; },
        [](auto& v) { v.source_id = "other source"; },
        [](auto& v) { v.clock_generation = 8; },
        [](auto& v) { v.clock_generation = 0; },
        [](auto& v) { v.subscription_id = 0; },
        [](auto& v) { v.euler.reset(); v.report.reset(); }
    };
    for (const auto& corrupt : malformed) {
        auto v = r::observation(123456789012346LL);
        v.subscription_id = s->id;
        corrupt(v);
        s->deliver(v, false);
        recorder.expect(1, 1);
    }
    // Independently validate each representation: malformed Euler must not
    // discard a valid Report, and vice versa. Exercise every numeric member.
    std::int64_t time = 123456789012346LL;
    std::size_t e = 1, p = 1;
    for (unsigned i = 0; i != 6; ++i) {
        auto v = r::observation(time++);
        auto& a = i < 3 ? v.euler->attitude : v.euler->rates;
        const auto bad = i % 2 ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
        if (i % 3 == 0) a.roll = bad;
        if (i % 3 == 1) a.pitch = bad;
        if (i % 3 == 2) a.yaw = bad;
        s->deliver(v); recorder.expect(e, ++p);
    }
    const std::vector<std::function<void(b::LosPointing&)>> bad_report{
        [](auto& v) { v.angles.reset(); }, [](auto& v) { v.rates.reset(); }, [](auto& v) { v.image_rotation.reset(); },
        [](auto& v) { v.angles->azimuth = std::numeric_limits<double>::quiet_NaN(); },
        [](auto& v) { v.angles->elevation = std::numeric_limits<double>::infinity(); },
        [](auto& v) { v.rates->azimuth = -std::numeric_limits<double>::infinity(); },
        [](auto& v) { v.rates->elevation = std::numeric_limits<double>::quiet_NaN(); },
        [](auto& v) { v.image_rotation = std::numeric_limits<double>::infinity(); },
        [](auto& v) { v.validity_flags = 1; }, [](auto& v) { v.validity_flags = 0xFFFFFFFFU; },
        [](auto& v) { v.at_speed = false; }, [](auto& v) { v.in_tolerance = true; },
        [](auto& v) { v.platform_attitude = b::LosEulerAngles{1, 2, 3}; }
    };
    for (const auto& corrupt : bad_report) {
        auto v = r::observation(time++); corrupt(*v.report);
        s->deliver(v); recorder.expect(++e, p);
    }
    auto last = r::observation(std::numeric_limits<std::int64_t>::max());
    s->deliver(last); recorder.expect(++e, ++p);
    recorder.exact(std::chrono::nanoseconds::max()); // no double conversion of time
    f.listener->wait(); // concurrent images, still no frame navigation
    CHECK(f.image->disable() == ir::Return::Success);
    s->wait_destroyed();

    // Restart establishes new clock generation, requires fresh events. Retired
    // subscription and clock values cannot replay across enable or attachment.
    f.recording->generation = 8;
    auto next = f.start();
    recorder.expect(e, p);
    auto v = r::observation(11);
    v.clock_generation = 8;
    v.subscription_id = s->id;
    next->deliver(v, false); recorder.expect(e, p);
    v.clock_generation = 7;
    next->deliver(v); recorder.expect(e, p);
    v.clock_generation = 8;
    next->deliver(v); recorder.expect(++e, ++p);
    recorder.exact(11ns);
    CHECK(f.control->detachChannel(f.image) == ir::Return::Success);
    next->wait_destroyed();
    CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightEuler>{}) == ir::Return::Fail);
    f.attach();
    f.callbacks(attached);
    auto fresh = f.start();
    attached.expect(0, 0);
    v.subscription_id = next->id;
    fresh->deliver(v, false); attached.expect(0, 0);
    fresh->deliver(v); attached.expect(1, 1);
    CHECK(f.image->disable() == ir::Return::Success);
}

void faults_and_reentry() {
    Recorder recorder;
    Gate health_called;
    std::atomic<unsigned> reentries{0};
    Fixture f;
    auto check_reentry = [&] {
        CHECK(f.image->disable() == ir::Return::Fail);
        CHECK(f.image->enable() == ir::Return::Fail);
        CHECK(f.control->detachChannel(f.image) == ir::Return::Fail);
        CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightEuler>{[](auto&, auto*) {}}) == ir::Return::Fail);
        CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightReport>{[](auto&, auto*) {}}) == ir::Return::Fail);
        CHECK(!f.image->send(mel::NavigationReport{}).get());
        ++reentries;
    };
    CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightEuler>{[&](auto&, auto* v) {
        check_reentry(); recorder.euler(v); throw std::runtime_error("test consumer");
    }}) == ir::Return::Success);
    CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightReport>{[&](auto&, auto* v) { recorder.report(v); }}) == ir::Return::Success);
    f.listener->hook = check_reentry;
    auto c2 = std::dynamic_pointer_cast<ir::C2Channel>(f.control->attachChannel(f.config(ir::ChannelType::CommandAndControl)));
    CHECK(c2->registerMetadataCallback(Callback<ir::CommandStatus>{[&](auto&, auto*) { check_reentry(); }}) == ir::Return::Success);
    CHECK(!c2->send(ir::EraseCommand{}).get());
    auto health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(f.control->attachChannel(f.config(ir::ChannelType::HealthAndStatus)));
    CHECK(health->registerMetadataCallback(Callback<mel::MFA_Status>{[&](auto&, auto*) { check_reentry(); health_called.open(); health_called.hold(); }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    health_called.wait();
    auto s = f.start();
    s->deliver(r::observation()); recorder.expect(1, 1);
    CHECK(gige_mel::provider_statistics(f.manager).callback_failures == 1);
    s->deliver(r::observation(123456789012346LL)); recorder.expect(2, 2);
    CHECK(gige_mel::provider_statistics(f.manager).callback_failures == 2);
    f.listener->wait();
    CHECK(reentries >= 5);
    s->fault();
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (gige_mel::provider_statistics(f.manager).last_error.find("deliberate recording fault") == std::string::npos) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    recorder.expect(2, 2);
    // Disable must cancel a failed or waiting source, not affect image headers.
    CHECK(f.image->disable() == ir::Return::Success);
    CHECK(gige_mel::provider_statistics(f.manager).callback_failures == 2); // no swallowed CHECK in reentry
    s->wait_destroyed();
    CHECK(health->disable() == ir::Return::Success);
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    CHECK(f.control->detachChannel(c2) == ir::Return::Success);

    f.recording->fail_open = true;
    CHECK(f.image->enable() == ir::Return::Fail);
    CHECK(f.image->disable() == ir::Return::Success);
    f.recording->fail_open = false;
    f.recording->generation = 0;
    CHECK(f.image->enable() == ir::Return::Fail);
    CHECK(f.image->disable() == ir::Return::Success);
}

void coalescing_and_detach(bool detach, bool throwing = false) {
    Recorder recorder;
    Gate gate;
    Fixture f;
    CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightEuler>{[&](auto&, auto* v) {
        recorder.euler(v); gate.hold();
        if (throwing) throw std::runtime_error("gated LOS callback failure");
    }}) == ir::Return::Success);
    CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightReport>{[&](auto&, auto* v) { recorder.report(v); }}) == ir::Return::Success);
    auto s = f.start();
    s->push(r::observation());
    OpenOnExit release{gate};
    gate.wait();
    for (unsigned i = 0; i != 100; ++i) s->push(r::observation(123456789012346LL + i));
    { std::lock_guard lock(s->mutex); CHECK(s->coalesced == 99); }
    // Prove operations and registration locks are not held by consumer dispatch.
    auto unlocked = std::async(std::launch::async, [&] {
        auto channel = f.control->attachChannel(f.config(ir::ChannelType::HealthAndStatus));
        CHECK(channel && f.control->detachChannel(channel) == ir::Return::Success);
        CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightEuler>{[](auto&, auto*) {}}) == ir::Return::Fail);
    });
    CHECK(unlocked.wait_for(3s) == std::future_status::ready); unlocked.get();
    if (!detach) {
        gate.open();
        // A marker after the coalesced latest proves completion deterministically.
        recorder.wait(2, 2);
        s->deliver(r::observation(123456789012500LL));
        recorder.expect(3, 3); // first, latest, marker; no unlimited queue
        CHECK(f.image->disable() == ir::Return::Success);
    } else {
        std::atomic<bool> joining{false};
        auto future = std::async(std::launch::async, [&] {
            test_join::Observe observer(joining);
            return f.control->detachChannel(f.image);
        });
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!joining) { CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
        CHECK(future.wait_for(0ms) == std::future_status::timeout);
        gate.open();
        CHECK(future.wait_for(3s) == std::future_status::ready && future.get() == ir::Return::Success);
        recorder.expect(1, 0); // retired before Report or queued latest admission
        s->push(r::observation(123456789012900LL));
        CHECK(!f.image->sendKeepAliveRep().get());
        recorder.expect(1, 0);
    }
    gate.check();
    s->wait_destroyed();
    CHECK(gige_mel::provider_statistics(f.manager).callback_failures == (throwing ? 1U : 0U));
}

void cancellation_during_delivery() {
    Recorder recorder;
    Fixture f;
    f.callbacks(recorder);
    auto s = f.start();
    { std::lock_guard lock(s->mutex); s->gate_delivery = true; }
    s->push(r::observation());
    s->wait_delivering();
    CHECK(f.control->detachChannel(f.image) == ir::Return::Success);
    s->wait_destroyed();
    recorder.expect(0, 0);
}

void standby_cancels() {
    Recorder recorder;
    Fixture f;
    f.callbacks(recorder);
    auto c2 = std::dynamic_pointer_cast<ir::C2Channel>(f.control->attachChannel(f.config(ir::ChannelType::CommandAndControl)));
    auto s = f.start();
    s->deliver(r::observation()); recorder.expect(1, 1);
    ir::ModeCmd command;
    command.setState(mel::MFA_State::Standby);
    command.setMode(ir::MFA_Mode::Unused);
    CHECK(c2->send(command).get());
    s->wait_destroyed();
    auto next = f.start();
    next->deliver(r::observation(1)); recorder.expect(2, 2);
    CHECK(f.image->disable() == ir::Return::Success);
    CHECK(f.control->detachChannel(c2) == ir::Return::Success);
}

void retained_channel_teardown() {
    Recorder recorder;
    Gate gate;
    test_join::SelfDetach reaper;
    auto recording = std::make_shared<r::Recording>();
    Fixture f(recording);
    std::weak_ptr<ir::Control> weak = f.control;
    CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightEuler>{[&](auto&, auto* v) {
        reaper.arm(); recorder.euler(v); gate.hold();
    }}) == ir::Return::Success);
    auto s = f.start(); reaper.started();
    s->push(r::observation());
    OpenOnExit release{gate}; gate.wait();
    f.control.reset(); // admitted callback pins Control while caller releases it
    CHECK(!weak.expired());
    gate.open(); reaper.join(); gate.check();
    CHECK(weak.expired());
    s->wait_destroyed();
    CHECK(f.image->enable() == ir::Return::Fail);
    CHECK(f.image->registerMetadataCallback(Callback<ir::LineOfSightReport>{}) == ir::Return::Fail);
    CHECK(!f.image->sendKeepAliveRep().get());
    recorder.expect(1, 0);
    f.image.reset(); f.manager.reset();
    CHECK(recording->devices_destroyed == 1);
}
} // namespace

int main() {
    try {
        test_join::prepare();
        capabilities(); std::cout << "PASS LOS capabilities and registrations\n";
        payload_and_time(); std::cout << "PASS LOS exact payload and time continuity\n";
        faults_and_reentry(); std::cout << "PASS LOS faults, exceptions and callback contexts\n";
        coalescing_and_detach(false); std::cout << "PASS LOS bounded latest coalescing\n";
        coalescing_and_detach(true); std::cout << "PASS LOS concurrent detach drain\n";
        coalescing_and_detach(true, true); std::cout << "PASS LOS throwing callback detach drain\n";
        cancellation_during_delivery(); std::cout << "PASS LOS cancellation during owned delivery\n";
        standby_cancels(); std::cout << "PASS LOS Standby cancellation\n";
        retained_channel_teardown(); std::cout << "PASS LOS Control/source destruction\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}