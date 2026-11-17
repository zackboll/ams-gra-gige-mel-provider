// GIGE-MEL-041: executable classification of the two @Required Health
// registrations that the pinned capability enumeration cannot advertise.
// Included after provider_subsystem_tests.inc in provider_tests.cpp's anonymous
// namespace; reuses its Health/BIT/Subsystem collectors and fixtures.
//
// Classification BLOCKED_BY_UPSTREAM_CAPABILITY_ENUM: both registrations must
// return Return::NotSupported, retain nothing, invoke nothing, change no
// provider state and remain absent from every advertised capability set, while
// the established Health batch (MFA_Status, MFA_StatusDetailed, BIT_Status,
// SubsystemStatusResp) is unchanged. See docs/tasks/GIGE-MEL-041-design.md.

// Exact pinned upstream parameter types. Top-level `const` on the pointer
// parameter is not part of the function type, so these are the declared types.
using DiscreteCallback = std::function<void(ir::Channel&, const mel::DiscreteStatus* const)>;
using AuditCallback = std::function<void(ir::Channel&, const mel::MFA_SecurityAuditRecord* const)>;
using CapabilityType = ir::ChannelMetadataCapabilityType;

// Overload selection against the pinned abstract interface: compilation fails
// if either exact upstream signature disappears or changes.
using DiscreteRegistration = ir::Return (ir::HealthStatusChannel::*)(DiscreteCallback);
using AuditRegistration = ir::Return (ir::HealthStatusChannel::*)(AuditCallback);
static_assert(std::is_same_v<DiscreteCallback, std::function<void(ir::Channel&, const mel::DiscreteStatus*)>>);
static_assert(std::is_same_v<AuditCallback, std::function<void(ir::Channel&, const mel::MFA_SecurityAuditRecord*)>>);
static_assert(std::is_abstract_v<ir::HealthStatusChannel>);

// Compile-time evidence of the upstream contradiction. If a future pinned
// revision adds a corresponding enumerator, these assertions fail and force a
// reevaluation instead of silently leaving a newly advertisable surface blocked.
template <class E> concept names_discrete_status = requires { E::DiscreteStatus; };
template <class E> concept names_security_audit = requires { E::MFASecurityAuditRecord; } ||
    requires { E::MFA_SecurityAuditRecord; } || requires { E::SecurityAuditRecord; };
template <class E> concept names_subsystem_status = requires { E::SubsystemStatusResp; }; // Positive control.
static_assert(!names_discrete_status<CapabilityType>, "pinned enum gained DiscreteStatus: reevaluate GIGE-MEL-041");
static_assert(!names_security_audit<CapabilityType>, "pinned enum gained a security-audit value: reevaluate GIGE-MEL-041");
static_assert(names_subsystem_status<CapabilityType>);
// The four reserved values are pinned, final and never repurposed.
static_assert(static_cast<std::uint32_t>(CapabilityType::ScanPerformanceReport) == 27);
static_assert(static_cast<std::uint32_t>(CapabilityType::Reserved3) == 28);
static_assert(static_cast<std::uint32_t>(CapabilityType::Reserved5) == 29);
static_assert(static_cast<std::uint32_t>(CapabilityType::Reserved9) == 30);
static_assert(static_cast<std::uint32_t>(CapabilityType::Reserved10) == 31);

const std::set<CapabilityType>& expected_health_metadata() {
    static const std::set<CapabilityType> expected{CapabilityType::ChannelCommsTestRep, CapabilityType::MFAStatus,
        CapabilityType::MFAStatusDetailed, CapabilityType::BITStatus, CapabilityType::SubsystemStatusResp};
    return expected;
}

void check_no_reserved(const std::set<CapabilityType>& set) {
    for (auto value : {CapabilityType::Reserved3, CapabilityType::Reserved5, CapabilityType::Reserved9,
                       CapabilityType::Reserved10}) CHECK(!set.contains(value));
    for (auto value : set)
        CHECK(static_cast<std::uint32_t>(value) <= static_cast<std::uint32_t>(CapabilityType::ScanPerformanceReport));
}

// Discovery and attached-channel capability accuracy for every channel type.
void check_advertisement(Fixture& f, const ir::HealthStatusChannel& health) {
    CHECK(health.getCapabilities().getChannelMetadataCapabilities() == expected_health_metadata());
    unsigned health_entries = 0;
    for (const auto& cap : f.control->getCapabilities()) {
        const auto& set = cap.getChannelMetadataCapabilities();
        check_no_reserved(set);
        if (cap.getChannelTypes() == std::vector{ir::ChannelType::HealthAndStatus}) {
            CHECK(set == expected_health_metadata());
            ++health_entries;
        } else {
            // Health-only payloads are never advertised on Image or C2.
            for (auto value : {CapabilityType::MFAStatus, CapabilityType::MFAStatusDetailed,
                               CapabilityType::SubsystemStatusResp}) CHECK(!set.contains(value));
        }
    }
    CHECK(health_entries == 1);
    check_no_reserved(f.image->getCapabilities().getChannelMetadataCapabilities());
    check_no_reserved(f.c2->getCapabilities().getChannelMetadataCapabilities());
}

// Counts erroneous invocations; ownership of this state also proves retention.
struct RejectedProbe {
    std::atomic<unsigned> invoked{0};
};

// One unsupported registration of each blocked payload. Each callback captures
// the only strong owner of fresh state, so expiry immediately after the call
// proves the provider neither stored, copied nor deferred the callback. The
// callbacks throw, so an erroneous invocation would also change
// callback_failures.
void check_blocked(ir::HealthStatusChannel& health, const std::shared_ptr<RejectedProbe>& totals) {
    auto discrete = std::make_shared<int>(0), audit = std::make_shared<int>(0);
    const std::weak_ptr<int> weak_discrete = discrete, weak_audit = audit;
    CHECK(health.registerMetadataCallback(DiscreteCallback{[owned = std::move(discrete), totals](auto&, auto*) {
        (void)owned; ++totals->invoked;
        throw std::runtime_error("blocked DiscreteStatus callback must never run");
    }}) == ir::Return::NotSupported);
    CHECK(health.registerMetadataCallback(AuditCallback{[owned = std::move(audit), totals](auto&, auto*) {
        (void)owned; ++totals->invoked;
        throw std::runtime_error("blocked security-audit callback must never run");
    }}) == ir::Return::NotSupported);
    CHECK(weak_discrete.expired() && weak_audit.expired());
    // Empty and repeated registrations are also NotSupported: an unimplemented
    // surface never answers BadPointer/Fail as if a callback slot existed.
    CHECK(health.registerMetadataCallback(DiscreteCallback{}) == ir::Return::NotSupported);
    CHECK(health.registerMetadataCallback(AuditCallback{}) == ir::Return::NotSupported);
}

// Records the complete established Health batch from the worker.
struct BatchRecorder {
    std::mutex mutex;
    std::condition_variable changed;
    std::string order;
    std::vector<std::thread::id> threads;
    std::vector<mel::MFA_Status> statuses;
    std::vector<std::map<std::string, std::string>> details;
    std::vector<ir::SubsystemStatusResp> subsystems;

    void add(char kind) {
        { std::lock_guard lock(mutex); order.push_back(kind); threads.push_back(std::this_thread::get_id()); }
        changed.notify_all();
    }
    void register_all(ir::HealthStatusChannel& health) {
        CHECK(health.registerMetadataCallback(HealthStatusCallback{[this](auto&, auto* value) {
            { std::lock_guard lock(mutex); statuses.push_back(*value); } add('S'); }}) == ir::Return::Success);
        CHECK(health.registerMetadataCallback(HealthDetailedCallback{[this](auto&, auto* value) {
            auto parsed = HealthReports::values(*value);
            { std::lock_guard lock(mutex); details.push_back(std::move(parsed)); } add('D'); }}) == ir::Return::Success);
        CHECK(health.registerMetadataCallback(BitCallback{[this](auto&, auto*) { add('B'); }}) == ir::Return::Success);
        CHECK(health.registerMetadataCallback(SubsystemCallback{[this](auto&, auto* value) {
            { std::lock_guard lock(mutex); subsystems.push_back(*value); } add('U'); }}) == ir::Return::Success);
    }
    std::size_t batches() { std::lock_guard lock(mutex); return order.size() / 4; }
    std::size_t size() { std::lock_guard lock(mutex); return order.size(); }

    // Waits for a COMPLETE batch at or after index `after` whose three
    // state-bearing reports agree with one expected observed state.
    void wait_state(mel::MFA_State state, const char* acquisition, bool configuration_valid, std::size_t after = 0) {
        const auto failure = state == mel::MFA_State::Degraded ? ir::Failure::Major : ir::Failure::Available;
        const auto mode = state == mel::MFA_State::Degraded ? ir::CSCIMode::Unknown :
            state == mel::MFA_State::Operate ? ir::CSCIMode::Imaging : ir::CSCIMode::Idle;
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 2s, [&] {
            for (auto batch = after; (batch + 1) * 4 <= order.size(); ++batch) {
                if (statuses.at(batch).getMFAState() != state) continue;
                const auto& detail = details.at(batch);
                const auto& subsystem = subsystems.at(batch);
                // Same captured state across all three reports in this batch.
                CHECK(detail.at("gige_mel.acquisition_state") == acquisition);
                CHECK(detail.at("gige_mel.configuration_valid") == (configuration_valid ? "true" : "false"));
                CHECK(in_state(subsystem, mode, failure));
                return true;
            }
            return false;
        }));
    }
    // Every batch is S,D,B,U on one provider worker thread; only the final
    // batch may be a prefix (disable can suppress callbacks not yet admitted).
    void check_order(std::thread::id caller) {
        std::lock_guard lock(mutex);
        static constexpr char pattern[] = "SDBU";
        for (std::size_t i = 0; i < order.size(); ++i) CHECK(order[i] == pattern[i % 4]);
        for (auto id : threads) CHECK(id == threads.front() && id != caller);
    }
    void quiet() {
        std::unique_lock lock(mutex);
        const auto before = order.size();
        CHECK(!changed.wait_for(lock, 150ms, [&] { return order.size() != before; }));
    }
};

// Exact overloads, NotSupported results, unchanged advertisement and the
// unchanged established registration contract around the blocked surfaces.
void required_metadata_contract() {
    Fixture f;
    auto health = attach_health(f);
    // Exact upstream overloads are reachable through the pinned interface type
    // and dispatch virtually to the provider.
    const DiscreteRegistration discrete = &ir::HealthStatusChannel::registerMetadataCallback;
    const AuditRegistration audit = &ir::HealthStatusChannel::registerMetadataCallback;
    auto totals = std::make_shared<RejectedProbe>();
    CHECK(((*health).*discrete)(DiscreteCallback{[totals](auto&, auto*) { ++totals->invoked; }}) == ir::Return::NotSupported);
    CHECK(((*health).*audit)(AuditCallback{[totals](auto&, auto*) { ++totals->invoked; }}) == ir::Return::NotSupported);
    check_advertisement(f, *health);
    // A blocked registration consumes, prompts and replaces no supported slot:
    // each supported surface still registers exactly once afterwards.
    check_blocked(*health, totals);
    BatchRecorder recorder;
    recorder.register_all(*health);
    CHECK(health->registerMetadataCallback(HealthStatusCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
    CHECK(health->registerMetadataCallback(SubsystemCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
    CHECK(health->registerMetadataCallback(HealthStatusCallback{}) == ir::Return::BadPointer);
    check_blocked(*health, totals); // Still blocked after supported registration...
    recorder.quiet();               // ...and nothing publishes while disabled.
    CHECK(health->enable() == ir::Return::Success);
    recorder.wait_state(mel::MFA_State::Standby, "idle", true);
    check_blocked(*health, totals); // ...and while enabled.
    recorder.wait_state(mel::MFA_State::Standby, "idle", true, recorder.batches() + 2);
    CHECK(health->disable() == ir::Return::Success);
    recorder.quiet();
    recorder.check_order(std::this_thread::get_id());
    check_advertisement(f, *health);
    CHECK(totals->invoked == 0);
    const auto statistics = gige_mel::provider_statistics(f.manager);
    CHECK(statistics.callback_failures == 0 && statistics.last_error.empty());
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
}

// Initial, configuration failure/recovery, running, faulted and recovered
// states: the blocked classification and advertisement never vary with state,
// and the established reports remain mutually consistent in every state.
void required_metadata_states() {
    auto plan = std::make_shared<ReceivePlan>();
    auto probe = std::make_shared<ConfigurationProbe>();
    Fixture f(true, std::make_shared<TransformBackend>(Transform::none, plan, probe));
    auto health = attach_health(f);
    BatchRecorder recorder;
    auto totals = std::make_shared<RejectedProbe>();
    TestCleanup cleanup{[&] { plan->open(); (void)health->disable(); (void)f.image->disable(); }};
    recorder.register_all(*health);
    check_blocked(*health, totals);
    CHECK(health->enable() == ir::Return::Success);
    // Consecutive steps differ in MFA_State, so a batch captured before the
    // step's action can never satisfy that step.
    const auto step = [&](mel::MFA_State state, const char* acquisition, bool valid) {
        recorder.wait_state(state, acquisition, valid, recorder.batches());
        check_blocked(*health, totals);
        check_advertisement(f, *health);
    };
    step(mel::MFA_State::Standby, "idle", true);                            // Initial.
    probe->fail_readback = true;
    {
        auto result = f.c2->send(config_command()).get();
        CHECK(!result || *result.get() == ir::Return::Fail);                 // Configuration failure.
    }
    step(mel::MFA_State::Degraded, "idle", false);
    CHECK(f.image->enable() == ir::Return::Fail);
    probe->fail_readback = false;
    CHECK(*response(f.c2->send(config_command())) == ir::Return::Success);  // Configuration recovery.
    step(mel::MFA_State::Standby, "idle", true);
    f.add_buffer();
    CHECK(f.image->enable() == ir::Return::Success);
    plan->wait_for(&ReceivePlan::entered);
    step(mel::MFA_State::Operate, "running", true);                         // Running.
    plan->open(); plan->wait_for_exit();
    step(mel::MFA_State::Degraded, "faulted", true);                        // Faulted.
    CHECK(f.image->disable() == ir::Return::Success);
    step(mel::MFA_State::Standby, "idle", true);                            // Recovered.
    CHECK(health->disable() == ir::Return::Success);
    recorder.check_order(std::this_thread::get_id());
    CHECK(totals->invoked == 0);
    CHECK(gige_mel::provider_statistics(f.manager).callback_failures == 0);
}

// Concurrent Image delivery, Health publication, capability queries and blocked
// registrations from several threads. The TSan lanes check for data races.
void required_metadata_concurrency() {
    Fixture f;
    f.add_buffer(); f.add_buffer(); f.listener->auto_release = true;
    auto health = attach_health(f);
    BatchRecorder recorder;
    recorder.register_all(*health);
    auto totals = std::make_shared<RejectedProbe>();
    CHECK(f.image->enable() == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    std::atomic<bool> stop{false};
    std::atomic<unsigned> attempts{0};
    std::vector<std::future<void>> workers;
    TestCleanup cleanup{[&] {
        stop = true;
        for (auto& worker : workers) if (worker.valid()) worker.wait();
        (void)health->disable(); (void)f.image->disable();
    }};
    for (int i = 0; i < 3; ++i) {
        workers.push_back(std::async(std::launch::async, [&] {
            while (!stop) {
                check_blocked(*health, totals);
                CHECK(health->getCapabilities().getChannelMetadataCapabilities() == expected_health_metadata());
                check_no_reserved(f.image->getCapabilities().getChannelMetadataCapabilities());
                ++attempts;
                std::this_thread::yield();
            }
        }));
    }
    const auto frames = f.listener->count();
    recorder.wait_state(mel::MFA_State::Operate, "running", true, recorder.batches() + 3);
    f.listener->wait_for(frames + 5);
    stop = true;
    for (auto& worker : workers) worker.get(); // Rethrows any worker CHECK failure.
    CHECK(health->disable() == ir::Return::Success);
    CHECK(f.image->disable() == ir::Return::Success);
    recorder.check_order(std::this_thread::get_id());
    CHECK(attempts > 0 && totals->invoked == 0);
    const auto statistics = gige_mel::provider_statistics(f.manager);
    CHECK(statistics.callback_failures == 0 && statistics.stream_failures == 0);
}

// Inside admitted Health callbacks, against this and another provider: blocked
// registrations still answer NotSupported (no deadlock, no admission), the
// supported registrations retain their callback-context Fail, and a throwing
// consumer callback remains contained exactly as before.
void required_metadata_reentry() {
    Fixture a, b;
    auto health = attach_health(a), other = attach_health(b);
    auto totals = std::make_shared<RejectedProbe>();
    std::mutex mutex;
    std::condition_variable changed;
    unsigned completed = 0;
    std::exception_ptr failure;
    TestCleanup cleanup{[&] { (void)health->disable(); }};
    CHECK(health->registerMetadataCallback(HealthStatusCallback{[&](auto&, auto*) {
        try {
            for (auto* h : {health.get(), other.get()}) {
                check_blocked(*h, totals);
                CHECK(h->registerMetadataCallback(SubsystemCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
                CHECK(h->enable() == ir::Return::Fail && h->disable() == ir::Return::Fail);
                CHECK(h->getCapabilities().getChannelMetadataCapabilities() == expected_health_metadata());
            }
            std::lock_guard lock(mutex);
            ++completed;
        } catch (...) {
            std::lock_guard lock(mutex);
            if (!failure) failure = std::current_exception();
        }
        changed.notify_all();
        throw std::runtime_error("deliberate Health consumer exception during blocked registration");
    }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 2s, [&] { return completed >= 3 || failure; }));
        if (failure) std::rethrow_exception(failure);
    }
    CHECK(health->disable() == ir::Return::Success);
    unsigned observed;
    { std::lock_guard lock(mutex); observed = completed; }
    const auto statistics = gige_mel::provider_statistics(a.manager);
    CHECK(statistics.callback_failures == observed); // Only the consumer's own throws.
    CHECK(statistics.last_error == "Health metadata callback threw; periodic reporting continues");
    CHECK(gige_mel::provider_statistics(b.manager).callback_failures == 0);
    CHECK(totals->invoked == 0);
}

// Disabled, enabled, disabled, detached and post-Control-destruction channels;
// no late callbacks; bounded resources (repeated rejection retains nothing).
void required_metadata_lifetime() {
    auto totals = std::make_shared<RejectedProbe>();
    BatchRecorder recorder; // Outlives the Fixture and every callback.
    Fixture f;
    auto health = attach_health(f);
    check_blocked(*health, totals); // Attached, never enabled.
    recorder.register_all(*health);
    CHECK(health->enable() == ir::Return::Success);
    recorder.wait_state(mel::MFA_State::Standby, "idle", true);
    CHECK(health->disable() == ir::Return::Success);
    const auto delivered = recorder.size();
    check_blocked(*health, totals); // Disabled after publication.
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    check_blocked(*health, totals); // Detached: still the unsupported result.
    CHECK(health->registerMetadataCallback(HealthStatusCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
    CHECK(health->enable() == ir::Return::Fail);
    recorder.quiet();
    CHECK(recorder.size() == delivered); // No callback after disable/detach.
    recorder.check_order(std::this_thread::get_id());

    // Bounded: many rejected registrations neither retain nor publish.
    auto reattached = attach_health(f);
    for (int i = 0; i < 4096; ++i) check_blocked(*reattached, totals);
    CHECK(totals.use_count() == 1);
    // Control destruction while both channel objects survive externally.
    const std::weak_ptr<ir::Control> weak_control = f.control;
    CHECK(f.control->detachChannel(reattached) == ir::Return::Success);
    CHECK(f.control->detachChannel(f.image) == ir::Return::Success);
    CHECK(f.control->detachChannel(f.c2) == ir::Return::Success);
    f.image.reset(); f.c2.reset(); f.control.reset();
    CHECK(weak_control.expired());
    check_blocked(*reattached, totals);
    check_blocked(*health, totals);
    CHECK(totals->invoked == 0 && totals.use_count() == 1); // No retained capture anywhere.
    CHECK(recorder.size() == delivered);
    CHECK(gige_mel::provider_statistics(f.manager).callback_failures == 0);
    health.reset(); reattached.reset(); // Channel destruction after Control.
    CHECK(recorder.size() == delivered);
}
