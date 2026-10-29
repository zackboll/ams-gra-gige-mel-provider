// Included after provider_bit_tests.inc in provider_tests.cpp's anonymous
// namespace. Reuses its Health/BIT collectors, gates and counting decorators.
using SubsystemCallback = std::function<void(ir::Channel&, const ir::SubsystemStatusResp*)>;

struct SubsystemReports {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<ir::SubsystemStatusResp> reports;
    std::vector<std::thread::id> threads;
    std::vector<std::chrono::steady_clock::time_point> times;
    SubsystemCallback callback() {
        return [this](auto&, auto* value) { record(*value); };
    }
    void record(const ir::SubsystemStatusResp& value) {
        { std::lock_guard lock(mutex); reports.push_back(value); threads.push_back(std::this_thread::get_id());
          times.push_back(std::chrono::steady_clock::now()); }
        changed.notify_all();
    }
    void wait(std::size_t count) {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 2s, [&] { return reports.size() >= count; }));
    }
    std::size_t count() { std::lock_guard lock(mutex); return reports.size(); }
    ir::SubsystemStatusResp at(std::size_t index) { std::lock_guard lock(mutex); CHECK(index < reports.size()); return reports[index]; }
    ir::SubsystemStatusResp last() { std::lock_guard lock(mutex); CHECK(!reports.empty()); return reports.back(); }
    std::vector<ir::SubsystemStatusResp> since(std::size_t first) {
        std::lock_guard lock(mutex); CHECK(first <= reports.size());
        return {reports.begin() + static_cast<std::ptrdiff_t>(first), reports.end()};
    }
    void quiet() {
        std::unique_lock lock(mutex); const auto before = reports.size();
        CHECK(!changed.wait_for(lock, 150ms, [&] { return reports.size() != before; }));
    }
    // Payload freshness: the first report at or beyond `after` satisfying the
    // predicate, selected while synchronized. Watchdog expiry fails the test.
    template <class Predicate> ir::SubsystemStatusResp wait_for(Predicate predicate, std::size_t after = 0) {
        std::unique_lock lock(mutex);
        std::optional<ir::SubsystemStatusResp> found;
        CHECK(changed.wait_for(lock, 2s, [&] {
            for (auto i = after; i < reports.size(); ++i)
                if (predicate(reports[i])) { found = reports[i]; return true; }
            return false;
        }));
        return *found;
    }
};

const ir::SubsystemCSCIInfo& only_csci(const std::vector<ir::SubsystemCSCIInfo>& csci) {
    CHECK(csci.size() == 1);
    return csci.front();
}

// Exact static software-subsystem inventory and version-field order.
void check_inventory(const ir::SubsystemStatusResp& report) {
    CHECK(report.getSubsystemId() == 1 && report.getCriticality() == 0);
    CHECK(report.getSubsystems().empty() && report.getSubsystemCount() == 0);
    const auto csci_list = report.getCSCI();
    CHECK(report.getCsciCount() == 1);
    const auto& csci = only_csci(csci_list);
    CHECK(csci.getCsci() == "ams-gra-gige-mel-provider");
    CHECK(csci.getCriticality() == 0);
    const auto version = csci.getVersion();
    CHECK(version.getSource() == 0 && version.getMajorRevision() == 0 && version.getMinorRevision() == 1 &&
          version.getEngineeringRevision() == 0);
    CHECK(csci.getFailureLevel() == report.getFailureLevel()); // Aggregate equals its single CSCI.
}

void check_state(const ir::SubsystemStatusResp& report, ir::CSCIMode mode, ir::Failure failure) {
    check_inventory(report);
    const auto csci_list = report.getCSCI();
    const auto& csci = only_csci(csci_list);
    CHECK(csci.getMode() == mode && report.getFailureLevel() == failure);
    CHECK(csci.getConnectionEstablished()); // Every publicly attachable state is initialized.
}

std::uint32_t revision(const ir::SubsystemStatusResp& report) {
    const auto csci = report.getCSCI();
    return only_csci(csci).getBIT_report();
}
bool in_state(const ir::SubsystemStatusResp& report, ir::CSCIMode mode, ir::Failure failure) {
    const auto csci = report.getCSCI();
    return report.getFailureLevel() == failure && only_csci(csci).getMode() == mode;
}
auto state_is(ir::CSCIMode mode, ir::Failure failure) {
    return [=](const ir::SubsystemStatusResp& report) { return in_state(report, mode, failure); };
}

// Payload-predicate freshness for BIT_Status, beyond an optional baseline.
template <class Predicate> mel::BIT_Status wait_bit(BitReports& bits, Predicate predicate, std::size_t after = 0) {
    std::unique_lock lock(bits.mutex);
    std::optional<mel::BIT_Status> found;
    CHECK(bits.changed.wait_for(lock, 2s, [&] {
        for (auto i = after; i < bits.reports.size(); ++i)
            if (predicate(bits.reports[i])) { found = bits.reports[i]; return true; }
        return false;
    }));
    return *found;
}

// Consecutive deliveries within one undisturbed enabled interval carry
// consecutive sequence numbers: nothing else consumes or skips the sequence.
void check_consecutive(const std::vector<ir::SubsystemStatusResp>& reports) {
    for (std::size_t i = 1; i < reports.size(); ++i)
        CHECK(reports[i].getStatusSeqNum() == static_cast<std::uint32_t>(reports[i - 1].getStatusSeqNum() + 1U));
}

void subsystem_surface() {
    SubsystemReports reports; // Outlives the Fixture and every callback.
    Fixture f;
    const auto expected = std::set<ir::ChannelMetadataCapabilityType>{
        ir::ChannelMetadataCapabilityType::ChannelCommsTestRep, ir::ChannelMetadataCapabilityType::MFAStatus,
        ir::ChannelMetadataCapabilityType::MFAStatusDetailed, ir::ChannelMetadataCapabilityType::BITStatus,
        ir::ChannelMetadataCapabilityType::SubsystemStatusResp};
    bool discovered = false;
    for (const auto& cap : f.control->getCapabilities()) {
        const bool is_health = cap.getChannelTypes() == std::vector{ir::ChannelType::HealthAndStatus};
        CHECK(cap.getChannelMetadataCapabilities().contains(ir::ChannelMetadataCapabilityType::SubsystemStatusResp) == is_health);
        if (is_health) { CHECK(cap.getChannelMetadataCapabilities() == expected); discovered = true; }
    }
    CHECK(discovered);
    // Distinct, unchanged version fields: MEL API 4, numeric library 1, text 0.1.0.
    const auto version = f.control->getVersionInfo();
    CHECK(version.getAPIVersion() == 4 && version.getLibVersion() == 1);
    CHECK(version.getVendor() == "ams-gra-gige-mel-provider" && version.getDescription().rfind("0.1.0 ", 0) == 0);
    auto health = attach_health(f);
    CHECK(health->getCapabilities().getChannelMetadataCapabilities() == expected);
    CHECK(health->getCapabilities().getWidth() == 0 && health->getCapabilities().getBufferSize() == 0);
    CHECK(health->registerMetadataCallback(SubsystemCallback{}) == ir::Return::BadPointer);
    const auto registering = std::this_thread::get_id();
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Fail);
    CHECK(health->registerMetadataCallback(SubsystemCallback{}) == ir::Return::BadPointer);
    reports.quiet(); // Disabled: registration publishes nothing inline or later.
#define CHECK_SUBSYSTEM_UNSUPPORTED(T) \
    CHECK(health->registerMetadataCallback(std::function<void(ir::Channel&, const T*)>{[](auto&, auto*) {}}) == ir::Return::NotSupported)
    CHECK_SUBSYSTEM_UNSUPPORTED(ir::LFStatus);
    CHECK_SUBSYSTEM_UNSUPPORTED(mel::DiscreteStatus);
    CHECK_SUBSYSTEM_UNSUPPORTED(mel::MFA_SecurityAuditRecord);
    CHECK_SUBSYSTEM_UNSUPPORTED(ir::NUC_TempData);
#undef CHECK_SUBSYSTEM_UNSUPPORTED
    auto buffer = f.add_buffer(3072, false);
    CHECK(health->registerBuffer(buffer) == ir::Return::NotSupported);
    CHECK(health->enable() == ir::Return::Success); // Subsystem-only subscriber.
    reports.wait(3);
    CHECK(health->disable() == ir::Return::Success); reports.quiet();
    const auto all = reports.since(0);
    CHECK(all.front().getStatusSeqNum() == 1); // First prepared report of this manager.
    check_consecutive(all);
    std::vector<std::thread::id> threads;
    std::vector<std::chrono::steady_clock::time_point> times;
    { std::lock_guard lock(reports.mutex); threads = reports.threads; times = reports.times; }
    for (std::size_t i = 0; i < all.size(); ++i) {
        check_state(all[i], ir::CSCIMode::Idle, ir::Failure::Available);
        CHECK(revision(all[i]) == 0); // Empty canonical BIT state; unchanged samples.
        CHECK(threads[i] != registering);
    }
    // Nominal 100 ms best effort: a lower bound between unprompted samples only.
    CHECK(times[2] - times[1] >= 90ms);
    CHECK(health->unregisterBuffer(buffer) == ir::Return::NotSupported);
    CHECK(*response(health->sendKeepAliveRep()) == ir::Return::Success);
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Fail);
    CHECK(health->registerMetadataCallback(SubsystemCallback{}) == ir::Return::Fail);
}

// Manager-lifetime counters across re-enable, reattachment and Control
// recreation; independent managers keep independent counters.
void subsystem_lifecycle() {
    SubsystemReports reports, other_reports;
    Fixture f, other;
    auto health = attach_health(f);
    CHECK(health->enable() == ir::Return::Success);
    // Late registration while enabled prompts the existing worker, not the caller.
    const auto registering = std::this_thread::get_id();
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    reports.wait(1);
    CHECK(health->disable() == ir::Return::Success); reports.quiet();
    CHECK(reports.at(0).getStatusSeqNum() == 1 && reports.threads.front() != registering);
    // An accepted execution advances the BIT revision once.
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    auto before = reports.count();
    auto last_sequence = reports.last().getStatusSeqNum();
    CHECK(health->enable() == ir::Return::Success); // Re-enable: no reset.
    reports.wait(before + 2);
    CHECK(health->disable() == ir::Return::Success); reports.quiet();
    auto interval = reports.since(before);
    CHECK(interval.front().getStatusSeqNum() == last_sequence + 1); // Disabled: nothing consumed.
    check_consecutive(interval);
    for (const auto& report : interval) CHECK(revision(report) == 1);
    // Detach and reattach: fresh registration state, same manager counters.
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    health = attach_health(f);
    before = reports.count(); last_sequence = reports.last().getStatusSeqNum();
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success); reports.wait(before + 1);
    CHECK(health->disable() == ir::Return::Success); reports.quiet();
    CHECK(reports.at(before).getStatusSeqNum() == last_sequence + 1 && revision(reports.at(before)) == 1);
    // Control recreation within the same surviving runtime.
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    CHECK(f.control->detachChannel(f.c2) == ir::Return::Success);
    CHECK(f.control->detachChannel(f.image) == ir::Return::Success);
    health.reset(); f.c2.reset(); f.image.reset(); f.control.reset();
    f.control = ir::getControl("synthetic", f.manager);
    CHECK(f.control && f.control->init() == ir::Return::Success); f.attach();
    health = attach_health(f);
    before = reports.count(); last_sequence = reports.last().getStatusSeqNum();
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success); reports.wait(before + 1);
    CHECK(health->disable() == ir::Return::Success); reports.quiet();
    CHECK(reports.at(before).getStatusSeqNum() == last_sequence + 1 && revision(reports.at(before)) == 1);
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    // An independent manager starts from its own zero state. Before any
    // Subsystem registration, Health BIT-only samples and C2 executions prepare
    // BIT state (advancing ITS revision) but consume no subsystem sequence.
    BitReports other_bits;
    auto other_health = attach_health(other);
    CHECK(other_health->registerMetadataCallback(other_bits.callback()) == ir::Return::Success);
    CHECK(other_health->enable() == ir::Return::Success); other_bits.wait(3);
    CHECK(other_health->disable() == ir::Return::Success);
    CHECK(*response(other.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    CHECK(*response(other.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    CHECK(other_health->registerMetadataCallback(other_reports.callback()) == ir::Return::Success);
    CHECK(other_health->enable() == ir::Return::Success); other_reports.wait(1);
    CHECK(other_health->disable() == ir::Return::Success);
    CHECK(other_reports.at(0).getStatusSeqNum() == 1 && revision(other_reports.at(0)) == 2);
    CHECK(other.control->detachChannel(other_health) == ir::Return::Success);
    // f's counters were not affected by the other manager.
    health = attach_health(f);
    before = reports.count(); last_sequence = reports.last().getStatusSeqNum();
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success); reports.wait(before + 1);
    CHECK(health->disable() == ir::Return::Success);
    CHECK(reports.at(before).getStatusSeqNum() == last_sequence + 1 && revision(reports.at(before)) == 1);
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
}


// Current readiness from one captured core state; historical counters,
// diagnostics and a retained failed BIT completion do not degrade it.
void subsystem_state_mapping() {
    auto plan = std::make_shared<ReceivePlan>(); // First receive gated, then terminal stop.
    auto probe = std::make_shared<ConfigurationProbe>();
    SubsystemReports reports;
    BitReports bits;
    Fixture f(true, std::make_shared<TransformBackend>(Transform::none, plan, probe));
    auto health = attach_health(f);
    TestCleanup cleanup{[&] { plan->open(); (void)health->disable(); }};
    CHECK(health->registerMetadataCallback(bits.callback()) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    auto idle = reports.wait_for(state_is(ir::CSCIMode::Idle, ir::Failure::Available));
    check_state(idle, ir::CSCIMode::Idle, ir::Failure::Available);
    CHECK(revision(idle) == 0);
    f.add_buffer();
    CHECK(f.image->enable() == ir::Return::Success);
    plan->wait_for(&ReceivePlan::entered); // Owned acquisition, no terminal failure yet.
    auto imaging = reports.wait_for(state_is(ir::CSCIMode::Imaging, ir::Failure::Available), reports.count());
    check_state(imaging, ir::CSCIMode::Imaging, ir::Failure::Available);
    CHECK(revision(imaging) == 0);
    plan->open(); plan->wait_for_exit(); // Injected unexpected stop: terminal failure.
    auto failed = reports.wait_for(state_is(ir::CSCIMode::Unknown, ir::Failure::Major), reports.count());
    check_state(failed, ir::CSCIMode::Unknown, ir::Failure::Major); // Software association remains.
    CHECK(revision(failed) == 1); // Observed fault creation, committed by Health.
    CHECK(f.image->enable() == ir::Return::Fail);
    // Accepted execution with a Fail result: completion replacement only.
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    failed = reports.wait_for([](const auto& r) { return revision(r) == 2; }, reports.count());
    check_state(failed, ir::CSCIMode::Unknown, ir::Failure::Major);
    CHECK(f.image->disable() == ir::Return::Success); // Explicit recovery.
    auto recovered = reports.wait_for(state_is(ir::CSCIMode::Idle, ir::Failure::Available), reports.count());
    check_state(recovered, ir::CSCIMode::Idle, ir::Failure::Available);
    CHECK(revision(recovered) == 3); // Observed retirement.
    // History retained; current readiness and the historical BIT result differ.
    const auto statistics = gige_mel::provider_statistics(f.manager);
    CHECK(statistics.stream_failures == 1 && statistics.last_error == plan->terminal.message);
    const auto retained = wait_bit(bits, [](const mel::BIT_Status& r) {
        return r.getFaults().empty() && r.getCompletedBITs().size() == 1 &&
               r.getCompletedBITs().front().getResult() == mel::BIT_Result::Fail;
    });
    check_completion(retained, true, false);
    // Invalid configuration: Major until valid readback, independent of acquisition.
    probe->fail_readback = true;
    auto rejected = f.c2->send(config_command()).get();
    CHECK(!rejected || *rejected.get() == ir::Return::Fail);
    auto invalid = reports.wait_for(state_is(ir::CSCIMode::Unknown, ir::Failure::Major), reports.count());
    check_state(invalid, ir::CSCIMode::Unknown, ir::Failure::Major);
    CHECK(revision(invalid) == 4);
    CHECK(f.image->enable() == ir::Return::Fail);
    probe->fail_readback = false;
    CHECK(*response(f.c2->send(config_command())) == ir::Return::Success);
    recovered = reports.wait_for(state_is(ir::CSCIMode::Idle, ir::Failure::Available), reports.count());
    CHECK(revision(recovered) == 5);
    // Recovery to a fresh acquisition after release: Imaging again.
    f.release_buffers(); f.listener->auto_release = true;
    CHECK(f.image->enable() == ir::Return::Success); f.listener->wait_for(1);
    imaging = reports.wait_for(state_is(ir::CSCIMode::Imaging, ir::Failure::Available), reports.count());
    CHECK(revision(imaging) == 5);
    CHECK(health->disable() == ir::Return::Success);
    CHECK(f.image->disable() == ir::Return::Success);
}


// Observation is controlled: Health is enabled only to take samples, so no
// background commit can interleave with a C2 execution between assertions.
ir::SubsystemStatusResp sample(const std::shared_ptr<ir::HealthStatusChannel>& health, SubsystemReports& reports,
                               std::size_t samples = 1) {
    const auto before = reports.count();
    CHECK(health->enable() == ir::Return::Success);
    reports.wait(before + samples);
    CHECK(health->disable() == ir::Return::Success); // Quiesced.
    const auto taken = reports.since(before);
    check_consecutive(taken);
    for (const auto& report : taken) CHECK(revision(report) == revision(taken.front())); // Unchanged samples.
    return reports.at(before);
}

void subsystem_counters() {
    auto probe = std::make_shared<ConfigurationProbe>();
    SubsystemReports reports;
    BitReports c2_bits;
    std::vector<ir::CommandStatus> statuses;
    Fixture f(true, std::make_shared<TransformBackend>(Transform::none, nullptr, probe));
    auto health = attach_health(f);
    TestCleanup cleanup{[&] { (void)health->disable(); }};
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success); // Subsystem only.
    // Initial empty state; repeated unchanged reports advance only statusSeqNum.
    auto report = sample(health, reports, 3);
    CHECK(revision(report) == 0 && report.getStatusSeqNum() == 1);
    // C2 registration and initial delivery with unchanged state: no revision.
    CHECK(f.c2->registerMetadataCallback(c2_bits.callback()) == ir::Return::Success);
    CHECK(f.c2->registerMetadataCallback(StatusCallback{[&](auto&, auto* s) { statuses.push_back(*s); }}) == ir::Return::Success);
    CHECK(revision(sample(health, reports)) == 0);
    // Accepted Pass, then a repeated command ID/result: each is a new completion.
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    CHECK(revision(sample(health, reports)) == 1);
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    CHECK(revision(sample(health, reports)) == 2);
    // A repeated host timestamp is still a new completion.
    std::chrono::nanoseconds first_time{}, second_time{};
    {
        test_clock::Fixed clock;
        CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
        first_time = c2_bits.last().getCompletedBITs().front().getTimeTag();
        CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
        second_time = c2_bits.last().getCompletedBITs().front().getTimeTag();
    }
    CHECK(first_time == second_time);
    CHECK(revision(sample(health, reports)) == 4);
    // Rejected commands (including unsupported clear) leave it unchanged.
    const auto rejected = std::vector<ir::BIT_Command>{{60,{},{},{}}, {61,{2},{},{}}, {62,{},{1},{}},
                                                       {63,{},{},{"gige_mel.configuration_invalid"}}, {64,{1,1},{},{}}};
    for (const auto& command : rejected) (void)f.c2->send(command).get();
    CHECK(statuses.size() == 4 + rejected.size());
    CHECK(revision(sample(health, reports)) == 4);
    // Configuration generation change with unchanged (valid) BIT state.
    const auto generation_before = probe->applies;
    CHECK(*response(f.c2->send(config_command())) == ir::Return::Success);
    CHECK(probe->applies == generation_before + 1);
    CHECK(revision(sample(health, reports)) == 4);
    // Observed fault creation (Subsystem-only observation), then accepted Fail.
    probe->fail_readback = true; (void)f.c2->send(config_command()).get();
    report = sample(health, reports, 2);
    CHECK(revision(report) == 5 && in_state(report, ir::CSCIMode::Unknown, ir::Failure::Major));
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    check_completion(c2_bits.last(), false, true);
    CHECK(revision(sample(health, reports)) == 6);
    // Observed retirement, then recurrence.
    probe->fail_readback = false; CHECK(*response(f.c2->send(config_command())) == ir::Return::Success);
    CHECK(revision(sample(health, reports)) == 7);
    probe->fail_readback = true; (void)f.c2->send(config_command()).get();
    CHECK(revision(sample(health, reports)) == 8);
    // Completion replacement plus fault retirement in ONE atomic commit: once.
    probe->fail_readback = false; CHECK(*response(f.c2->send(config_command())) == ir::Return::Success);
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    CHECK(c2_bits.last().getFaults().empty());
    CHECK(revision(sample(health, reports)) == 9);
    // Completion plus fault creation in one commit: also once.
    probe->fail_readback = true; (void)f.c2->send(config_command()).get();
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    CHECK(c2_bits.last().getFaults().size() == 1);
    CHECK(revision(sample(health, reports)) == 10);
    // Health disable/detach/reattach preserves manager-lifetime counter state.
    const auto sequence = reports.last().getStatusSeqNum();
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    health = attach_health(f);
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    report = sample(health, reports);
    CHECK(revision(report) == 10 && report.getStatusSeqNum() == sequence + 1);
}


// A Health batch is held after capture (inside its BIT callback). A C2 BIT
// executes and completes meanwhile. The held batch's later Subsystem report must
// carry the OLDER captured revision paired with its BIT payload, not a reread.
void subsystem_coherent_batch() {
    auto bit_gate = std::make_shared<WatchdogGate>();
    std::mutex mutex;
    std::vector<char> order;
    std::vector<mel::BIT_Status> bits;
    SubsystemReports reports; // Collectors outlive Fixture and every callback.
    std::atomic<bool> hold{false};
    Fixture f;
    auto health = attach_health(f);
    TestCleanup cleanup{[&] { bit_gate->open(); (void)health->disable(); }};
    CHECK(health->registerMetadataCallback(HealthStatusCallback{[&](auto&, auto*) {
        std::lock_guard lock(mutex); order.push_back('S');
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(HealthDetailedCallback{[&](auto&, auto*) {
        std::lock_guard lock(mutex); order.push_back('D');
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(BitCallback{[&](auto&, auto* value) {
        { std::lock_guard lock(mutex); order.push_back('B'); bits.push_back(*value); }
        if (hold.exchange(false)) bit_gate->enter_and_wait(); // Batch already captured.
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(SubsystemCallback{[&](auto& c, auto* value) {
        { std::lock_guard lock(mutex); order.push_back('U'); }
        reports.callback()(c, value);
    }}) == ir::Return::Success);
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success); // Revision 1.
    hold = true;
    CHECK(health->enable() == ir::Return::Success);
    bit_gate->wait_for_entry();
    std::size_t held_bits;
    { std::lock_guard lock(mutex); held_bits = bits.size(); CHECK(held_bits == 1); }
    const auto held_time = bits.front().getCompletedBITs().front().getTimeTag();
    CHECK(reports.count() == 0);
    // C2 completes without waiting for the held Health callback.
    auto executed = std::async(std::launch::async, [&] { return f.c2->send(ir::BIT_Command{43,{1},{},{}}).get(); });
    CHECK(executed.wait_for(2s) == std::future_status::ready);
    auto result = executed.get();
    CHECK(result && *result.get() == ir::Return::Success);
    CHECK(reports.count() == 0);
    bit_gate->open();
    reports.wait(1);
    const auto held = reports.at(0);
    CHECK(revision(held) == 1); // Paired with the held BIT payload, not the newer 2.
    // A subsequent batch carries the new completion and revision together.
    const auto fresh = reports.wait_for([](const auto& r) { return revision(r) == 2; }, 1);
    CHECK(fresh.getStatusSeqNum() > held.getStatusSeqNum());
    CHECK(health->disable() == ir::Return::Success); bit_gate->check();
    std::lock_guard lock(mutex);
    bool newer_payload = false;
    for (std::size_t i = held_bits; i < bits.size(); ++i)
        newer_payload = newer_payload || bits[i].getCompletedBITs().front().getTimeTag() != held_time;
    CHECK(newer_payload);
    // The held batch and the fresh batch both completed before disable, in the
    // appended order; later batches may legally lose a suffix at disable.
    CHECK(order.size() >= 8);
    for (std::size_t i = 0; i < 8; ++i) CHECK(order[i] == "SDBU"[i % 4]);
}


struct SubsystemUnwind {};

// Counting adapter wraps EVERY backend/device/stream operation. Receive is held
// at a known milestone with one delivered buffer outstanding; Subsystem
// registration and publication must add zero calls and mutate nothing.
void gated_subsystem_no_io(const std::shared_ptr<ReceivePlan>& plan, bool unwind_early) {
    auto adapter = std::make_shared<CountingBackend>();
    adapter->inner = std::make_shared<TransformBackend>(Transform::none, plan);
    SubsystemReports reports; // Collector outlives the Health drain below.
    Fixture f(true, adapter);
    std::shared_ptr<ir::HealthStatusChannel> health;
    std::future<ir::Return> disabled;
    // Declared after everything it references: open the receive gate before any
    // future wait or Fixture join, then drain Health while its collector lives.
    TestCleanup cleanup{[&] {
        plan->open();
        if (disabled.valid()) disabled.wait();
        if (health) (void)health->disable();
    }};
    f.add_buffer();
    CHECK(f.image->enable() == ir::Return::Success);
    f.listener->wait_for(1); plan->wait_for(&ReceivePlan::entered);
    const auto frame = f.listener->at(0); const auto bytes = copy_image(frame);
    const auto generation = plan->last_generation.load(); const auto calls = adapter->calls->load();
    const auto width = f.image->getCapabilities().getWidth();
    const auto statistics = gige_mel::provider_statistics(f.manager);
    health = attach_health(f);
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success); reports.wait(3);
    if (unwind_early) throw SubsystemUnwind{}; // Receive gated, Health enabled.
    CHECK(health->disable() == ir::Return::Success);
    for (const auto& report : reports.since(0)) check_state(report, ir::CSCIMode::Imaging, ir::Failure::Available);
    // Zero-I/O measurement completes before shutdown legitimately calls the backend.
    CHECK(adapter->calls->load() == calls && plan->last_generation == generation);
    CHECK(f.image->getCapabilities().getWidth() == width && copy_image(frame) == bytes);
    const auto after = gige_mel::provider_statistics(f.manager);
    CHECK(after.delivered_frames == statistics.delivered_frames && after.stream_failures == 0);
    CHECK(f.image->enable() == ir::Return::Success); // Acquisition unchanged: still owned.
    // Normal shutdown: observe stop_requested before explicitly opening the gate.
    disabled = std::async(std::launch::async, [&] { return f.image->disable(); });
    plan->wait_for(&ReceivePlan::stop_requested);
    plan->open();
    CHECK(disabled.wait_for(2s) == std::future_status::ready);
    CHECK(disabled.get() == ir::Return::Success);
    plan->wait_for_exit(); // Worker exited; also rejects gate expiry.
    { std::lock_guard lock(plan->mutex); CHECK(plan->proceed && !plan->gate_expired); }
    CHECK(gige_mel::provider_statistics(f.manager).stream_failures == 0);
    CHECK(copy_image(frame) == bytes);
    CHECK(f.image->enable() == ir::Return::Fail); // Health did not release the image.
    CHECK(frame.buffer->release() == ir::Return::Success);
    CHECK(f.image->enable() == ir::Return::Success);
    CHECK(f.image->disable() == ir::Return::Success);
}

void subsystem_no_io() { gated_subsystem_no_io(gated_plan(), false); }

void subsystem_no_io_unwind() {
    auto plan = gated_plan(); // Outlives the scenario, its worker and cleanup.
    bool unwound = false;
    try { gated_subsystem_no_io(plan, true); } catch (const SubsystemUnwind&) { unwound = true; }
    CHECK(unwound);
    plan->wait_for_exit();
    std::lock_guard lock(plan->mutex);
    CHECK(plan->entered && plan->proceed && !plan->gate_expired);
}


// Callback context: safe reads/comms/buffer release succeed; same- and
// cross-provider lifecycle/configuration reentry rejects; exceptions contained.
// Assertion failures on the worker are captured and rethrown on this thread.
void subsystem_callbacks() {
    auto a_probe = std::make_shared<ConfigurationProbe>(), b_probe = std::make_shared<ConfigurationProbe>();
    Fixture a(true, std::make_shared<TransformBackend>(Transform::none, nullptr, a_probe));
    Fixture b(true, std::make_shared<TransformBackend>(Transform::none, nullptr, b_probe));
    const auto a_applies = a_probe->applies, b_applies = b_probe->applies;
    auto a_buffer = a.add_buffer(), b_buffer = b.add_buffer();
    CHECK(b.image->enable() == ir::Return::Success); b.listener->wait_for(1);
    auto health = attach_health(a), other = attach_health(b);
    std::mutex mutex;
    std::condition_variable changed;
    unsigned completed = 0;
    std::exception_ptr failure;
    std::optional<ir::Return> released;
    TestCleanup cleanup{[&] { (void)health->disable(); (void)b.image->disable(); }};
    CHECK(health->registerMetadataCallback(SubsystemCallback{[&](auto& channel, auto*) {
        try {
            check_callback_policy(a, a_buffer);
            for (auto* target : {&a, &b}) {
                expect_error(target->c2->send(config_command()), mel::ErrorCode::InvalidState);
                expect_error(target->c2->send(ir::BIT_Command{42,{1},{},{}}), mel::ErrorCode::InvalidState);
            }
            for (auto* h : {health.get(), other.get()}) {
                CHECK(h->enable() == ir::Return::Fail && h->disable() == ir::Return::Fail);
                CHECK(h->registerMetadataCallback(SubsystemCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
                CHECK(h->getCapabilities().getWidth() == 0);
            }
            CHECK(!b.control->attachChannel(b.config(ir::ChannelType::CommandAndControl)));
            CHECK(b.control->detachChannel(other) == ir::Return::Fail);
            CHECK(*response(channel.sendKeepAliveRep()) == ir::Return::Success);
            CHECK(response(channel.send(comms_request(31, 32)))->getRequestID() == 32);
            (void)gige_mel::provider_statistics(b.manager);
            std::lock_guard lock(mutex);
            if (!released) released = b_buffer->release(); // Cross-provider explicit release.
            ++completed;
        } catch (...) {
            std::lock_guard lock(mutex);
            if (!failure) failure = std::current_exception();
        }
        changed.notify_all();
        throw std::runtime_error("deliberate Subsystem consumer exception");
    }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 2s, [&] { return completed >= 3 || failure; }));
        if (failure) std::rethrow_exception(failure);
    }
    CHECK(health->disable() == ir::Return::Success);
    CHECK(released && *released == ir::Return::Success);
    b.listener->wait_for(2); // Released slot resumed delivery on the other provider.
    const auto statistics = gige_mel::provider_statistics(a.manager);
    CHECK(statistics.callback_failures >= 3 && statistics.stream_failures == 0);
    CHECK(statistics.last_error == "Health metadata callback threw; periodic reporting continues");
    CHECK(a_probe->applies == a_applies && b_probe->applies == b_applies);
    CHECK(gige_mel::provider_statistics(b.manager).callback_failures == 0);
    // The contained exception neither faulted Image nor stopped later reporting.
    CHECK(a.image->enable() == ir::Return::Success); a.listener->wait_for(1);
    CHECK(a.image->disable() == ir::Return::Success);
}


// External disable/detach drains an admitted Subsystem callback. A gate in the
// BIT callback (before Subsystem admission) shows independent suffix suppression.
void subsystem_shutdown() {
    for (int path = 0; path < 4; ++path) {
        const bool detach = path % 2 == 1, gate_subsystem = path < 2;
        auto gate = std::make_shared<WatchdogGate>();
        SubsystemReports reports;
        std::atomic<unsigned> bits{0};
        Fixture f;
        auto health = attach_health(f);
        std::atomic<bool> joining{false};
        std::future<ir::Return> stopped;
        TestCleanup cleanup{[&] {
            gate->open();
            if (stopped.valid()) stopped.wait();
            (void)health->disable(); (void)f.control->detachChannel(health);
        }};
        CHECK(health->registerMetadataCallback(BitCallback{[&](auto&, auto*) {
            if (++bits == 1 && !gate_subsystem) gate->enter_and_wait();
        }}) == ir::Return::Success);
        CHECK(health->registerMetadataCallback(SubsystemCallback{[&](auto& c, auto* value) {
            reports.callback()(c, value);
            if (gate_subsystem && reports.count() == 1) gate->enter_and_wait();
        }}) == ir::Return::Success);
        CHECK(health->enable() == ir::Return::Success); gate->wait_for_entry();
        stopped = std::async(std::launch::async, [&] {
            if (detach) return f.control->detachChannel(health);
            test_join::Observe observation(joining); return health->disable();
        });
        if (detach) eventually([&] { return health->registerMetadataCallback(SubsystemCallback{}) == ir::Return::Fail; });
        else eventually([&] { return joining.load(std::memory_order_acquire); });
        CHECK(stopped.wait_for(0ms) == std::future_status::timeout); // Drains the admitted callback.
        gate->open();
        CHECK(stopped.wait_for(2s) == std::future_status::ready && stopped.get() == ir::Return::Success);
        gate->check();
        // Gated in Subsystem: exactly that admitted report. Gated in BIT: the
        // unadmitted Subsystem suffix was suppressed; the gap is valid.
        CHECK(reports.count() == (gate_subsystem ? 1U : 0U) && bits == 1);
        reports.quiet();
        if (!detach) {
            CHECK(health->registerMetadataCallback(BitCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
            CHECK(health->enable() == ir::Return::Success); reports.wait(reports.count() + 1);
            // Delivered or suppressed, the first prepared report consumed
            // sequence 1, so the first report of the new interval carries 2.
            CHECK(reports.last().getStatusSeqNum() == 2);
            CHECK(health->disable() == ir::Return::Success);
        }
    }
}

// Final external owner released from a Subsystem-only callback while Health is
// publishing; the publisher's scoped pins keep callback code safe.
void subsystem_owners() {
    Fixture f;
    auto health = attach_health(f);
    std::weak_ptr<ir::Control> control = f.control;
    std::weak_ptr<ir::HealthStatusChannel> channel = health;
    std::promise<bool> body; auto completed = body.get_future();
    std::promise<void> done; auto exited = done.get_future();
    CHECK(health->registerMetadataCallback(SubsystemCallback{[&](auto& source, auto*) {
        const auto bit = f.c2->send(ir::BIT_Command{42,{1},{},{}}).get();
        const bool rejected = !bit && bit.getError().getCode() == mel::ErrorCode::InvalidState &&
            health->registerMetadataCallback(SubsystemCallback{}) == ir::Return::Fail;
        f.control.reset(); health.reset();
        body.set_value(rejected && !control.expired() && !channel.expired() &&
                       *response(source.sendKeepAliveRep()) == ir::Return::Success);
        done.set_value_at_thread_exit();
    }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    CHECK(completed.wait_for(2s) == std::future_status::ready && completed.get());
    CHECK(exited.wait_for(2s) == std::future_status::ready); // Worker thread exited.
    CHECK(control.expired() && channel.expired());
    expect_error(f.c2->send(ir::BIT_Command{42,{1},{},{}}), mel::ErrorCode::InvalidState);
}


// Executed failure injection: a NEW fault needs the checked UTC clock, which is
// unavailable on the Health worker for exactly one sample. That sample keeps
// Status/Detailed, suppresses BIT and Subsystem, records the diagnostic and
// commits nothing. The next valid sample recovers with an unconsumed sequence.
void subsystem_clock_failure() {
    auto probe = std::make_shared<ConfigurationProbe>();
    auto first_gate = std::make_shared<WatchdogGate>(), failed_gate = std::make_shared<WatchdogGate>();
    SubsystemReports reports;
    BitReports bits;
    Fixture f(true, std::make_shared<TransformBackend>(Transform::none, nullptr, probe));
    auto health = attach_health(f);
    std::unique_ptr<test_clock::Unavailable> unavailable; // Worker-thread only.
    std::atomic<unsigned> statuses{0}, details{0};
    TestCleanup cleanup{[&] { first_gate->open(); failed_gate->open(); (void)health->disable(); }};
    CHECK(health->registerMetadataCallback(HealthStatusCallback{[&](auto&, auto*) {
        const auto count = ++statuses;
        if (count == 1) unavailable = std::make_unique<test_clock::Unavailable>(); // After sample 1 prepared.
        if (count == 2) unavailable.reset(); // Same worker thread, after sample 2 failed.
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(HealthDetailedCallback{[&](auto&, auto*) {
        if (++details == 2) failed_gate->enter_and_wait();
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(bits.callback()) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(SubsystemCallback{[&](auto& c, auto* value) {
        reports.callback()(c, value);
        if (reports.count() == 1) first_gate->enter_and_wait();
    }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success); first_gate->wait_for_entry();
    const auto first = reports.at(0);
    CHECK(first.getStatusSeqNum() == 1 && revision(first) == 0);
    probe->fail_readback = true; (void)f.c2->send(config_command()).get(); // Fresh fault next sample.
    first_gate->open();
    failed_gate->wait_for_entry(); // Sample 2: Status and Detailed published...
    CHECK(statuses == 2 && details == 2 && bits.count() == 1 && reports.count() == 1); // ...BIT/Subsystem not.
    const auto before = wall_now();
    failed_gate->open();
    eventually([&] { return gige_mel::provider_statistics(f.manager).last_error.find(
        "Health BIT publication preparation failed") != std::string::npos; });
    reports.wait(2);
    CHECK(health->disable() == ir::Return::Success);
    first_gate->check(); failed_gate->check();
    CHECK(!unavailable);
    const auto recovered = reports.at(1);
    // Failed preparation committed nothing: exactly one revision for the fault,
    // and the suppressed sample consumed no subsystem sequence.
    CHECK(revision(recovered) == 1 && recovered.getStatusSeqNum() == 2);
    check_state(recovered, ir::CSCIMode::Unknown, ir::Failure::Major);
    const auto paired = bits.at(1);
    CHECK(paired.getFaults().size() == 1);
    CHECK(before <= paired.getFaults().front().getDetectionTime()); // Not an earlier failed attempt's time.
}

