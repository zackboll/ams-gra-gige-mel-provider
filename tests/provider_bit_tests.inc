using BitCallback = std::function<void(ir::Channel&, const mel::BIT_Status*)>;
using CatalogueCallback = std::function<void(ir::Channel&, const mel::BIT_Configuration*)>;

mel::UCI_ID expected_bit_id() {
    return {{0x9c,0x85,0xc2,0x63,0x54,0x6e,0x4e,0x11,0x9b,0x71,0xd2,0x40,0x5a,0x13,0x80,0x01},
            "gige_mel.provider_state.v1"};
}
auto wall_now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()); }

struct BitReports {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<mel::BIT_Status> reports;
    std::vector<std::thread::id> threads;
    BitCallback callback() {
        return [this](auto&, auto* value) {
            { std::lock_guard lock(mutex); reports.push_back(*value); threads.push_back(std::this_thread::get_id()); }
            changed.notify_all();
        };
    }
    void wait(std::size_t count) {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 2s, [&] { return reports.size() >= count; }));
    }
    mel::BIT_Status last() { std::lock_guard lock(mutex); CHECK(!reports.empty()); return reports.back(); }
    mel::BIT_Status at(std::size_t index) { std::lock_guard lock(mutex); CHECK(index < reports.size()); return reports[index]; }
    std::size_t count() { std::lock_guard lock(mutex); return reports.size(); }
    void quiet() {
        std::unique_lock lock(mutex); const auto before = reports.size();
        CHECK(!changed.wait_for(lock, 130ms, [&] { return reports.size() != before; }));
    }
    // Payload freshness, not delivery count: a later callback may carry a batch
    // captured before a synchronous command. Valid only where history started
    // empty and exactly one accepted Pass execution is expected.
    static bool has_expected_completion(const mel::BIT_Status& report) {
        return report.getCompletedBITs().size() == 1 &&
               report.getCompletedBITs().front().getBitID() == expected_bit_id() &&
               report.getCompletedBITs().front().getResult() == mel::BIT_Result::Pass;
    }
    std::optional<mel::BIT_Status> find_completed(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        std::optional<mel::BIT_Status> found;
        (void)changed.wait_for(lock, timeout, [&] {
            const auto match = std::find_if(reports.rbegin(), reports.rend(), has_expected_completion);
            if (match != reports.rend()) found = *match; // Selected while synchronized.
            return found.has_value();
        });
        return found;
    }
    mel::BIT_Status wait_completed() {
        auto found = find_completed(2s);
        CHECK(found);
        return *found;
    }
};

void check_completion(const mel::BIT_Status& report, bool configuration = true, bool acquisition = true) {
    CHECK(report.getActiveBITs().empty());
    CHECK(report.getCompletedBITs().size() == 1);
    const auto& result = report.getCompletedBITs().front();
    CHECK(result.getBitID() == expected_bit_id());
    CHECK(result.getResult() == (configuration && acquisition ? mel::BIT_Result::Pass : mel::BIT_Result::Fail));
    CHECK(result.getFailReason().empty() == (configuration && acquisition));
    CHECK(result.getBitItem().size() == 2);
    const bool passes[] = {configuration, acquisition};
    const char* names[] = {"gige_mel.configuration_known_valid", "gige_mel.owned_acquisition_without_terminal_failure"};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& item = result.getBitItem()[i];
        CHECK(item.getBitItemName() == names[i]);
        CHECK(item.getResult() == (passes[i] ? mel::BIT_Result::Pass : mel::BIT_Result::Fail));
        CHECK(item.getFailReason().empty() == passes[i]);
    }
}

void check_fault(const mel::BIT_Status& report, bool configuration, std::chrono::nanoseconds before,
                 std::chrono::nanoseconds after) {
    CHECK(report.getFaults().size() == 1);
    const auto& fault = report.getFaults().front();
    auto uuid = expected_bit_id().getUUID(); uuid.back() = configuration ? 0x11 : 0x12;
    const std::string code = configuration ? "gige_mel.configuration_invalid" : "gige_mel.acquisition_failed";
    CHECK(fault.getFaultID() == mel::UCI_ID(uuid, code));
    CHECK(fault.getFaultCode() == code);
    CHECK(fault.getSeverity() == mel::FaultSeverity::Warning && fault.getState() == mel::FaultState::Set);
    CHECK(!fault.getFaultDescription().empty());
    CHECK(fault.getComponentID().empty() && fault.getAmbiguityGroup().empty() && fault.getFaultData().empty());
    CHECK(before <= fault.getDetectionTime() && fault.getDetectionTime() <= after);
}

void bit_surface() {
    BitReports reports;
    Fixture f;
    const auto metadata = f.c2->getCapabilities().getChannelMetadataCapabilities();
    CHECK(metadata == (std::set{ir::ChannelMetadataCapabilityType::ChannelCommsTestRep,
        ir::ChannelMetadataCapabilityType::CommandStatus, ir::ChannelMetadataCapabilityType::BITConfiguration,
        ir::ChannelMetadataCapabilityType::BITStatus}));
    for (const auto& cap : f.control->getCapabilities()) {
        if (cap.getChannelTypes().front() == ir::ChannelType::CommandAndControl)
            CHECK(cap.getChannelMetadataCapabilities() == metadata);
    }
    CHECK(f.c2->registerMetadataCallback(CatalogueCallback{}) == ir::Return::BadPointer);
    CHECK(f.c2->registerMetadataCallback(BitCallback{}) == ir::Return::BadPointer);
    unsigned catalogues = 0;
    const auto thread = std::this_thread::get_id();
    CHECK(f.c2->registerMetadataCallback(CatalogueCallback{[&](auto&, auto* catalogue) {
        CHECK(std::this_thread::get_id() == thread);
        CHECK(catalogue->getBit().size() == 1);
        const auto& test = catalogue->getBit().front();
        CHECK(test.getBitID() == expected_bit_id());
        CHECK(test.getAcceptedInterface() == mel::BIT_ControlInterface::SubsystemBITCommand);
        CHECK(test.getBitItemName() == (std::vector<std::string>{"gige_mel.configuration_known_valid",
            "gige_mel.owned_acquisition_without_terminal_failure"}));
        CHECK(test.getSubsystemComponentID().empty() && test.getExpectedDuration() == 0ns);
        ++catalogues;
    }}) == ir::Return::Success);
    CHECK(catalogues == 1);
    CHECK(f.c2->registerMetadataCallback(CatalogueCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
    CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(reports.count() == 1 && reports.threads.front() == thread);
    CHECK(reports.last().getCompletedBITs().empty() && reports.last().getFaults().empty());
    CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Fail);
    CHECK(f.control->detachChannel(f.c2) == ir::Return::Success);
    CHECK(f.c2->registerMetadataCallback(BitCallback{}) == ir::Return::Fail);
    CHECK(f.c2->registerMetadataCallback(CatalogueCallback{}) == ir::Return::Fail);
    expect_error(f.c2->send(ir::BIT_Command{42,{1},{},{}}), mel::ErrorCode::InvalidState);
}

void bit_commands() {
    BitReports reports;
    Fixture f;
    std::vector<ir::CommandStatus> statuses;
    CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(f.c2->registerMetadataCallback(StatusCallback{[&](auto&, auto* value) { statuses.push_back(*value); }}) == ir::Return::Success);
    const auto before = wall_now();
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{0,0},{"ignored"}})) == ir::Return::Success);
    const auto after = wall_now();
    check_completion(reports.last()); check_status(statuses.back(),42,ir::CannotComply::NotSet);
    const auto time = reports.last().getCompletedBITs().front().getTimeTag();
    CHECK(before <= time && time <= after);
    struct Case { ir::BIT_Command command; mel::ErrorCode error; ir::CannotComply reason; };
    const std::vector<Case> cases{
        {{0,{},{},{}},mel::ErrorCode::InvalidParameters,ir::CannotComply::InvalidInputParameter},
        {{1,{1,1},{1},{}},mel::ErrorCode::InvalidParameters,ir::CannotComply::InvalidInputParameter},
        {{2,{0},{1},{}},mel::ErrorCode::InvalidId,ir::CannotComply::UnknownID},
        {{3,{UINT32_MAX},{1},{"ignored"}},mel::ErrorCode::InvalidId,ir::CannotComply::UnknownID},
        {{4,{},{1,1},{"ignored"}},mel::ErrorCode::InvalidParameters,ir::CannotComply::InvalidInputParameter},
        {{5,{},{0},{"ignored"}},mel::ErrorCode::InvalidId,ir::CannotComply::UnknownID},
        {{6,{},{UINT32_MAX},{}},mel::ErrorCode::InvalidId,ir::CannotComply::UnknownID},
        {{7,{},{1},{"ignored",""}},mel::ErrorCode::InvalidState,ir::CannotComply::StateOrSettings},
        {{8,{},{},{"gige_mel.configuration_invalid"}},mel::ErrorCode::Unsupported,ir::CannotComply::CapabilityUnavailable},
        {{9,{1,0,1},{1},{}},mel::ErrorCode::InvalidParameters,ir::CannotComply::InvalidInputParameter}};
    for (const auto& c : cases) {
        const auto count = reports.count();
        expect_error(f.c2->send(c.command), c.error);
        check_status(statuses.back(),c.command.getCommandID(),c.reason);
        CHECK(reports.count() == count && reports.last().getCompletedBITs().front().getTimeTag() == time);
    }
    CHECK(f.c2->enable() == ir::Return::Success && f.c2->disable() == ir::Return::Success);
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    CHECK(reports.count() == 4 && statuses.size() == cases.size()+3); // No duplicate-ID suppression.
    CHECK(f.control->detachChannel(f.c2) == ir::Return::Success);
    f.c2 = std::dynamic_pointer_cast<ir::C2Channel>(f.control->attachChannel(f.config(ir::ChannelType::CommandAndControl)));
    BitReports retained;
    CHECK(f.c2->registerMetadataCallback(retained.callback()) == ir::Return::Success);
    check_completion(retained.last());
    // Completion belongs to the manager/runtime, not the Control or channels.
    CHECK(f.control->detachChannel(f.c2) == ir::Return::Success);
    CHECK(f.control->detachChannel(f.image) == ir::Return::Success);
    f.c2.reset(); f.image.reset(); f.control.reset();
    f.control = ir::getControl("synthetic",f.manager);
    CHECK(f.control && f.control->init() == ir::Return::Success); f.attach();
    CHECK(f.c2->registerMetadataCallback(retained.callback()) == ir::Return::Success);
    check_completion(retained.last());
    Fixture fresh;
    BitReports empty;
    CHECK(fresh.c2->registerMetadataCallback(empty.callback()) == ir::Return::Success);
    CHECK(empty.last().getCompletedBITs().empty());
}

void bit_configuration() {
    auto probe = std::make_shared<ConfigurationProbe>();
    BitReports reports;
    Fixture f(true,std::make_shared<TransformBackend>(Transform::none,nullptr,probe));
    CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    std::optional<std::chrono::nanoseconds> previous_detection;
    std::optional<std::chrono::nanoseconds> previous_completion;
    for (bool invalid : {false,true}) {
        probe->fail_readback = !invalid; probe->invalid_readback = invalid;
        (void)f.c2->send(config_command()).get();
        const auto before = wall_now();
        CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
        auto failed = reports.last(); check_completion(failed,false,true);
        check_fault(failed,true,before,wall_now());
        const auto completion = failed.getCompletedBITs().front().getTimeTag();
        CHECK(before <= completion && completion <= wall_now());
        if(previous_completion && before > *previous_completion) CHECK(completion != *previous_completion);
        previous_completion = completion;
        const auto detection = failed.getFaults().front().getDetectionTime();
        if (previous_detection && before > *previous_detection) CHECK(detection != *previous_detection);
        previous_detection = detection;
        CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
        CHECK(reports.last().getFaults().front().getDetectionTime() == detection);
        const auto retained_time = reports.last().getCompletedBITs().front().getTimeTag();
        expect_error(f.c2->send(ir::BIT_Command{45,{},{},{"gige_mel.configuration_invalid"}}),mel::ErrorCode::Unsupported);
        CHECK(reports.last().getCompletedBITs().front().getTimeTag() == retained_time);
        probe->fail_readback = false; probe->invalid_readback = false;
        CHECK(*response(f.c2->send(config_command())) == ir::Return::Success);
        CHECK(f.control->detachChannel(f.c2) == ir::Return::Success);
        f.c2 = std::dynamic_pointer_cast<ir::C2Channel>(f.control->attachChannel(f.config(ir::ChannelType::CommandAndControl)));
        CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Success);
        CHECK(reports.last().getFaults().empty()); check_completion(reports.last(),false,true);
        CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
        check_completion(reports.last());
        CHECK(!gige_mel::provider_statistics(f.manager).last_error.empty());
    }
}

void bit_acquisition() {
    for (bool callback_failure : {false,true}) {
        auto plan = std::make_shared<ReceivePlan>();
        BitReports reports;
        Fixture f(true,std::make_shared<TransformBackend>(Transform::none,callback_failure ? nullptr : plan));
        CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Success);
        f.add_buffer(); f.listener->throw_after_record = callback_failure;
        CHECK(f.image->enable() == ir::Return::Success);
        if (callback_failure) eventually([&] {
            return gige_mel::provider_statistics(f.manager).last_error ==
                "image callback threw; acquisition stopped, delivered buffer still requires release";
        });
        else { plan->wait_for(&ReceivePlan::entered); plan->open(); plan->wait_for_exit(); }
        const auto before = wall_now();
        CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
        auto failed = reports.last(); check_completion(failed,true,false); check_fault(failed,false,before,wall_now());
        CHECK(f.image->enable() == ir::Return::Fail);
        CHECK(f.image->disable() == ir::Return::Success);
        CHECK(f.control->detachChannel(f.c2) == ir::Return::Success);
        f.c2 = std::dynamic_pointer_cast<ir::C2Channel>(f.control->attachChannel(f.config(ir::ChannelType::CommandAndControl)));
        CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Success);
        CHECK(reports.last().getFaults().empty()); check_completion(reports.last(),true,false);
        CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
        check_completion(reports.last());
    }
}

struct ControlledUnwind {};

// The scripted first stream blocks receive after one delivered frame. The gate
// must be explicitly opened: request_stop only notifies, it never satisfies it.
void gated_no_io_scenario(const std::shared_ptr<ReceivePlan>& plan, bool unwind_early) {
    auto adapter = std::make_shared<CountingBackend>();
    adapter->inner = std::make_shared<TransformBackend>(Transform::none,plan);
    BitReports reports, health_reports;
    Fixture f(true,adapter);
    std::shared_ptr<ir::HealthStatusChannel> health;
    std::future<ir::Return> disabled;
    // Declared after everything it references. Release the receive gate before
    // waiting on stop or letting Fixture destruction join the Image worker, and
    // drain Health while its collector is alive. Never throws or waits on expiry.
    TestCleanup cleanup{[&] {
        plan->open();
        if (disabled.valid()) disabled.wait();
        if (health) (void)health->disable();
    }};
    f.add_buffer();
    CHECK(f.image->enable() == ir::Return::Success);
    f.listener->wait_for(1); plan->wait_for(&ReceivePlan::entered); // Worker deterministically blocked in receive.
    const auto frame = f.listener->at(0); const auto bytes = copy_image(frame);
    const auto generation = plan->last_generation.load(); const auto calls = adapter->calls->load();
    CHECK(f.c2->registerMetadataCallback(CatalogueCallback{[](auto&, auto*) {}}) == ir::Return::Success);
    CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    check_completion(reports.last()); CHECK(reports.last().getFaults().empty());
    health = attach_health(f);
    CHECK(health->registerMetadataCallback(health_reports.callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success); health_reports.wait(3);
    if (unwind_early) throw ControlledUnwind{}; // Receive gated, Health enabled.
    CHECK(health->disable() == ir::Return::Success);
    // Complete the zero-I/O measurement before shutdown legitimately calls the backend.
    CHECK(adapter->calls->load() == calls && plan->last_generation == generation);
    CHECK(copy_image(frame) == bytes && f.image->enable() == ir::Return::Success);
    const auto failures = gige_mel::provider_statistics(f.manager).stream_failures;
    // Observed cancellation precedes gate release, so the scripted terminal
    // result is classified as requested shutdown rather than a stream fault.
    disabled = std::async(std::launch::async, [&] { return f.image->disable(); });
    plan->wait_for(&ReceivePlan::stop_requested);
    plan->open();
    CHECK(disabled.wait_for(2s) == std::future_status::ready);
    CHECK(disabled.get() == ir::Return::Success);
    plan->wait_for_exit(); // Worker exited; also rejects gate_expired.
    { std::lock_guard lock(plan->mutex); CHECK(plan->proceed && !plan->gate_expired); }
    CHECK(gige_mel::provider_statistics(f.manager).stream_failures == failures);
    CHECK(copy_image(frame) == bytes);
    CHECK(f.image->enable() == ir::Return::Fail); // BIT did not release the delivered image.
    CHECK(frame.buffer->release() == ir::Return::Success);
    CHECK(f.image->enable() == ir::Return::Success);
    CHECK(f.image->disable() == ir::Return::Success);
}

std::shared_ptr<ReceivePlan> gated_plan() {
    auto plan = std::make_shared<ReceivePlan>(); plan->frames_before_error = 1; plan->timeouts_before_frame = 3;
    return plan;
}

void bit_no_io() { gated_no_io_scenario(gated_plan(), false); }

void bit_no_io_unwind() {
    auto plan = gated_plan(); // Outlives the scenario, its worker and cleanup.
    bool unwound = false;
    try { gated_no_io_scenario(plan, true); } catch (const ControlledUnwind&) { unwound = true; }
    CHECK(unwound);
    plan->wait_for_exit(); // Cleanup released the gate; the worker drained before expiry.
    std::lock_guard lock(plan->mutex);
    CHECK(plan->entered && plan->proceed && !plan->gate_expired);
}

void bit_callbacks() {
    unsigned statuses = 0;
    Fixture other;
    for (int path = 0; path < 3; ++path) {
        Fixture f;
        std::weak_ptr<ir::Control> control = f.control;
        std::weak_ptr<ir::C2Channel> channel = f.c2;
        unsigned called = 0;
        CHECK(other.c2->registerMetadataCallback(StatusCallback{[&](auto&, auto*) { ++statuses; }}) ==
              (path == 0 ? ir::Return::Success : ir::Return::Fail));
        auto body = [&](ir::Channel& source) {
            ++called;
            expect_error(other.c2->send(ir::BIT_Command{42,{1},{},{}}),mel::ErrorCode::InvalidState);
            expect_error(f.c2->send(ir::BIT_Command{42,{1},{},{}}),mel::ErrorCode::InvalidState);
            CHECK(other.c2->registerMetadataCallback(CatalogueCallback{}) == ir::Return::Fail);
            CHECK(f.c2->registerMetadataCallback(BitCallback{}) == ir::Return::Fail);
            CHECK(*response(source.sendKeepAliveRep()) == ir::Return::Success);
            CHECK(response(source.send(comms_request(10,11)))->getRequestID() == 11);
            CHECK(source.getCapabilities().getChanID() == f.c2->getCapabilities().getChanID());
            f.control.reset(); f.c2.reset();
            CHECK(!control.expired() && !channel.expired());
        };
        auto* raw = f.c2.get();
        if (path == 0) CHECK(raw->registerMetadataCallback(CatalogueCallback{[&](auto& c,auto*) { body(c); }}) == ir::Return::Success);
        if (path == 1) CHECK(raw->registerMetadataCallback(BitCallback{[&](auto& c,auto*) { body(c); }}) == ir::Return::Success);
        if (path == 2) {
            bool initial = true;
            CHECK(raw->registerMetadataCallback(BitCallback{[&](auto& c,auto*) { if (!initial) body(c); }}) == ir::Return::Success);
            initial = false;
            CHECK(*response(raw->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
        }
        CHECK(called == 1 && statuses == 0 && control.expired() && channel.expired());
        CHECK(raw != nullptr); // Never dereference after dispatch pins expire.
    }
    BitReports reports;
    Fixture f;
    unsigned status = 0;
    std::vector<char> order;
    CHECK(f.c2->registerMetadataCallback(CatalogueCallback{[](auto&,auto*) { throw std::runtime_error("catalogue"); }}) == ir::Return::Success);
    CHECK(f.c2->registerMetadataCallback(BitCallback{[&](auto& c,auto* value) {
        order.push_back('B'); reports.callback()(c,value); throw std::runtime_error("BIT status");
    }}) == ir::Return::Success);
    CHECK(f.c2->registerMetadataCallback(StatusCallback{[&](auto&,auto*) { order.push_back('C'); ++status; }}) == ir::Return::Success);
    order.clear();
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    CHECK(status == 1 && gige_mel::provider_statistics(f.manager).callback_failures == 3);
    CHECK(order == (std::vector<char>{'B','C'}));
    check_completion(reports.last()); CHECK(reports.last().getFaults().empty());
}

void bit_concurrent_detach() {
    auto probe = std::make_shared<ConfigurationProbe>();
    auto gate = std::make_shared<WatchdogGate>();
    Fixture f(true,std::make_shared<TransformBackend>(Transform::none,nullptr,probe));
    std::future<std::shared_ptr<ir::Return>> first;
    std::future<ir::Return> detached;
    std::atomic<unsigned> reports{0}, statuses{0};
    std::optional<mel::BIT_Status> captured;
    TestCleanup cleanup{[&] { gate->open(); if(first.valid()) first.wait(); if(detached.valid()) detached.wait(); }};
    CHECK(f.c2->registerMetadataCallback(BitCallback{[&](auto&,auto* value) {
        if (++reports == 2) { gate->enter_and_wait(); captured = *value; }
    }}) == ir::Return::Success);
    CHECK(f.c2->registerMetadataCallback(StatusCallback{[&](auto&,auto*) { ++statuses; }}) == ir::Return::Success);
    first = std::async(std::launch::async,[&] { return response(f.c2->send(ir::BIT_Command{42,{1},{},{}})); });
    gate->wait_for_entry();
    probe->fail_readback = true; (void)f.c2->send(config_command()).get();
    CHECK(*response(f.c2->send(ir::BIT_Command{43,{1},{},{}})) == ir::Return::Success);
    const auto delivered_statuses = statuses.load();
    detached = std::async(std::launch::async,[&] { return f.control->detachChannel(f.c2); });
    eventually([&] { return f.c2->registerMetadataCallback(BitCallback{}) == ir::Return::Fail; });
    CHECK(detached.wait_for(0ms) == std::future_status::timeout);
    gate->open(); CHECK(*first.get() == ir::Return::Success);
    CHECK(detached.get() == ir::Return::Success); gate->check();
    CHECK(statuses == delivered_statuses); // First command's unadmitted status suppressed.
    CHECK(captured); check_completion(*captured); // Later failed completion cannot replace command one's payload.
}

void bit_health() {
    for (bool detach : {false,true}) {
        auto gate = std::make_shared<WatchdogGate>();
        BitReports reports;
        Fixture f;
        auto health = attach_health(f);
        std::vector<char> order;
        std::future<ir::Return> stopped;
        std::atomic<bool> joining{false};
        TestCleanup cleanup{[&] { gate->open(); if(stopped.valid()) stopped.wait(); (void)health->disable(); }};
        CHECK(health->registerMetadataCallback(HealthStatusCallback{[&](auto&,auto*) { order.push_back('S'); gate->enter_and_wait(); }}) == ir::Return::Success);
        CHECK(health->registerMetadataCallback(HealthDetailedCallback{[&](auto&,auto*) { order.push_back('D'); }}) == ir::Return::Success);
        CHECK(health->registerMetadataCallback(BitCallback{[&](auto& c,auto* value) { order.push_back('B'); reports.callback()(c,value); }}) == ir::Return::Success);
        reports.quiet();
        CHECK(health->enable() == ir::Return::Success); gate->wait_for_entry();
        stopped = std::async(std::launch::async,[&] {
            if(detach) return f.control->detachChannel(health);
            test_join::Observe observation(joining); return health->disable();
        });
        if(detach) eventually([&] { return health->registerMetadataCallback(BitCallback{}) == ir::Return::Fail; });
        else eventually([&] { return joining.load(std::memory_order_acquire); });
        CHECK(stopped.wait_for(0ms) == std::future_status::timeout);
        gate->open(); CHECK(stopped.get() == ir::Return::Success); gate->check();
        CHECK(order == std::vector<char>{'S'} && reports.count() == 0);
        if(detach) health = attach_health(f);
        else CHECK(f.control->detachChannel(health) == ir::Return::Success);
        if(!detach) health = attach_health(f);
        CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success);
        CHECK(health->enable() == ir::Return::Success); reports.wait(1);
        CHECK(reports.last().getCompletedBITs().empty());
        CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
        check_completion(reports.wait_completed()); // Fresh payload, not merely a later callback.
        CHECK(health->disable() == ir::Return::Success); reports.quiet();
        for(auto thread : reports.threads) CHECK(thread != std::this_thread::get_id());
        const auto stopped_count = reports.count(); // Baseline after disable has quiesced.
        CHECK(health->enable() == ir::Return::Success); reports.wait(stopped_count+1);
        check_completion(reports.at(stopped_count)); // First report of the new enable interval.
        CHECK(health->disable() == ir::Return::Success);
    }
    // Late BIT-only registration prompts the existing worker, not caller dispatch.
    BitReports reports;
    Fixture f;
    auto health = attach_health(f);
    CHECK(health->enable() == ir::Return::Success);
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success); reports.wait(1);
    CHECK(health->disable() == ir::Return::Success);
    CHECK(health->registerMetadataCallback(BitCallback{}) == ir::Return::BadPointer);
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Fail);
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    health = attach_health(f);
    const auto count = reports.count();
    CHECK(health->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success); reports.wait(count+1);
    CHECK(health->disable() == ir::Return::Success);
}

// Controlled legal ordering: a Health batch captured before a synchronous C2
// command is delivered after the command returns. Its BIT callback is held after
// recording/notify and outside the collector mutex, so no newer batch can hide it.
void bit_health_stale_batch() {
    auto status_gate = std::make_shared<WatchdogGate>();
    auto bit_gate = std::make_shared<WatchdogGate>();
    BitReports reports; // Outlives Fixture and every Health callback.
    std::mutex mutex;
    std::vector<char> order;
    std::size_t statuses = 0;
    Fixture f;
    auto health = attach_health(f);
    TestCleanup cleanup{[&] { status_gate->open(); bit_gate->open(); (void)health->disable(); }};
    CHECK(health->registerMetadataCallback(HealthStatusCallback{[&](auto&,auto*) {
        bool second;
        { std::lock_guard lock(mutex); order.push_back('S'); second = ++statuses == 2; }
        if(second) status_gate->enter_and_wait(); // Batch two already captured.
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(HealthDetailedCallback{[&](auto&,auto*) {
        std::lock_guard lock(mutex); order.push_back('D');
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(BitCallback{[&](auto& c,auto* value) {
        { std::lock_guard lock(mutex); order.push_back('B'); }
        reports.callback()(c,value); // Recorded and notified first.
        if(reports.count() == 2) bit_gate->enter_and_wait();
    }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    reports.wait(1); CHECK(reports.at(0).getCompletedBITs().empty());
    status_gate->wait_for_entry();
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    const auto count = reports.count(); CHECK(count == 1);
    status_gate->open();
    bit_gate->wait_for_entry();
    // Old observation: count+1 is satisfied while the delivered payload predates
    // the accepted command. check_completion(reports.last()) would fail here.
    reports.wait(count+1);
    CHECK(reports.count() == count+1);
    const auto stale = reports.last();
    CHECK(stale.getCompletedBITs().empty());
    CHECK(!BitReports::has_expected_completion(stale));
    CHECK(!reports.find_completed(0ms)); // Corrected predicate rejects it.
    bit_gate->open();
    check_completion(reports.wait_completed()); // Later fresh batch.
    CHECK(health->disable() == ir::Return::Success); reports.quiet();
    status_gate->check(); bit_gate->check();
    // Re-enable: enough stopped-state reports already satisfy the old count+2.
    const auto stopped = reports.count();
    CHECK(stopped >= count+2);
    CHECK(health->enable() == ir::Return::Success);
    reports.wait(stopped+1); // Requires a report from this enable interval.
    check_completion(reports.at(stopped));
    CHECK(health->disable() == ir::Return::Success);
    // The gated batches were complete and ordered; later disables may legally
    // suppress a suffix, so only the controlled prefix is asserted.
    std::lock_guard lock(mutex);
    CHECK(order.size() >= 6);
    for(std::size_t i=0;i<6;++i) CHECK(order[i] == "SDB"[i%3]);
}

void bit_health_owners() {
    Fixture f;
    auto health = attach_health(f);
    std::weak_ptr<ir::Control> control = f.control;
    std::weak_ptr<ir::HealthStatusChannel> channel = health;
    std::promise<void> done; auto exited = done.get_future();
    CHECK(health->registerMetadataCallback(BitCallback{[&](auto& source,auto*) {
        expect_error(f.c2->send(ir::BIT_Command{42,{1},{},{}}),mel::ErrorCode::InvalidState);
        CHECK(health->registerMetadataCallback(BitCallback{}) == ir::Return::Fail);
        CHECK(f.c2->registerMetadataCallback(CatalogueCallback{}) == ir::Return::Fail);
        f.control.reset(); health.reset();
        CHECK(!control.expired() && !channel.expired());
        CHECK(*response(source.sendKeepAliveRep()) == ir::Return::Success);
        done.set_value_at_thread_exit();
    }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    CHECK(exited.wait_for(2s) == std::future_status::ready);
    CHECK(control.expired() && channel.expired());
    CHECK(f.c2->registerMetadataCallback(BitCallback{}) == ir::Return::Fail);
    CHECK(f.c2->registerMetadataCallback(CatalogueCallback{}) == ir::Return::Fail);
    expect_error(f.c2->send(ir::BIT_Command{42,{1},{},{}}),mel::ErrorCode::InvalidState);
}

void bit_registration_drain() {
    for (bool catalogue : {false,true}) {
        auto gate = std::make_shared<WatchdogGate>();
        Fixture f;
        std::future<ir::Return> registered, detached;
        TestCleanup cleanup{[&] { gate->open(); if(registered.valid()) registered.wait(); if(detached.valid()) detached.wait(); }};
        registered = std::async(std::launch::async,[&] {
            if(catalogue) return f.c2->registerMetadataCallback(CatalogueCallback{[&](auto&,auto*) { gate->enter_and_wait(); }});
            return f.c2->registerMetadataCallback(BitCallback{[&](auto&,auto*) { gate->enter_and_wait(); }});
        });
        gate->wait_for_entry();
        detached = std::async(std::launch::async,[&] { return f.control->detachChannel(f.c2); });
        eventually([&] { return f.c2->registerMetadataCallback(BitCallback{}) == ir::Return::Fail; });
        CHECK(detached.wait_for(0ms) == std::future_status::timeout);
        gate->open(); CHECK(registered.get() == ir::Return::Success && detached.get() == ir::Return::Success);
        gate->check();
    }
}

void bit_health_coherent() {
    auto probe = std::make_shared<ConfigurationProbe>();
    auto gate = std::make_shared<WatchdogGate>();
    BitReports reports;
    Fixture f(true,std::make_shared<TransformBackend>(Transform::none,nullptr,probe));
    auto health = attach_health(f);
    std::mutex mutex;
    std::vector<char> order;
    std::vector<mel::MFA_Status> statuses;
    std::vector<mel::MFA_StatusDetailed> details;
    TestCleanup cleanup{[&] { gate->open(); (void)health->disable(); }};
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(HealthStatusCallback{[&](auto&,auto* value) {
        bool first;
        { std::lock_guard lock(mutex); statuses.push_back(*value); order.push_back('S'); first = statuses.size() == 1; }
        if(first) gate->enter_and_wait();
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(HealthDetailedCallback{[&](auto&,auto* value) {
        std::lock_guard lock(mutex); details.push_back(*value); order.push_back('D');
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(BitCallback{[&](auto& c,auto* value) {
        { std::lock_guard lock(mutex); order.push_back('B'); }
        reports.callback()(c,value);
        throw std::runtime_error("contained Health BIT callback");
    }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success); gate->wait_for_entry();
    probe->fail_readback = true; (void)f.c2->send(config_command()).get();
    gate->open(); reports.wait(2);
    CHECK(health->disable() == ir::Return::Success); gate->check();
    CHECK(statuses.front().getMFAState() == mel::MFA_State::Standby);
    CHECK(HealthReports::values(details.front()).at("gige_mel.configuration_valid") == "true");
    CHECK(reports.reports.front().getFaults().empty());
    CHECK(statuses.back().getMFAState() == mel::MFA_State::Degraded);
    CHECK(HealthReports::values(details.back()).at("gige_mel.configuration_valid") == "false");
    CHECK(reports.last().getFaults().size() == 1); check_completion(reports.last()); // Earlier Pass, fresh current fault.
    for(std::size_t i=0;i<order.size();++i) CHECK(order[i] == "SDB"[i%3]);
    CHECK(gige_mel::provider_statistics(f.manager).callback_failures >= 2);
    probe->fail_readback = false; CHECK(*response(f.c2->send(config_command())) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    const auto count = reports.count(); reports.wait(count+1);
    CHECK(health->disable() == ir::Return::Success);
    CHECK(reports.last().getFaults().empty());
}

void bit_nonterminal() {
    BitReports reports;
    Fixture f;
    f.add_buffer();
    CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(f.image->enable() == ir::Return::Success); f.listener->wait_for(1);
    const auto frame = f.listener->at(0); const auto bytes = copy_image(frame);
    eventually([&] { return gige_mel::provider_statistics(f.manager).dropped_no_buffer > 1; });
    expect_error(f.c2->send(config_command()),mel::ErrorCode::InvalidState);
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    check_completion(reports.last()); CHECK(reports.last().getFaults().empty());
    CHECK(copy_image(frame) == bytes);
    auto health = attach_health(f);
    std::promise<void> released; auto done = released.get_future();
    std::atomic<unsigned> callbacks{0};
    TestCleanup cleanup{[&] { (void)health->disable(); }};
    CHECK(health->registerMetadataCallback(BitCallback{[&](auto&,auto*) {
        if(callbacks++ == 0) { CHECK(frame.buffer->release() == ir::Return::Success); released.set_value(); }
    }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    CHECK(done.wait_for(2s) == std::future_status::ready); f.listener->wait_for(2);
    CHECK(health->disable() == ir::Return::Success);
    CHECK(f.image->disable() == ir::Return::Success);
}

void bit_clock_failure() {
    BitReports reports;
    std::vector<ir::CommandStatus> statuses;
    Fixture f;
    CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(f.c2->registerMetadataCallback(StatusCallback{[&](auto&,auto* value) { statuses.push_back(*value); }}) == ir::Return::Success);
    CHECK(*response(f.c2->send(ir::BIT_Command{42,{1},{},{}})) == ir::Return::Success);
    const auto time = reports.last().getCompletedBITs().front().getTimeTag();
    {
        test_clock::Unavailable clock;
        expect_error(f.c2->send(ir::BIT_Command{43,{1},{},{}}),mel::ErrorCode::InsufficientLocalResources);
    }
    CHECK(reports.count() == 2);
    check_status(statuses.back(),43,ir::CannotComply::InsufficientResources);
    CHECK(statuses.back().getReasonDescription().find("clock_gettime") != std::string::npos);
    CHECK(f.control->detachChannel(f.c2) == ir::Return::Success);
    f.c2 = std::dynamic_pointer_cast<ir::C2Channel>(f.control->attachChannel(f.config(ir::ChannelType::CommandAndControl)));
    CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(reports.last().getCompletedBITs().front().getTimeTag() == time);
}

void bit_initial_faults() {
    auto probe = std::make_shared<ConfigurationProbe>();
    BitReports c2_reports, health_reports;
    Fixture f(true,std::make_shared<TransformBackend>(Transform::none,nullptr,probe));
    probe->fail_readback = true; (void)f.c2->send(config_command()).get();
    auto health = attach_health(f);
    CHECK(health->registerMetadataCallback(health_reports.callback()) == ir::Return::Success);
    {
        test_clock::Unavailable clock;
        CHECK(f.c2->registerMetadataCallback(c2_reports.callback()) == ir::Return::Fail);
    }
    const auto before = wall_now();
    CHECK(f.c2->registerMetadataCallback(c2_reports.callback()) == ir::Return::Success);
    CHECK(c2_reports.last().getCompletedBITs().empty()); check_fault(c2_reports.last(),true,before,wall_now());
    CHECK(health->enable() == ir::Return::Success); health_reports.wait(1);
    CHECK(health->disable() == ir::Return::Success);
    CHECK(health_reports.last().getCompletedBITs().empty());
    CHECK(health_reports.last().getFaults().front().getDetectionTime() == c2_reports.last().getFaults().front().getDetectionTime());
}

void bit_health_clock_failure() {
    auto probe = std::make_shared<ConfigurationProbe>();
    auto gate = std::make_shared<WatchdogGate>();
    auto first_gate = std::make_shared<WatchdogGate>();
    BitReports reports;
    Fixture f(true,std::make_shared<TransformBackend>(Transform::none,nullptr,probe));
    auto health = attach_health(f);
    std::unique_ptr<test_clock::Unavailable> unavailable;
    unsigned statuses = 0, details = 0;
    TestCleanup cleanup{[&] { first_gate->open(); gate->open(); (void)health->disable(); }};
    CHECK(health->registerMetadataCallback(HealthStatusCallback{[&](auto&,auto*) {
        ++statuses;
        if(statuses == 1) unavailable = std::make_unique<test_clock::Unavailable>();
        if(statuses == 2) unavailable.reset(); // Reset on this same worker thread.
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(HealthDetailedCallback{[&](auto&,auto*) {
        if(++details == 2) gate->enter_and_wait();
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(BitCallback{[&](auto& c,auto* value) {
        reports.callback()(c,value);
        if(reports.count() == 1) first_gate->enter_and_wait();
    }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success); first_gate->wait_for_entry();
    // The first sample was prepared before its Status armed the worker's clock
    // failure. Force a fresh fault observation requiring a timestamp next sample.
    probe->fail_readback = true; (void)f.c2->send(config_command()).get();
    first_gate->open();
    gate->wait_for_entry();
    CHECK(statuses == 2 && details == 2 && reports.count() == 1);
    gate->open();
    eventually([&] { return gige_mel::provider_statistics(f.manager).last_error.find("Health BIT publication preparation failed") != std::string::npos; });
    reports.wait(2); // Later reporting continues with a valid fresh fault timestamp.
    CHECK(health->disable() == ir::Return::Success); gate->check();
    CHECK(!unavailable && statuses >= 3 && details >= 3);
    CHECK(reports.last().getFaults().size() == 1);
}