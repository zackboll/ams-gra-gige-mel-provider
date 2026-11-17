// GIGE-MEL-042: admitted unsupported C2 commands. Included at the end of
// provider_tests.cpp's anonymous namespace, after the navigation fixtures, to
// reuse the counting/transform/navigation decorators and Health/BIT/subsystem
// collectors. Contract: docs/tasks/GIGE-MEL-042-design.md.
//
// Every admitted CalibrationConfigurationCmd, CalibrationStatusCmd,
// CameraProtectCmd, EraseCommand and SystemTrackDataResponse returns its ready,
// typed Unsupported error and, with a registered usable callback, exactly one
// correlated Rejected/CapabilityUnavailable CommandStatus. Pre-admission
// rejection (callback context, closing Control, detached channel) is
// InvalidState with no status. Nothing else changes.

// Exact pinned overloads: selection by member-pointer type fails to compile if
// any upstream request or response type changes.
using CalibrationConfigurationSend =
    mel::RequestFor<mel::CalibrationConfiguration> (ir::C2Channel::*)(ir::CalibrationConfigurationCmd);
using CalibrationStatusSend = mel::RequestFor<mel::CalibrationStatus> (ir::C2Channel::*)(ir::CalibrationStatusCmd);
using CameraProtectSend = mel::RequestFor<ir::CameraProtectCmdResp> (ir::C2Channel::*)(ir::CameraProtectCmd);
using EraseSend = mel::RequestFor<ir::EraseCommandType> (ir::C2Channel::*)(ir::EraseCommand);
using SystemTrackSend = mel::RequestFor<ir::CommandStatus> (ir::C2Channel::*)(ir::SystemTrackDataResponse);
[[maybe_unused]] constexpr CalibrationConfigurationSend calibration_configuration_send = &ir::C2Channel::send;
[[maybe_unused]] constexpr CalibrationStatusSend calibration_status_send = &ir::C2Channel::send;
[[maybe_unused]] constexpr CameraProtectSend camera_protect_send = &ir::C2Channel::send;
[[maybe_unused]] constexpr EraseSend erase_send = &ir::C2Channel::send;
[[maybe_unused]] constexpr SystemTrackSend system_track_send = &ir::C2Channel::send;
static_assert(std::is_same_v<decltype(std::declval<ir::C2Channel&>().send(std::declval<ir::SystemTrackDataResponse>())),
                             std::future<mel::ErrorOr<std::shared_ptr<ir::CommandStatus>>>>);

enum class UnsupportedKind { calibration_configuration, calibration_status, camera_protect, erase, system_track };
constexpr std::array<UnsupportedKind, 5> unsupported_kinds{UnsupportedKind::calibration_configuration,
    UnsupportedKind::calibration_status, UnsupportedKind::camera_protect, UnsupportedKind::erase,
    UnsupportedKind::system_track};

// The exact documented description, shared by the future and the status.
std::string unsupported_description(UnsupportedKind kind) {
    switch (kind) {
    case UnsupportedKind::calibration_configuration:
        return "CalibrationConfigurationCmd is unsupported: calibration configuration is not available in the experimental image-stream profile";
    case UnsupportedKind::calibration_status:
        return "CalibrationStatusCmd is unsupported: calibration status is not available in the experimental image-stream profile";
    case UnsupportedKind::camera_protect:
        return "CameraProtectCmd is unsupported: camera protection is not available in the experimental image-stream profile";
    case UnsupportedKind::erase:
        return "EraseCommand is unsupported: erase and zeroize are not available in the experimental image-stream profile";
    case UnsupportedKind::system_track:
        return "SystemTrackDataResponse is unsupported: system track data input is not available in the experimental image-stream profile";
    }
    throw std::logic_error("unknown unsupported command kind");
}

// A settled future. `ready` is sampled with a zero timeout before get(): an
// unresolved promise would be observed as not ready instead of blocking.
struct Settled {
    bool valid{false}, ready{false}, has_value{false};
    mel::ErrorCode code{mel::ErrorCode::None};
    std::string description;
};

template <class T> Settled settle(mel::RequestFor<T> future) {
    Settled out;
    out.valid = future.valid();
    out.ready = out.valid && future.wait_for(0ms) == std::future_status::ready;
    if (!out.ready) return out;
    auto result = future.get();
    out.has_value = static_cast<bool>(result);
    if (!out.has_value) {
        out.code = result.getError().getCode();
        out.description = result.getError().getDescription();
    }
    return out;
}

// Each request carries nondefault non-ID fields so that only commandID can
// explain correlation; none of them must be interpreted.
Settled send_unsupported(ir::C2Channel& c2, UnsupportedKind kind, std::uint32_t id) {
    switch (kind) {
    case UnsupportedKind::calibration_configuration: {
        ir::CalibrationConfigurationCmd command; command.setCommandID(id); command.setRequestID(~id);
        return settle(c2.send(command));
    }
    case UnsupportedKind::calibration_status: {
        ir::CalibrationStatusCmd command; command.setCommandID(id); command.setRequestID(~id);
        return settle(c2.send(command));
    }
    case UnsupportedKind::camera_protect: {
        ir::CameraProtectCmd command; command.setCommandID(id); command.setProtectFlag(true);
        command.setResponseTime(5ms);
        return settle(c2.send(command));
    }
    case UnsupportedKind::erase: {
        ir::EraseCommand command; command.setCommandID(id);
        command.setEraseCommandType(ir::EraseCommandType::CDE_AND_Zeroize);
        return settle(c2.send(command));
    }
    case UnsupportedKind::system_track: {
        ir::SystemTrackDataResponse command; command.setCommandID(id); command.setRequestId(~id);
        command.setTrackId(9); command.setRange(1000.0); command.setRangeValid(true); command.setSystemTime(7ns);
        return settle(c2.send(command));
    }
    }
    throw std::logic_error("unknown unsupported command kind");
}

void check_unsupported_future(const Settled& settled, UnsupportedKind kind) {
    CHECK(settled.valid && settled.ready);
    CHECK(!settled.has_value); // Never a fabricated response object.
    CHECK(settled.code == mel::ErrorCode::Unsupported);
    CHECK(settled.description == unsupported_description(kind));
}

// Pre-admission rejection: InvalidState with a diagnostic and no status.
void check_closed_future(const Settled& settled, const std::string& expected) {
    CHECK(settled.valid && settled.ready && !settled.has_value);
    CHECK(settled.code == mel::ErrorCode::InvalidState);
    CHECK(settled.description == expected);
}
const std::string in_callback_rejection = "submit C2 commands outside provider callbacks";

void check_rejection(const ir::CommandStatus& status, std::uint32_t id, UnsupportedKind kind) {
    CHECK(status.getCommandID() == id);
    CHECK(status.getState() == ir::CommandState::Rejected);
    CHECK(status.getReasonID() == ir::CannotComply::CapabilityUnavailable);
    CHECK(status.getReasonDescription() == unsupported_description(kind));
}

// Thread-safe status collector. Records the delivering thread too.
struct StatusRecorder {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<ir::CommandStatus> statuses;
    std::vector<std::thread::id> threads;
    StatusCallback callback() {
        return [this](auto&, auto* status) {
            { std::lock_guard lock(mutex); statuses.push_back(*status); threads.push_back(std::this_thread::get_id()); }
            changed.notify_all();
        };
    }
    std::size_t count() { std::lock_guard lock(mutex); return statuses.size(); }
    ir::CommandStatus last() { std::lock_guard lock(mutex); CHECK(!statuses.empty()); return statuses.back(); }
    std::vector<ir::CommandStatus> all() { std::lock_guard lock(mutex); return statuses; }
};

void status_quiet(StatusRecorder& recorder) {
    std::unique_lock lock(recorder.mutex);
    const auto before = recorder.statuses.size();
    CHECK(!recorder.changed.wait_for(lock, 50ms, [&] { return recorder.statuses.size() != before; }));
}

const std::set<ir::ChannelMetadataCapabilityType>& expected_c2_metadata() {
    using Type = ir::ChannelMetadataCapabilityType;
    static const std::set<Type> value{Type::ChannelCommsTestRep, Type::CommandStatus, Type::BITConfiguration, Type::BITStatus};
    return value;
}

// No calibration, protection, erase or track capability is advertised anywhere,
// in discovery or on the attached channel, and the unsupported registrations
// remain NotSupported without retaining their callbacks.
void check_unadvertised(Fixture& f) {
    using Type = ir::ChannelMetadataCapabilityType;
    for (const auto& capability : f.control->getCapabilities()) {
        const auto& metadata = capability.getChannelMetadataCapabilities();
        CHECK(!metadata.contains(Type::CameraProtectCmdResp));
        if (capability.getChannelTypes() == std::vector{ir::ChannelType::CommandAndControl})
            CHECK(metadata == expected_c2_metadata());
    }
    CHECK(f.c2->getCapabilities().getChannelMetadataCapabilities() == expected_c2_metadata());
    auto owned = std::make_shared<int>(0);
    const std::weak_ptr<int> weak = owned;
    CHECK(f.c2->registerMetadataCallback(std::function<void(ir::Channel&, const mel::CalibrationConfiguration*)>{
        [owned](auto&, auto*) { (void)owned; }}) == ir::Return::NotSupported);
    CHECK(f.c2->registerMetadataCallback(std::function<void(ir::Channel&, const mel::CalibrationStatus*)>{
        [owned](auto&, auto*) { (void)owned; }}) == ir::Return::NotSupported);
    CHECK(f.c2->registerMetadataCallback(std::function<void(ir::Channel&, const ir::CameraProtectCmdResp*)>{
        [owned](auto&, auto*) { (void)owned; }}) == ir::Return::NotSupported);
    owned.reset();
    CHECK(weak.expired());
}

// All five types; distinct, zero, high-bit, UINT32_MAX and repeated IDs; exact
// state/reason/description; ready typed errors; unregistered and duplicate
// registration; synchronous delivery on the submitting thread.
void c2_unsupported_correlation_and_futures() {
    StatusRecorder recorder; // Outlives the Fixture and its callbacks.
    Fixture f;
    const auto initial = gige_mel::provider_statistics(f.manager);
    // Unregistered: the future alone; nothing is queued for a later registration.
    for (auto kind : unsupported_kinds) check_unsupported_future(send_unsupported(*f.c2, kind, 7), kind);
    CHECK(f.c2->registerMetadataCallback(recorder.callback()) == ir::Return::Success);
    status_quiet(recorder);
    CHECK(recorder.count() == 0);

    const std::uint32_t ids[] = {0U, 1U, 0x80000000U, std::numeric_limits<std::uint32_t>::max(), 42U, 42U};
    std::size_t delivered = 0;
    for (auto id : ids) {
        for (auto kind : unsupported_kinds) {
            check_unsupported_future(send_unsupported(*f.c2, kind, id), kind);
            CHECK(recorder.count() == ++delivered); // Delivered before send returned.
            check_rejection(recorder.last(), id, kind);
        }
    }
    // Distinct IDs across one interleaved sequence: no state shared between types.
    std::uint32_t next = 1000;
    for (int round = 0; round < 3; ++round) {
        for (auto kind : unsupported_kinds) {
            check_unsupported_future(send_unsupported(*f.c2, kind, next), kind);
            check_rejection(recorder.last(), next++, kind);
            ++delivered;
        }
    }
    CHECK(recorder.count() == delivered);
    // Repeated IDs are independent rejections: no deduplication or history.
    std::size_t repeated = 0;
    for (const auto& status : recorder.all()) {
        CHECK(status.getState() == ir::CommandState::Rejected); // Never Accepted/Received.
        if (status.getCommandID() == 42U) ++repeated;
    }
    CHECK(repeated == 2 * unsupported_kinds.size());
    for (auto thread : recorder.threads) CHECK(thread == std::this_thread::get_id());

    // Registration contract is unchanged and unsupported requests never displace
    // the registered callback.
    CHECK(f.c2->registerMetadataCallback(StatusCallback{}) == ir::Return::BadPointer);
    CHECK(f.c2->registerMetadataCallback(StatusCallback{[](auto&, auto*) {
        throw std::runtime_error("duplicate status callback must never run");
    }}) == ir::Return::Fail);
    for (auto kind : unsupported_kinds) {
        check_unsupported_future(send_unsupported(*f.c2, kind, 5), kind);
        check_rejection(recorder.last(), 5, kind);
    }
    CHECK(recorder.count() == delivered + unsupported_kinds.size());
    check_unadvertised(f);

    // The callback receives this C2 channel.
    StatusRecorder other_recorder;
    Fixture other;
    const ir::Channel* seen = nullptr;
    CHECK(other.c2->registerMetadataCallback(StatusCallback{[&](auto& channel, auto* status) {
        seen = &channel;
        other_recorder.callback()(channel, status);
    }}) == ir::Return::Success);
    check_unsupported_future(send_unsupported(*other.c2, UnsupportedKind::system_track, 3), UnsupportedKind::system_track);
    CHECK(seen == static_cast<const ir::Channel*>(other.c2.get()) && other_recorder.count() == 1);
    CHECK(recorder.count() == delivered + unsupported_kinds.size()); // No cross-manager delivery.

    // Ordinary rejection is not a provider diagnostic or callback failure.
    const auto final_statistics = gige_mel::provider_statistics(f.manager);
    CHECK(final_statistics.callback_failures == initial.callback_failures);
    CHECK(final_statistics.last_error == initial.last_error);
}

// Captures the complete observable Health/BIT/subsystem state once, idle, from
// one attached Health channel that is then detached again.
struct ObservedState {
    std::map<std::string, std::string> details;
    mel::MFA_State state{mel::MFA_State::NotSet};
    std::uint32_t bit_revision{0};
    mel::BIT_Status bit;
};
ObservedState observe(Fixture& f) {
    HealthReports reports;
    SubsystemReports subsystems;
    BitReports bits;
    auto health = attach_health(f);
    reports.register_both(health);
    CHECK(health->registerMetadataCallback(bits.callback()) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(subsystems.callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    subsystems.wait(1); // Batch order S,D,B,U: the first batch is complete.
    CHECK(health->disable() == ir::Return::Success);
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    ObservedState out;
    std::lock_guard lock(reports.mutex);
    out.details = HealthReports::values(reports.details.front());
    out.state = reports.statuses.front().getMFAState();
    out.bit_revision = revision(subsystems.at(0));
    out.bit = bits.at(0);
    return out;
}

void send_every_unsupported(ir::C2Channel& c2, StatusRecorder& recorder, std::uint32_t first_id) {
    for (auto kind : unsupported_kinds) {
        const auto before = recorder.count();
        check_unsupported_future(send_unsupported(c2, kind, first_id), kind);
        CHECK(recorder.count() == before + 1);
        check_rejection(recorder.last(), first_id++, kind);
    }
}

// No backend/device/stream I/O; no configuration, generation, acquisition,
// buffer, BIT, subsystem revision, Health, diagnostic or navigation change.
void c2_unsupported_no_side_effects() {
    StatusRecorder recorder;
    auto counted = std::make_shared<CountingBackend>();
    Fixture f(true, counted);
    f.add_buffer();
    // Genuine history: a diagnostic and a completed BIT that must both survive.
    CHECK(!response(f.c2->send(camera_command(8, 0)))->getCommandSuccess());
    CHECK(*response(f.c2->send(ir::BIT_Command{9, {1}, {}, {}})) == ir::Return::Success);
    CHECK(f.c2->registerMetadataCallback(recorder.callback()) == ir::Return::Success);
    const auto before = observe(f);
    const auto calls = counted->calls->load();
    const auto statistics = gige_mel::provider_statistics(f.manager);
    const auto width = f.image->getCapabilities().getWidth();
    for (std::uint32_t round = 0; round < 4; ++round) send_every_unsupported(*f.c2, recorder, 10 * round);
    CHECK(counted->calls->load() == calls); // No enumerate/open/apply/readback/stream call at all.
    const auto after_statistics = gige_mel::provider_statistics(f.manager);
    CHECK(after_statistics.delivered_frames == statistics.delivered_frames &&
          after_statistics.dropped_no_buffer == statistics.dropped_no_buffer &&
          after_statistics.rejected_frames == statistics.rejected_frames &&
          after_statistics.callback_failures == statistics.callback_failures &&
          after_statistics.stream_failures == statistics.stream_failures &&
          after_statistics.last_error == statistics.last_error);
    CHECK(f.listener->count() == 0 && f.image->getCapabilities().getWidth() == width);
    const auto after = observe(f);
    CHECK(after.details == before.details); // Includes generation, validity, acquisition, history.
    CHECK(after.state == mel::MFA_State::Standby && before.state == mel::MFA_State::Standby);
    CHECK(after.bit_revision == before.bit_revision);
    CHECK(after.bit.getCompletedBITs().size() == 1 && before.bit.getCompletedBITs().size() == 1);
    CHECK(after.bit.getCompletedBITs().front().getTimeTag() == before.bit.getCompletedBITs().front().getTimeTag());
    CHECK(after.bit.getFaults().empty());
    CHECK(counted->calls->load() == calls);
    // The registered, idle Image still enables normally afterwards.
    CHECK(f.image->enable() == ir::Return::Success);
    f.listener->wait_for(1);
    CHECK(f.image->disable() == ir::Return::Success);

    // Running acquisition: no configuration apply/readback, no stream create,
    // start, cancellation or stop; delivery continues uninterrupted.
    StatusRecorder active_recorder;
    auto plan = std::make_shared<ReceivePlan>();
    plan->inject_terminal = false;
    auto probe = std::make_shared<ConfigurationProbe>();
    Fixture active(true, std::make_shared<TransformBackend>(Transform::none, plan, probe));
    active.listener->auto_release = true;
    active.add_buffer(); active.add_buffer();
    CHECK(active.c2->registerMetadataCallback(active_recorder.callback()) == ir::Return::Success);
    CHECK(active.image->enable() == ir::Return::Success);
    active.listener->wait_for(2);
    const auto applies = probe->applies, reads = probe->reads;
    const auto creations = plan->creations.load(), starts = plan->starts.load();
    const auto cancellations = plan->cancellations.load(), stops = plan->stops.load();
    send_every_unsupported(*active.c2, active_recorder, 500);
    CHECK(probe->applies == applies && probe->reads == reads && probe->requests.size() == applies);
    CHECK(plan->creations == creations && plan->starts == starts);
    CHECK(plan->cancellations == cancellations && plan->stops == stops);
    const auto frames = active.listener->count();
    active.listener->wait_for(frames + 2);
    CHECK(active.image->enable() == ir::Return::Success); // Still enabled, not faulted.
    CHECK(active.image->disable() == ir::Return::Success);
    CHECK(gige_mel::provider_statistics(active.manager).stream_failures == 0);

    // Navigation: no backend submission, no receipt callback, no retained-time change.
    StatusRecorder navigation_recorder;
    auto navigation = std::make_shared<NavigationProbe>();
    Fixture n(true, std::make_shared<NavigationBackend>(navigation));
    unsigned receipts = 0;
    CHECK(n.image->registerMetadataCallback(NavigationCallback{[&](auto&, auto*) { ++receipts; }}) == ir::Return::Success);
    CHECK(n.c2->registerMetadataCallback(navigation_recorder.callback()) == ir::Return::Success);
    CHECK(response(n.image->send(navigation_test_data(5ns)))->getSystemTime() == 5ns);
    send_every_unsupported(*n.c2, navigation_recorder, 600);
    CHECK(navigation->reports.size() == 1 && receipts == 1);
    expect_error(n.image->send(navigation_test_data(4ns)), mel::ErrorCode::InvalidState); // Stale vs 5 ns.
    CHECK(response(n.image->send(navigation_test_data(6ns)))->getSystemTime() == 6ns);
    CHECK(navigation->reports.size() == 2 && receipts == 2);
}

// Counts pre-admission outcomes from contexts where CHECK must not throw into
// provider code (an Image callback exception would fault the acquisition).
struct ContextProbe {
    std::atomic<unsigned> rejected{0}, wrong{0};
    void send_all(ir::C2Channel& c2, std::uint32_t id) {
        for (auto kind : unsupported_kinds) {
            const auto settled = send_unsupported(c2, kind, id);
            const bool ok = settled.valid && settled.ready && !settled.has_value &&
                settled.code == mel::ErrorCode::InvalidState && settled.description == in_callback_rejection;
            ++(ok ? rejected : wrong);
        }
    }
};

// Same-/cross-provider reentry from status, Image, comms and navigation-sink
// contexts rejects before locking with no recursive status; consumer
// exceptions are contained and counted; ordinary use resumes afterwards.
void c2_unsupported_callback_reentry_and_exceptions() {
    StatusRecorder other_recorder;
    Fixture f, other;
    CHECK(other.c2->registerMetadataCallback(other_recorder.callback()) == ir::Return::Success);
    ContextProbe probe;
    unsigned calls = 0, completed = 0;
    std::vector<ir::CommandStatus> seen;
    CHECK(f.c2->registerMetadataCallback(StatusCallback{[&](auto&, auto* status) {
        ++calls;
        seen.push_back(*status);
        probe.send_all(*f.c2, 900);     // Same provider: no recursive rejection status.
        probe.send_all(*other.c2, 901); // Another provider instance.
        expect_error(f.c2->send(config_command()), mel::ErrorCode::InvalidState);
        expect_error(f.c2->send(standby_command()), mel::ErrorCode::InvalidState);
        CHECK(f.c2->registerMetadataCallback(StatusCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
        CHECK(*response(f.c2->sendKeepAliveRep()) == ir::Return::Success); // Callback-safe read.
        ++completed;
        throw std::runtime_error("deliberate unsupported-status consumer exception");
    }}) == ir::Return::Success);
    const auto failures = gige_mel::provider_statistics(f.manager).callback_failures;
    std::uint32_t id = 20;
    for (auto kind : unsupported_kinds) {
        check_unsupported_future(send_unsupported(*f.c2, kind, id), kind); // Throw cannot alter it.
        check_rejection(seen.back(), id++, kind);
    }
    CHECK(calls == 5 && completed == 5 && seen.size() == 5);
    CHECK(probe.rejected == 50 && probe.wrong == 0);
    CHECK(other_recorder.count() == 0);
    const auto statistics = gige_mel::provider_statistics(f.manager);
    CHECK(statistics.callback_failures == failures + 5);
    CHECK(statistics.last_error == "metadata callback threw; successful operation remains committed");
    CHECK(gige_mel::provider_statistics(other.manager).callback_failures == 0);
    // Usable after the exceptions: supported and unsupported commands alike.
    CHECK(*response(f.c2->send(config_command(31))) == ir::Return::Success);
    check_status(seen.back(), 31);
    check_unsupported_future(send_unsupported(*other.c2, UnsupportedKind::erase, 32), UnsupportedKind::erase);
    check_rejection(other_recorder.last(), 32, UnsupportedKind::erase);

    // Image frame callback (worker thread) and Image comms callback contexts.
    ContextProbe image_probe;
    StatusRecorder image_recorder;
    Fixture g;
    CHECK(g.c2->registerMetadataCallback(image_recorder.callback()) == ir::Return::Success);
    g.add_buffer(); g.listener->auto_release = true;
    g.listener->hook = [&] { image_probe.send_all(*g.c2, 40); image_probe.send_all(*other.c2, 41); };
    CHECK(g.image->enable() == ir::Return::Success);
    g.listener->wait_for(2);
    CHECK(g.image->disable() == ir::Return::Success);
    CHECK(image_probe.wrong == 0 && image_probe.rejected >= 20);
    CHECK(image_recorder.count() == 0 && gige_mel::provider_statistics(g.manager).stream_failures == 0);
    CHECK(g.image->registerMetadataCallback(CommsCallback{[&](auto&, auto*) {
        image_probe.send_all(*g.c2, 42);
    }}) == ir::Return::Success);
    const auto before_comms = image_probe.rejected.load();
    (void)response(g.image->send(comms_request(1, 2)));
    CHECK(image_probe.rejected == before_comms + 5 && image_probe.wrong == 0 && image_recorder.count() == 0);

    // Backend navigation submission holds operations: rejection must not wait on it.
    ContextProbe sink_probe;
    StatusRecorder sink_recorder;
    auto navigation = std::make_shared<NavigationProbe>();
    Fixture n(true, std::make_shared<NavigationBackend>(navigation));
    CHECK(n.c2->registerMetadataCallback(sink_recorder.callback()) == ir::Return::Success);
    navigation->reentry = [&] { sink_probe.send_all(*n.c2, 50); };
    CHECK(response(n.image->send(navigation_test_data(1ns)))->getSystemTime() == 1ns);
    CHECK(sink_probe.rejected == 5 && sink_probe.wrong == 0 && sink_recorder.count() == 0);
    navigation->reentry = nullptr;
    send_every_unsupported(*n.c2, sink_recorder, 60);

    // Strong pins: the callback drops the last external Control/C2 references.
    Fixture pinned;
    unsigned pinned_completed = 0;
    const std::weak_ptr<ir::Control> weak_control = pinned.control;
    const std::weak_ptr<ir::C2Channel> weak_channel = pinned.c2;
    CHECK(pinned.c2->registerMetadataCallback(StatusCallback{[&](auto& channel, auto* status) {
        check_rejection(*status, 70, UnsupportedKind::camera_protect);
        pinned.control.reset();
        pinned.c2.reset();
        CHECK(!weak_control.expired() && !weak_channel.expired());
        CHECK(*response(channel.sendKeepAliveRep()) == ir::Return::Success);
        ++pinned_completed;
    }}) == ir::Return::Success);
    auto* raw = pinned.c2.get();
    check_unsupported_future(send_unsupported(*raw, UnsupportedKind::camera_protect, 70), UnsupportedKind::camera_protect);
    CHECK(pinned_completed == 1 && weak_control.expired() && weak_channel.expired());
    CHECK(gige_mel::provider_statistics(pinned.manager).callback_failures == 0);
}

// Admission during concurrent Image delivery and the Health worker, and from
// concurrent submitters. Each submitter observes its own statuses on its own
// thread in its own order; no global cross-thread order is asserted.
void c2_unsupported_concurrency() {
    StatusRecorder recorder;
    BatchRecorder batches;
    Fixture f;
    f.listener->auto_release = true;
    f.add_buffer(); f.add_buffer();
    auto health = attach_health(f);
    TestCleanup cleanup{[&] { (void)health->disable(); (void)f.image->disable(); }};
    batches.register_all(*health);
    CHECK(f.c2->registerMetadataCallback(recorder.callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    CHECK(f.image->enable() == ir::Return::Success);
    f.listener->wait_for(1);
    const auto frames = f.listener->count();
    const auto batches_before = batches.batches();

    constexpr unsigned threads = 4, per_thread = 40;
    std::vector<std::future<unsigned>> submitters;
    for (unsigned t = 0; t < threads; ++t) {
        submitters.push_back(std::async(std::launch::async, [&, t] {
            unsigned failures = 0;
            for (unsigned i = 0; i < per_thread; ++i) {
                const auto kind = unsupported_kinds[i % unsupported_kinds.size()];
                const auto settled = send_unsupported(*f.c2, kind, t * 1000 + i);
                if (!settled.ready || settled.has_value || settled.code != mel::ErrorCode::Unsupported ||
                    settled.description != unsupported_description(kind)) ++failures;
            }
            return failures;
        }));
    }
    for (auto& submitter : submitters) CHECK(submitter.get() == 0);
    f.listener->wait_for(frames + 2);             // Delivery continued.
    batches.wait_state(mel::MFA_State::Operate, "running", true, batches_before); // Health undisturbed.
    CHECK(f.image->disable() == ir::Return::Success);
    CHECK(health->disable() == ir::Return::Success);
    batches.check_order(std::this_thread::get_id());

    const auto statuses = recorder.all();
    std::vector<std::thread::id> delivering;
    { std::lock_guard lock(recorder.mutex); delivering = recorder.threads; }
    CHECK(statuses.size() == threads * per_thread);
    std::map<std::uint32_t, std::thread::id> owner;
    std::map<std::thread::id, std::uint32_t> last_index;
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        const auto id = statuses[i].getCommandID();
        const auto t = id / 1000, index = id % 1000;
        CHECK(t < threads && index < per_thread);
        check_rejection(statuses[i], id, unsupported_kinds[index % unsupported_kinds.size()]);
        CHECK(owner.emplace(id, delivering[i]).second); // Each distinct ID exactly once.
        CHECK(delivering[i] != std::this_thread::get_id());
        // Per-submitter order only: statuses from one thread are in its send order.
        const auto found = last_index.find(delivering[i]);
        if (found != last_index.end()) CHECK(index > found->second);
        last_index[delivering[i]] = index;
    }
    for (const auto& [id, thread] : owner) CHECK(owner.at(id - id % 1000) == thread); // One thread per submitter.
    CHECK(gige_mel::provider_statistics(f.manager).stream_failures == 0);
    CHECK(gige_mel::provider_statistics(f.manager).callback_failures == 0);
}

// C2 enable/disable do not gate admission; detach drains an admitted callback
// and closes admission; Control destruction; no late callback afterwards.
void c2_unsupported_lifecycle() {
    StatusRecorder recorder;
    Fixture f;
    CHECK(f.c2->registerMetadataCallback(recorder.callback()) == ir::Return::Success);
    send_every_unsupported(*f.c2, recorder, 1); // Never enabled.
    CHECK(f.c2->enable() == ir::Return::Success);
    send_every_unsupported(*f.c2, recorder, 11);
    CHECK(f.c2->disable() == ir::Return::Success);
    send_every_unsupported(*f.c2, recorder, 21); // Disabled: still admitted, as for ConfigSet.
    CHECK(recorder.count() == 15);

    // Detach drains an admitted, gated status; a competing submission blocks
    // behind the drain and then reports the detached channel without status.
    WatchdogGate gate;
    std::atomic<unsigned> entered{0}, drained{0};
    Fixture g;
    CHECK(g.c2->registerMetadataCallback(StatusCallback{[&](auto&, auto* status) {
        ++entered;
        check_rejection(*status, 77, UnsupportedKind::calibration_status);
        gate.enter_and_wait();
        ++drained;
    }}) == ir::Return::Success);
    auto sent = std::async(std::launch::async, [&] {
        return send_unsupported(*g.c2, UnsupportedKind::calibration_status, 77);
    });
    TestCleanup open_gate{[&] { gate.open(); }};
    gate.wait_for_entry();
    auto detached = std::async(std::launch::async, [&] { return g.control->detachChannel(g.c2); });
    eventually([&] { return !g.c2->sendKeepAliveRep().get(); }); // Admission closed.
    auto competing = std::async(std::launch::async, [&] {
        return send_unsupported(*g.c2, UnsupportedKind::erase, 78);
    });
    const bool waited = detached.wait_for(30ms) == std::future_status::timeout;
    gate.open();
    CHECK(sent.wait_for(2s) == std::future_status::ready);
    check_unsupported_future(sent.get(), UnsupportedKind::calibration_status); // Admitted before detach.
    CHECK(detached.wait_for(2s) == std::future_status::ready && detached.get() == ir::Return::Success);
    CHECK(competing.wait_for(2s) == std::future_status::ready);
    check_closed_future(competing.get(), "channel is detached");
    CHECK(waited && entered == 1 && drained == 1);
    gate.check();
    for (auto kind : unsupported_kinds) check_closed_future(send_unsupported(*g.c2, kind, 79), "channel is detached");
    CHECK(g.c2->registerMetadataCallback(StatusCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
    CHECK(entered == 1);

    // Reattach: a fresh channel has no inherited callback; old handle stays closed.
    auto old = g.c2;
    g.c2 = std::dynamic_pointer_cast<ir::C2Channel>(g.control->attachChannel(g.config(ir::ChannelType::CommandAndControl)));
    CHECK(g.c2 && g.c2 != old);
    for (auto kind : unsupported_kinds) check_unsupported_future(send_unsupported(*g.c2, kind, 80), kind);
    check_closed_future(send_unsupported(*old, UnsupportedKind::system_track, 81), "channel is detached");
    CHECK(entered == 1);

    // Control destruction while channel handles survive: closing Control is
    // InvalidState before any admission and publishes nothing, ever again.
    const auto delivered = recorder.count();
    const std::weak_ptr<ir::Control> weak_control = f.control;
    f.control.reset();
    CHECK(weak_control.expired());
    for (auto kind : unsupported_kinds) check_closed_future(send_unsupported(*f.c2, kind, 90), "control is closing");
    CHECK(f.c2->registerMetadataCallback(StatusCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
    status_quiet(recorder);
    CHECK(recorder.count() == delivered);
    f.c2.reset(); f.image.reset(); // Channel destruction after Control: no late callback.
    status_quiet(recorder);
    CHECK(recorder.count() == delivered);
}

// Supported C2 operations keep their exact behavior when interleaved with
// unsupported requests, through the same registered status callback.
void c2_unsupported_supported_regression() {
    StatusRecorder recorder;
    Fixture f;
    std::vector<std::uint32_t> image_replies;
    std::vector<char> order; // 'I' Image reply, 'S' C2 status.
    CHECK(f.image->registerMetadataCallback(ImageReplyCallback{[&](auto&, auto* reply) {
        image_replies.push_back(reply->getCommandID()); order.push_back('I');
    }}) == ir::Return::Success);
    const auto status_callback = recorder.callback();
    CHECK(f.c2->registerMetadataCallback(StatusCallback{[&](auto& channel, auto* status) {
        order.push_back('S'); status_callback(channel, status);
    }}) == ir::Return::Success);
    BitReports bits;
    CHECK(f.c2->registerMetadataCallback(bits.callback()) == ir::Return::Success); // Initial publication.
    CHECK(bits.count() == 1);
    check_unadvertised(f);

    for (int round = 0; round < 2; ++round) {
        const std::uint32_t base = 100U * static_cast<std::uint32_t>(round + 1);
        check_unsupported_future(send_unsupported(*f.c2, UnsupportedKind::calibration_configuration, base),
                                 UnsupportedKind::calibration_configuration);
        check_rejection(recorder.last(), base, UnsupportedKind::calibration_configuration);
        order.clear();
        CHECK(response(f.c2->send(camera_command(base + 1)))->getCommandSuccess());
        CHECK((order == std::vector<char>{'I', 'S'}) && image_replies.back() == base + 1); // Reply, then status.
        check_status(recorder.last(), base + 1);
        check_unsupported_future(send_unsupported(*f.c2, UnsupportedKind::camera_protect, base + 2),
                                 UnsupportedKind::camera_protect);
        CHECK(*response(f.c2->send(config_command(base + 3))) == ir::Return::Success);
        check_status(recorder.last(), base + 3);
        expect_error(f.c2->send(config_command(base + 4, "not JSON")), mel::ErrorCode::InvalidParameters);
        check_status(recorder.last(), base + 4, ir::CannotComply::InvalidInputParameter);
        check_unsupported_future(send_unsupported(*f.c2, UnsupportedKind::erase, base + 5), UnsupportedKind::erase);
        const auto bit_count = bits.count();
        CHECK(*response(f.c2->send(ir::BIT_Command{base + 6, {1}, {}, {}})) == ir::Return::Success);
        check_status(recorder.last(), base + 6);
        CHECK(bits.count() == bit_count + 1 && BitReports::has_expected_completion(bits.last()));
        check_unsupported_future(send_unsupported(*f.c2, UnsupportedKind::system_track, base + 7),
                                 UnsupportedKind::system_track);
        CHECK(bits.count() == bit_count + 1); // Unsupported requests publish no BIT_Status.
        CHECK(*response(f.c2->send(standby_command(base + 8))) == ir::MFA_Mode::Unused);
        check_status(recorder.last(), base + 8);
        auto operate = standby_command(base + 9);
        operate.setState(mel::MFA_State::Operate); operate.setMode(ir::MFA_Mode::TaskSched);
        expect_error(f.c2->send(operate), mel::ErrorCode::Unsupported);
        check_status(recorder.last(), base + 9, ir::CannotComply::CapabilityUnavailable);
        check_unsupported_future(send_unsupported(*f.c2, UnsupportedKind::calibration_status, base + 10),
                                 UnsupportedKind::calibration_status);
        check_rejection(recorder.last(), base + 10, UnsupportedKind::calibration_status);
        // Image-origin CameraCommand: Image reply only, never C2 status.
        const auto statuses = recorder.count();
        CHECK(response(f.image->send(camera_command(base + 11)))->getCommandSuccess());
        CHECK(image_replies.back() == base + 11 && recorder.count() == statuses);
    }
    // 2 rounds x 11 C2 statuses; only CameraCommand produced Image replies.
    CHECK(recorder.count() == 22 && image_replies.size() == 4);
    CHECK(f.c2->getCapabilities().getChannelMetadataCapabilities() == expected_c2_metadata());
    CHECK(gige_mel::provider_statistics(f.manager).callback_failures == 0);
    // The unchanged legacy check: still Unsupported with a nonempty description.
    unsupported_surfaces_return_real_errors();
}
