// GIGE-MEL-047: optional camera-profile navigation handler, reusable bridge,
// generic recording backend, the unchanged MEL ImageChannel::send entry point
// and dynamically loaded modules. SDK-free. The fixture record is fictional and
// a recorded operation is fixture acceptance only: no transport, detector
// processing, image association or transfer alignment is exercised or claimed.
#include <gige_mel/provider.hpp>
#include "public_navigation_demo.hpp"
#include "navigation_record_policy.hpp"
#include "support/alternate_camera_profile.hpp"
#include "support/module_unload_probe.hpp"
#include "support/profile_navigation_backend.hpp"
#include "selection/profile_loader.hpp"
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <atomic>
#include <bit>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
namespace gb = gige_mel::backend;
namespace gp = gb::profiles;
namespace fx = profile_navigation_fixture;
namespace nr = navigation_record;
using namespace std::chrono_literals;

namespace {
#define CHECK(expression)                                                                                  \
    do {                                                                                                   \
        if (!(expression))                                                                                 \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #expression); \
    } while (false)

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

template <class T> std::shared_ptr<T> response(mel::RequestFor<T> future) {
    CHECK(future.wait_for(0ms) == std::future_status::ready);
    auto result = future.get();
    CHECK(result && result.get());
    return result.get();
}

template <class T> void expect_error(mel::RequestFor<T> future, mel::ErrorCode code, std::string_view part = {}) {
    CHECK(future.wait_for(0ms) == std::future_status::ready);
    auto result = future.get();
    CHECK(!result);
    CHECK(result.getError().getCode() == code);
    if (!contains(result.getError().getDescription(), part))
        throw std::runtime_error("expected '" + std::string(part) + "' in: " + result.getError().getDescription());
}

template <class T> T take(gb::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

void expect_status(const gb::Status& status, gb::ErrorCode code, std::string_view part = {}) {
    CHECK(!status);
    CHECK(status.error().code == code);
    if (!contains(status.error().message, part))
        throw std::runtime_error("expected '" + std::string(part) + "' in: " + status.error().message);
}

// Independently specified copy of navigation_test_data(); not derived from conversion code.
gb::NavigationReport expected_report(std::chrono::nanoseconds time,
                                     gb::NavigationSolutionState state = gb::NavigationSolutionState::gps) {
    gb::NavigationReport r;
    r.system_time = time;
    r.state = state;
    r.latitude = 0.5; r.longitude = -1.0; r.altitude = 1234.5; r.altitude_msl = 1200.25;
    r.attitude = {0.1, -0.2, 0.3};
    r.attitude_rate = {{0.01, -0.02, 0.03}, 7ns};
    r.speed = {10, -20, 30}; r.acceleration = {-1, 2, -3};
    r.wander_angle = -0.1; r.magnetic_heading = 0.7;
    r.position_velocity_covariance_uncertainty = {
        1, -0.1, 0.2, 2, -0.3, 3, -0.4, 0.5, -0.6, 0.7, -0.8, 0.9, 4, -1, 1.1, 5, -1.2, 6};
    return r;
}

mel::NavigationReport mel_report(std::chrono::nanoseconds time,
                                 mel::PositionSolutionState state = mel::PositionSolutionState::GPS) {
    auto report = navigation_test_data(time);
    report.setState(state);
    return report;
}

// Check the fictional fixture record field by field against the report.
void check_record(const gp::NavigationOperation& operation, const gb::NavigationReport& expected) {
    CHECK(operation.format == nr::format);
    CHECK(operation.report == expected);
    const auto fields = nr::decode(operation.payload);
    CHECK(fields.has_value());
    const auto bits = [](double value) { return std::bit_cast<std::uint64_t>(value); };
    const std::vector<std::pair<std::uint16_t, std::uint64_t>> wanted{
        {0x0001, static_cast<std::uint64_t>(expected.system_time.count())},
        {0x0002, nr::state_code(expected.state)},
        {0x0010, bits(expected.latitude)}, {0x0011, bits(expected.longitude)}, {0x0012, bits(expected.altitude)},
        {0x0020, bits(expected.attitude.roll)}, {0x0021, bits(expected.attitude.pitch)},
        {0x0022, bits(expected.attitude.yaw)},
        {0x0030, bits(expected.position_velocity_covariance_uncertainty.position_position_pn_pn)},
        {0x0031, bits(expected.position_velocity_covariance_uncertainty.position_position_pe_pe)},
        {0x0032, bits(expected.position_velocity_covariance_uncertainty.position_position_pd_pd)}};
    CHECK(*fields == wanted);
}

struct TestCleanup {
    std::function<void()> cleanup;
    ~TestCleanup() { cleanup(); }
};

struct BridgeFixture {
    std::shared_ptr<nr::Controls> controls{std::make_shared<nr::Controls>()};
    std::shared_ptr<fx::Recording> recording{std::make_shared<fx::Recording>()};
    std::unique_ptr<gp::ProfileNavigationBridge> bridge;
    explicit BridgeFixture(std::shared_ptr<const gp::ICameraProfile> profile = {}) {
        if (!profile) profile = nr::make(controls);
        fx::StaticFeatures features;
        auto state = take(profile->open(features));
        bridge = take(gp::ProfileNavigationBridge::create(std::move(state), std::make_unique<fx::RecordingPort>(recording)));
    }
};

void bridge_capability_discovery() {
    fx::StaticFeatures features;
    auto recording = std::make_shared<fx::Recording>();
    auto null_state = gp::ProfileNavigationBridge::create({}, std::make_unique<fx::RecordingPort>(recording));
    CHECK(!null_state && null_state.error().code == gb::ErrorCode::invalid_argument);
    // Profile A variants: the built-in policy and a distinct test policy. No fabricated handler.
    for (const auto& profile : {gp::make_mono_free_running_profile(), alternate_profile::make()}) {
        auto state = take(profile->open(features));
        CHECK(gp::navigation_handler(*state) == nullptr);
        auto refused = gp::ProfileNavigationBridge::create(state, std::make_unique<fx::RecordingPort>(recording));
        CHECK(!refused && refused.error().code == gb::ErrorCode::unsupported);
    }
    auto controls = std::make_shared<nr::Controls>();
    auto state = take(nr::make(controls)->open(features));
    CHECK(gp::navigation_handler(*state) != nullptr);
    CHECK(state->capabilities().maximum_frame_rate_hz == 20); // ordinary camera behavior retained
    auto null_port = gp::ProfileNavigationBridge::create(state, nullptr);
    CHECK(!null_port && null_port.error().code == gb::ErrorCode::invalid_argument);
    // Refused creation retires the offered port immediately and invokes nothing.
    CHECK(controls->entered == 0 && recording->ports_created == 3 && recording->ports_destroyed == 3);
}

void bridge_preparation_and_mapping() {
    using Fault = nr::Controls::Fault;
    BridgeFixture f;
    const auto gps = expected_report(10ns);
    CHECK(f.bridge->submit(gps));
    CHECK(f.recording->operation_count() == 1);
    check_record(f.recording->operations[0], gps);
    for (const auto state : {gb::NavigationSolutionState::free_inertial, gb::NavigationSolutionState::blended}) {
        const auto report = expected_report(11ns, state);
        CHECK(f.bridge->submit(report));
        check_record(f.recording->operations.back(), report);
    }
    // Profile policy rejections and preparation failures never reach the port.
    const auto rejected = [&](gb::NavigationReport report, gb::ErrorCode code, std::string_view part) {
        const auto before = f.recording->snapshot_events().size();
        expect_status(f.bridge->submit(report), code, part);
        CHECK(f.recording->snapshot_events().size() == before && f.recording->operation_count() == 3);
    };
    rejected(expected_report(12ns, gb::NavigationSolutionState::aligning), gb::ErrorCode::unsupported, "Aligning");
    auto noisy = expected_report(12ns);
    noisy.position_velocity_covariance_uncertainty.position_position_pe_pe = nr::maximum_position_variance * 2;
    rejected(noisy, gb::ErrorCode::invalid_argument, "variance");
    const std::pair<Fault, std::pair<gb::ErrorCode, std::string_view>> faults[]{
        {Fault::error, {gb::ErrorCode::io_error, "injected preparation error"}},
        {Fault::throw_runtime, {gb::ErrorCode::invalid_state, "handler threw before submission"}},
        {Fault::throw_allocation, {gb::ErrorCode::resource_exhausted, "allocation"}},
        {Fault::empty_format, {gb::ErrorCode::invalid_state, "without a format"}},
        {Fault::alter_report, {gb::ErrorCode::invalid_state, "altered the report"}}};
    for (const auto& [fault, expected] : faults) {
        f.controls->fault = fault;
        rejected(expected_report(12ns), expected.first, expected.second);
    }
    f.controls->fault = Fault::none;
    // Definite refusal is not sticky; the same report can be submitted explicitly.
    f.recording->set_mode(fx::PortMode::refuse);
    expect_status(f.bridge->submit(expected_report(12ns)), gb::ErrorCode::busy, "did not submit");
    f.recording->set_mode(fx::PortMode::accept);
    CHECK(f.bridge->submit(expected_report(12ns)));
    CHECK(f.recording->operation_count() == 4 && f.controls->rejected == 2);
}

void bridge_uncertainty_and_control_loss() {
    for (const auto mode : {fx::PortMode::uncertain, fx::PortMode::throw_after_entry}) {
        BridgeFixture f;
        f.recording->set_mode(mode);
        expect_status(f.bridge->submit(expected_report(1ns)), gb::ErrorCode::timeout, "uncertain");
        CHECK(f.recording->operation_count() == 1); // the effect may have happened
        f.recording->set_mode(fx::PortMode::accept);
        const auto entered = f.controls->entered.load();
        expect_status(f.bridge->submit(expected_report(2ns)), gb::ErrorCode::invalid_state, "unresolved");
        CHECK(f.controls->entered == entered && f.recording->operation_count() == 1); // no automatic retry
    }
    BridgeFixture lost;
    lost.recording->set_mode(fx::PortMode::lost_after_accept);
    expect_status(lost.bridge->submit(expected_report(1ns)), gb::ErrorCode::timeout, "after control loss");
    lost.recording->set_mode(fx::PortMode::accept);
    expect_status(lost.bridge->submit(expected_report(2ns)), gb::ErrorCode::disconnected, "control was lost");
    CHECK(lost.recording->operation_count() == 1 && lost.controls->entered == 1);
    BridgeFixture before;
    before.recording->set_mode(fx::PortMode::lost_before_send);
    expect_status(before.bridge->submit(expected_report(1ns)), gb::ErrorCode::disconnected, "control lost before");
    expect_status(before.bridge->submit(expected_report(2ns)), gb::ErrorCode::disconnected);
    CHECK(before.recording->operation_count() == 0 && before.controls->entered == 1);
}

void bridge_concurrency_and_teardown() {
    // Concurrent submission is refused (no queue); reentry is refused.
    {
        BridgeFixture f;
        f.recording->port_gate.arm();
        auto first = std::async(std::launch::async, [&] { return f.bridge->submit(expected_report(1ns)); });
        TestCleanup release{[&] { f.recording->port_gate.release(); }};
        CHECK(f.recording->port_gate.wait_entered());
        expect_status(f.bridge->submit(expected_report(2ns)), gb::ErrorCode::busy, "no queue");
        f.recording->port_gate.release();
        CHECK(first.get());
        CHECK(f.recording->operation_count() == 1 && f.recording->port_gate.clean());
        gb::Status nested;
        bool shutdown_from_handler = true;
        f.controls->on_prepare = [&] {
            nested = f.bridge->submit(expected_report(3ns));
            shutdown_from_handler = f.bridge->shutdown();
        };
        CHECK(f.bridge->submit(expected_report(4ns)));
        f.controls->on_prepare = {};
        expect_status(nested, gb::ErrorCode::invalid_state, "reentrant");
        CHECK(!shutdown_from_handler && !f.bridge->retired()); // refusal changed nothing
    }
    // Shutdown racing an admitted submission: admission closes immediately, the
    // active operation drains, then the port and state retire, in that order,
    // after the last external profile and state references were already dropped.
    for (unsigned cycle = 0; cycle < 25; ++cycle) {
        auto controls = std::make_shared<nr::Controls>();
        auto recording = std::make_shared<fx::Recording>();
        std::unique_ptr<gp::ProfileNavigationBridge> bridge;
        {
            auto profile = nr::make(controls);
            fx::StaticFeatures features;
            auto state = take(profile->open(features));
            bridge = take(gp::ProfileNavigationBridge::create(state, std::make_unique<fx::RecordingPort>(recording)));
        } // profile and every external state reference are gone; the bridge owns the state
        CHECK(controls->states == 1 && controls->retired == 0);
        recording->port_gate.arm();
        auto submission = std::async(std::launch::async, [&] { return bridge->submit(expected_report(1ns)); });
        TestCleanup release{[&] { recording->port_gate.release(); }};
        CHECK(recording->port_gate.wait_entered());
        std::promise<void> started;
        auto closing = std::async(std::launch::async, [&] { started.set_value(); return bridge->shutdown(); });
        started.get_future().wait();
        // Deterministic: retirement cannot complete while the port is held.
        CHECK(closing.wait_for(20ms) == std::future_status::timeout);
        CHECK(controls->retired == 0 && recording->ports_destroyed == 0);
        recording->port_gate.release();
        CHECK(submission.get()); // the admitted operation completes normally
        CHECK(closing.get());
        CHECK(bridge->retired() && controls->retired == 1 && recording->ports_destroyed == 1);
        const auto events = recording->snapshot_events();
        CHECK(events == (std::vector<std::string>{"port recorded", "port retired"}));
        expect_status(bridge->submit(expected_report(2ns)), gb::ErrorCode::disconnected, "retired");
        CHECK(bridge->shutdown() && controls->entered == 1);
        bridge.reset();
        CHECK(recording->port_gate.clean());
    }
    // A submission arriving after admission closed never invokes the handler.
    BridgeFixture closed;
    CHECK(closed.bridge->shutdown());
    expect_status(closed.bridge->submit(expected_report(1ns)), gb::ErrorCode::disconnected);
    CHECK(closed.controls->entered == 0 && closed.controls->retired == 1);
}

class CountingListener final : public ir::ImageListener {
public:
    void onImage(const ir::Channel&, const ir::FrameHeader&, std::shared_ptr<ir::Buffer> buffer) override {
        ++frames;
        (void)buffer->release();
    }
    std::atomic<unsigned> frames{0};
};

using NavigationCallback = std::function<void(ir::Channel&, const ir::NavigationReportResp*)>;

// Public MEL path: make_api_manager -> Control::init -> attach Image -> send.
struct MelSession {
    std::vector<std::vector<std::byte>> storage;
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::shared_ptr<ir::ImageChannel> image;
    std::vector<std::shared_ptr<ir::Buffer>> buffers;
    std::shared_ptr<CountingListener> listener{std::make_shared<CountingListener>()};

    explicit MelSession(std::shared_ptr<gb::ICameraBackend> backend) {
        gige_mel::ProviderSettings settings;
        settings.instance = "profile-navigation";
        manager = gige_mel::make_api_manager(settings, std::move(backend));
        CHECK(manager);
        control = ir::getControl(settings.instance, manager);
        CHECK(control && control->init() == ir::Return::Success);
        attach();
    }
    ~MelSession() {
        try { if (image) (void)image->disable(); } catch (...) {}
        try { if (control && image) (void)control->detachChannel(image); } catch (...) {}
        image.reset(); buffers.clear(); control.reset(); manager.reset();
    }
    void attach() {
        for (const auto& cap : control->getCapabilities()) {
            if (cap.getChannelTypes() != std::vector{ir::ChannelType::IRSTImage}) continue;
            CHECK(cap.getChannelMetadataCapabilities().contains(ir::ChannelMetadataCapabilityType::NavigationReportResp));
            ir::Config config;
            config.setChanID(cap.getChanID());
            config.setChannelType(ir::ChannelType::IRSTImage);
            config.setImgLstnr(listener);
            image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(config));
        }
        CHECK(image);
    }
    void add_buffers(unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            storage.emplace_back(64 * 48);
            auto buffer = ir::getBuffer("profile-navigation", manager);
            CHECK(buffer && buffer->init(storage.back().data(), storage.back().size(), i) == ir::Return::Success);
            CHECK(image->registerBuffer(buffer) == ir::Return::Success);
            buffers.push_back(std::move(buffer));
        }
    }
};

void mel_profile_a_receipt_only() {
    // Profile A selected through the same generic backend: no handler, so the
    // device exposes no INavigationSink and the provider keeps receipt-only behavior.
    for (const auto& profile : {gp::make_mono_free_running_profile(), alternate_profile::make()}) {
        auto recording = std::make_shared<fx::Recording>();
        MelSession s{std::make_shared<fx::ProfileNavigationBackend>(profile, recording)};
        unsigned calls = 0;
        CHECK(s.image->registerMetadataCallback(NavigationCallback{[&](auto&, auto*) { ++calls; }}) == ir::Return::Success);
        CHECK(response(s.image->send(mel_report(5ns)))->getSystemTime() == 5ns);
        CHECK(response(s.image->send(mel_report(6ns, mel::PositionSolutionState::Aligning)))->getSystemTime() == 6ns);
        expect_error(s.image->send(mel_report(6ns)), mel::ErrorCode::InvalidState, "strictly increase");
        CHECK(calls == 2);
        CHECK(recording->devices_opened == 1 && recording->sink_devices == 0 && recording->ports_created == 0);
        CHECK(recording->snapshot_events().empty());
    }
}

struct ProfileBSession {
    std::shared_ptr<nr::Controls> controls{std::make_shared<nr::Controls>()};
    std::shared_ptr<fx::Recording> recording{std::make_shared<fx::Recording>()};
    MelSession mel{std::make_shared<fx::ProfileNavigationBackend>(nr::make(controls), recording)};
};

void mel_profile_b_dispatch() {
    ProfileBSession b;
    auto& s = b.mel;
    CHECK(b.recording->sink_devices == 1 && b.recording->ports_created == 1);
    unsigned calls = 0;
    CHECK(s.image->registerMetadataCallback(NavigationCallback{[&](auto& channel, auto* reply) {
        CHECK(&channel == s.image.get() && reply && reply->getCommandID() == 0 && reply->getReqId() == 0);
        // Callback runs after backend acceptance and outside the sink.
        CHECK(b.recording->snapshot_events().back() == "port recorded");
        CHECK(reply->getSystemTime() == b.recording->operations.back().report.system_time);
        b.recording->event("callback");
        ++calls;
    }}) == ir::Return::Success);

    // Success with exact value preservation, three accepted states.
    const std::pair<mel::PositionSolutionState, gb::NavigationSolutionState> states[]{
        {mel::PositionSolutionState::FreeInertial, gb::NavigationSolutionState::free_inertial},
        {mel::PositionSolutionState::GPS, gb::NavigationSolutionState::gps},
        {mel::PositionSolutionState::Blended, gb::NavigationSolutionState::blended}};
    auto time = 123456789012345ns;
    for (const auto& [source, target] : states) {
        CHECK(response(s.image->send(mel_report(time, source)))->getSystemTime() == time);
        check_record(b.recording->operations.back(), expected_report(time, target));
        ++time;
    }
    CHECK(calls == 3 && b.recording->operation_count() == 3);
    CHECK(b.recording->snapshot_events() == (std::vector<std::string>{
        "sink entered", "port recorded", "callback", "sink entered", "port recorded", "callback",
        "sink entered", "port recorded", "callback"}));

    // Invalid and stale MEL reports never reach the sink or the handler.
    const auto entered = b.controls->entered.load();
    auto invalid = mel_report(time);
    invalid.setLatitude(2);
    expect_error(s.image->send(invalid), mel::ErrorCode::InvalidParameters, "latitude");
    expect_error(s.image->send(mel::NavigationReport{}), mel::ErrorCode::InvalidParameters);
    expect_error(s.image->send(mel_report(time - 1ns)), mel::ErrorCode::InvalidState, "strictly increase");
    CHECK(b.controls->entered == entered && b.recording->snapshot_events().size() == 9);

    // Profile-specific unsupported and policy rejection: definite, no operation,
    // no callback, no timestamp commit (the same time succeeds afterwards).
    expect_error(s.image->send(mel_report(time, mel::PositionSolutionState::Aligning)), mel::ErrorCode::Unsupported, "Aligning");
    auto noisy = mel_report(time);
    auto covariance = noisy.getPositionVelocityCovarianceUncertainty();
    covariance.setPositionPositionPdPd(nr::maximum_position_variance + 1);
    noisy.setPositionVelocityCovarianceUncertainty(covariance);
    expect_error(s.image->send(noisy), mel::ErrorCode::InvalidParameters, "variance");
    // Preparation errors and exceptions before any operation.
    using Fault = nr::Controls::Fault;
    const std::pair<Fault, mel::ErrorCode> faults[]{
        {Fault::error, mel::ErrorCode::InsufficientRemoteResources},
        {Fault::throw_runtime, mel::ErrorCode::InvalidState},
        {Fault::throw_allocation, mel::ErrorCode::InsufficientLocalResources},
        {Fault::alter_report, mel::ErrorCode::InvalidState}};
    for (const auto& [fault, code] : faults) {
        b.controls->fault = fault;
        expect_error(s.image->send(mel_report(time)), code);
    }
    b.controls->fault = Fault::none;
    CHECK(b.recording->operation_count() == 3 && calls == 3);
    // Definite backend refusal maps through the existing sink contract.
    b.recording->refuse_code = gb::ErrorCode::disconnected;
    b.recording->set_mode(fx::PortMode::refuse);
    expect_error(s.image->send(mel_report(time)), mel::ErrorCode::InsufficientRemoteResources, "did not submit");
    b.recording->set_mode(fx::PortMode::accept);
    CHECK(response(s.image->send(mel_report(time)))->getSystemTime() == time);
    CHECK(b.recording->operation_count() == 4 && calls == 4);

    // Reentry from the handler: provider operations are refused, not deadlocked.
    unsigned reentries = 0;
    b.controls->on_prepare = [&] {
        expect_error(s.image->send(mel_report(time + 100ns)), mel::ErrorCode::InvalidState);
        CHECK(s.image->disable() == ir::Return::Fail);
        CHECK(s.control->detachChannel(s.image) == ir::Return::Fail);
        ++reentries;
    };
    CHECK(response(s.image->send(mel_report(++time)))->getSystemTime() == time);
    b.controls->on_prepare = {};
    CHECK(reentries == 1 && b.recording->operation_count() == 5 && calls == 5);
}

void mel_profile_b_uncertainty() {
    for (const auto mode : {fx::PortMode::uncertain, fx::PortMode::throw_after_entry, fx::PortMode::lost_after_accept}) {
        ProfileBSession b;
        auto& s = b.mel;
        unsigned calls = 0;
        CHECK(s.image->registerMetadataCallback(NavigationCallback{[&](auto&, auto*) { ++calls; }}) == ir::Return::Success);
        CHECK(response(s.image->send(mel_report(1ns))));
        b.recording->set_mode(mode);
        // The recorded effect may have happened; MEL reports uncertain, not rejected.
        expect_error(s.image->send(mel_report(2ns)), mel::ErrorCode::InvalidState, "uncertain");
        CHECK(b.recording->operation_count() == 2 && calls == 1);
        b.recording->set_mode(fx::PortMode::accept);
        // Never retried, and no later success masks the unresolved effect.
        const auto entered = b.controls->entered.load();
        const auto later = s.image->send(mel_report(3ns)).get();
        CHECK(!later && b.controls->entered == entered && b.recording->operation_count() == 2 && calls == 1);
        CHECK(later.getError().getCode() == (mode == fx::PortMode::lost_after_accept
            ? mel::ErrorCode::InsufficientRemoteResources : mel::ErrorCode::InvalidState));
    }
}

void mel_profile_b_lifecycle() {
    {
        // Detach serializes with an admitted submission; reattach is a new session.
        ProfileBSession b;
        auto& s = b.mel;
        b.recording->port_gate.arm();
        auto pending = std::async(std::launch::async, [&] { return s.image->send(mel_report(10ns)); });
        TestCleanup release{[&] { b.recording->port_gate.release(); }};
        CHECK(b.recording->port_gate.wait_entered());
        std::promise<void> started;
        auto detached = std::async(std::launch::async, [&] { started.set_value(); return s.control->detachChannel(s.image); });
        started.get_future().wait();
        CHECK(detached.wait_for(20ms) == std::future_status::timeout); // waits for the operation lock
        b.recording->port_gate.release();
        CHECK(response(pending.get())->getSystemTime() == 10ns);
        CHECK(detached.get() == ir::Return::Success);
        auto old = s.image;
        expect_error(old->send(mel_report(11ns)), mel::ErrorCode::InvalidState, "detached");
        s.attach();
        CHECK(response(s.image->send(mel_report(1ns)))->getSystemTime() == 1ns); // new attachment threshold
        CHECK(b.recording->operation_count() == 2 && b.recording->port_gate.clean());
    }
    {
        // Frame receive overlaps navigation submission (synthetic stream + fixture port).
        ProfileBSession b;
        auto& s = b.mel;
        s.add_buffers(3);
        CHECK(s.image->enable() == ir::Return::Success);
        b.recording->port_gate.arm();
        auto pending = std::async(std::launch::async, [&] { return s.image->send(mel_report(1ns)); });
        TestCleanup release{[&] { b.recording->port_gate.release(); }};
        CHECK(b.recording->port_gate.wait_entered());
        const auto before = s.listener->frames.load();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (s.listener->frames.load() < before + 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        CHECK(s.listener->frames.load() >= before + 2);
        b.recording->port_gate.release();
        CHECK(response(pending.get()));
        CHECK(s.image->disable() == ir::Return::Success);
    }
    for (unsigned cycle = 0; cycle < 10; ++cycle) {
        // Dropping the last external manager/Control/profile references during an
        // admitted submission: it completes, then the device closes, the bridge
        // drains, and the port and profile state retire exactly once.
        auto controls = std::make_shared<nr::Controls>();
        auto recording = std::make_shared<fx::Recording>();
        auto session = std::make_unique<MelSession>(std::make_shared<fx::ProfileNavigationBackend>(nr::make(controls), recording));
        recording->port_gate.arm();
        auto image = session->image;
        auto pending = std::async(std::launch::async, [&] { return image->send(mel_report(1ns)); });
        TestCleanup release{[&] { recording->port_gate.release(); }};
        CHECK(recording->port_gate.wait_entered());
        std::weak_ptr<ir::Control> weak = session->control;
        session->image.reset();
        session->control.reset();
        session->manager.reset();
        CHECK(!weak.expired() && controls->retired == 0); // the admitted submission pins its runtime
        recording->port_gate.release();
        CHECK(response(pending.get())->getSystemTime() == 1ns);
        expect_error(image->send(mel_report(2ns)), mel::ErrorCode::InvalidState);
        image.reset();
        session.reset();
        CHECK(weak.expired());
        CHECK(controls->retired == 1 && recording->ports_destroyed == 1 && recording->operation_count() == 1);
        const auto events = recording->snapshot_events();
        CHECK(events == (std::vector<std::string>{"sink entered", "port recorded", "port retired", "device closed"}));
        CHECK(recording->port_gate.clean());
    }
}

void dynamic_module_navigation() {
    namespace s = gige_mel::selection;
    ModuleUnloadProbe probe(true);
    unsigned unloads = 0;
    // An incompatible build identity is refused before the factory (which aborts if called).
    auto incompatible = s::load_profile_module(GIGE_MEL_TEST_PROFILE_DIR "/libinvalid_navigation_profile_1.so");
    CHECK(!incompatible && incompatible.error().code == gb::ErrorCode::unsupported);
    CHECK(contains(incompatible.error().message, "build identity"));
    probe.expect(++unloads);
    {
        // A module without the extension stays unsupported at the profile level.
        auto loaded = take(s::load_profile_module(GIGE_MEL_TEST_PROFILE_MODULE));
        fx::StaticFeatures features;
        auto state = take(loaded.profile->open(features));
        CHECK(gp::navigation_handler(*state) == nullptr);
        auto bridge = gp::ProfileNavigationBridge::create(state, std::make_unique<fx::RecordingPort>(std::make_shared<fx::Recording>()));
        CHECK(!bridge && bridge.error().code == gb::ErrorCode::unsupported);
    }
    probe.expect(++unloads);
    for (unsigned cycle = 0; cycle < 10; ++cycle) {
        auto recording = std::make_shared<fx::Recording>();
        std::unique_ptr<gp::ProfileNavigationBridge> bridge;
        {
            auto loaded = take(s::load_profile_module(GIGE_MEL_TEST_NAVIGATION_MODULE));
            CHECK(loaded.profile->id() == nr::id);
            s::BackendCatalog catalog;
            CHECK(s::register_loaded_profile(catalog, loaded));
            auto selected = take(loaded.create());
            fx::StaticFeatures features;
            auto state = take(selected->open(features));
            // Host-visible wrapper preserves the module state's capability.
            CHECK(gp::navigation_handler(*state) != nullptr);
            bridge = take(gp::ProfileNavigationBridge::create(std::move(state), std::make_unique<fx::RecordingPort>(recording)));
        } // probe, factory, selected profile and catalog released; the bridge's state pins the module
        probe.expect(unloads);
        // The module's own handler runs through the host-visible wrapper.
        CHECK(bridge->submit(expected_report(1ns)));
        check_record(recording->operations.back(), expected_report(1ns));
        expect_status(bridge->submit(expected_report(2ns, gb::NavigationSolutionState::aligning)), gb::ErrorCode::unsupported, "Aligning");
        // Failed preparation does not leak or drop a module pin.
        probe.expect(unloads);
        // Race teardown with an admitted module-handler submission.
        recording->port_gate.arm();
        auto pending = std::async(std::launch::async, [&] { return bridge->submit(expected_report(3ns)); });
        TestCleanup release{[&] { recording->port_gate.release(); }};
        CHECK(recording->port_gate.wait_entered());
        auto closing = std::async(std::launch::async, [&] { return bridge->shutdown(); });
        CHECK(closing.wait_for(20ms) == std::future_status::timeout);
        probe.expect(unloads); // module code is still executing in the admitted call
        recording->port_gate.release();
        CHECK(pending.get() && closing.get());
        probe.expect(++unloads); // retired only after the drain, port and state
        // Prepared operations are plain values and remain valid after unload.
        CHECK(recording->operation_count() == 2);
        check_record(recording->operations.back(), expected_report(3ns));
        bridge.reset();
        probe.expect(unloads);
    }
    {
        // Full MEL path with the dynamically loaded profile; Control teardown unloads.
        auto recording = std::make_shared<fx::Recording>();
        std::unique_ptr<MelSession> session;
        {
            auto loaded = take(s::load_profile_module(GIGE_MEL_TEST_NAVIGATION_MODULE));
            session = std::make_unique<MelSession>(std::make_shared<fx::ProfileNavigationBackend>(take(loaded.create()), recording));
        }
        CHECK(response(session->image->send(mel_report(5ns)))->getSystemTime() == 5ns);
        expect_error(session->image->send(mel_report(6ns, mel::PositionSolutionState::Aligning)), mel::ErrorCode::Unsupported);
        check_record(recording->operations.back(), expected_report(5ns));
        probe.expect(unloads);
        session.reset();
        probe.expect(++unloads);
        CHECK(recording->operation_count() == 1 && recording->ports_destroyed == 1);
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string, void (*)()>> tests{
        {"bridge_capability_discovery", bridge_capability_discovery},
        {"bridge_preparation_and_mapping", bridge_preparation_and_mapping},
        {"bridge_uncertainty_and_control_loss", bridge_uncertainty_and_control_loss},
        {"bridge_concurrency_and_teardown", bridge_concurrency_and_teardown},
        {"mel_profile_a_receipt_only", mel_profile_a_receipt_only},
        {"mel_profile_b_dispatch", mel_profile_b_dispatch},
        {"mel_profile_b_uncertainty", mel_profile_b_uncertainty},
        {"mel_profile_b_lifecycle", mel_profile_b_lifecycle},
        {"dynamic_module_navigation", dynamic_module_navigation},
    };
    const std::string only = argc == 2 ? argv[1] : "";
    unsigned ran = 0;
    for (const auto& [name, run] : tests) {
        if (!only.empty() && only != name) continue;
        try { run(); }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
        std::cout << "PASS " << name << '\n';
        ++ran;
    }
    if (!ran) { std::cerr << "unknown scenario: " << only << '\n'; return 2; }
    std::cout << ran << " profile navigation scenarios passed (fixture acceptance only)\n";
    return 0;
}
