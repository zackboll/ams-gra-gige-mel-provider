// Included after the existing ModeCmd scenarios; all synchronization is test-only.
void mode_active_cancel_join_and_repeat() {
    auto plan = std::make_shared<ReceivePlan>();
    Fixture f(true, std::make_shared<TransformBackend>(Transform::none, plan));
    f.add_buffer();
    std::atomic<unsigned> statuses{0};
    std::atomic<bool> joining{false};
    CHECK(f.c2->registerMetadataCallback(StatusCallback{[&](auto& channel, auto* s) {
        CHECK(&channel == f.c2.get());
        check_status(*s, 0);
        CHECK(plan->worker_exit.wait_for(0ms) == std::future_status::ready);
        CHECK(plan->stops >= 2); // Existing worker stop plus owner's idempotent stop.
        ++statuses;
    }}) == ir::Return::Success);
    CHECK(f.image->enable() == ir::Return::Success);
    plan->wait_for(&ReceivePlan::entered);
    auto command = std::async(std::launch::async, [&] {
        test_join::Observe observed(joining);
        return f.c2->send(standby_command(0));
    });
    TestCleanup cleanup{[&] { plan->open(); }};
    plan->wait_for(&ReceivePlan::stop_requested);
    eventually([&] { return joining.load(); });
    CHECK(command.wait_for(0ms) == std::future_status::timeout && statuses == 0);
    plan->open();
    CHECK(*response(command.get()) == ir::MFA_Mode::Unused);
    plan->wait_for_exit();
    CHECK(statuses == 1 && plan->cancellations == 1 && plan->creations == 1);
    const auto stops = plan->stops.load();
    CHECK(*response(f.c2->send(standby_command(0))) == ir::MFA_Mode::Unused);
    CHECK(statuses == 2 && plan->stops == stops && plan->cancellations == 1 && plan->creations == 1);
    f.listener->require_quiet();
    CHECK(gige_mel::provider_statistics(f.manager).stream_failures == 0);
    CHECK(f.image->enable() == ir::Return::Success);
    f.listener->wait_for(1);
    CHECK(f.image->disable() == ir::Return::Success);
}

void mode_shutdown_serializations() {
    // Force Standby to own operations first. Every competing operation must
    // wait for the receiving worker, not race a partially unpublished stream.
    for (unsigned action = 0; action != 7; ++action) {
        auto plan = std::make_shared<ReceivePlan>();
        Fixture f(true, std::make_shared<TransformBackend>(Transform::none, plan));
        f.add_buffer();
        std::atomic<bool> joining{false};
        CHECK(f.image->enable() == ir::Return::Success);
        plan->wait_for(&ReceivePlan::entered);
        auto mode = std::async(std::launch::async, [&] {
            test_join::Observe observed(joining);
            return f.c2->send(standby_command());
        });
        TestCleanup mode_cleanup{[&] { plan->open(); }};
        eventually([&] { return joining.load(); });
        auto contender = std::async(std::launch::async, [&] {
            switch (action) {
            case 0: return f.image->enable() == ir::Return::Success;
            case 1: return f.image->disable() == ir::Return::Success;
            case 2: return response(f.c2->send(camera_command()))->getCommandSuccess();
            case 3: return *response(f.c2->send(config_command())) == ir::Return::Success;
            case 4: return f.control->detachChannel(f.image) == ir::Return::Success;
            case 5: return f.control->detachChannel(f.c2) == ir::Return::Success;
            default: return *response(f.c2->send(standby_command(UINT32_MAX))) == ir::MFA_Mode::Unused;
            }
        });
        TestCleanup contender_cleanup{[&] { plan->open(); }};
        CHECK(contender.wait_for(0ms) == std::future_status::timeout);
        plan->open();
        CHECK(*response(mode.get()) == ir::MFA_Mode::Unused);
        CHECK(contender.get());
        CHECK(plan->cancellations == 1);
        if (action == 0) {
            CHECK(plan->creations == 2);
            f.listener->wait_for(1);
        } else CHECK(plan->creations == 1);
    }

    // Reverse ordering: disable or Image detach owns operations before Standby.
    for (bool detach : {false, true}) {
        auto plan = std::make_shared<ReceivePlan>();
        Fixture f(true, std::make_shared<TransformBackend>(Transform::none, plan)); f.add_buffer();
        CHECK(f.image->enable() == ir::Return::Success); plan->wait_for(&ReceivePlan::entered);
        std::atomic<bool> joining{false};
        auto stop = std::async(std::launch::async, [&] {
            test_join::Observe observed(joining);
            return detach ? f.control->detachChannel(f.image) : f.image->disable();
        });
        TestCleanup stop_cleanup{[&] { plan->open(); }};
        eventually([&] { return joining.load(); });
        auto mode = std::async(std::launch::async, [&] { return f.c2->send(standby_command()); });
        TestCleanup mode_cleanup{[&] { plan->open(); }};
        CHECK(mode.wait_for(0ms) == std::future_status::timeout);
        plan->open();
        CHECK(stop.get() == ir::Return::Success);
        CHECK(*response(mode.get()) == ir::MFA_Mode::Unused);
        CHECK(plan->cancellations == 1 && plan->creations == 1);
    }

    // Configuration first while active must reject, without a device apply;
    // the subsequent Standby and explicit reconfiguration both succeed.
    for (bool config_set : {false, true}) {
        auto probe = std::make_shared<ConfigurationProbe>();
        Fixture f(true, std::make_shared<TransformBackend>(Transform::none, nullptr, probe)); f.add_buffer();
        CHECK(f.image->enable() == ir::Return::Success); f.listener->wait_for(1);
        const auto applies = probe->applies;
        if (config_set) expect_error(f.c2->send(config_command()), mel::ErrorCode::InvalidState);
        else expect_error(f.c2->send(camera_command()), mel::ErrorCode::InvalidState);
        CHECK(probe->applies == applies && f.image->enable() == ir::Return::Success);
        CHECK(*response(f.c2->send(standby_command())) == ir::MFA_Mode::Unused);
        f.release_buffers();
        if (config_set) CHECK(*response(f.c2->send(config_command())) == ir::Return::Success);
        else CHECK(response(f.c2->send(camera_command()))->getCommandSuccess());
    }
}

void mode_active_lifetime_and_simultaneous() {
    // A last external Control release cannot start teardown during a joined
    // transition. The command pins all three participating provider objects.
    auto plan = std::make_shared<ReceivePlan>();
    Fixture f(true, std::make_shared<TransformBackend>(Transform::none, plan)); f.add_buffer();
    std::weak_ptr<ir::Control> control = f.control;
    std::weak_ptr<ir::ImageChannel> image = f.image;
    std::weak_ptr<ir::C2Channel> c2 = f.c2;
    CHECK(f.image->enable() == ir::Return::Success); plan->wait_for(&ReceivePlan::entered);
    std::atomic<bool> joining{false};
    auto command_channel = f.c2;
    auto mode = std::async(std::launch::async, [&, command_channel] {
        test_join::Observe observed(joining);
        return command_channel->send(standby_command());
    });
    TestCleanup cleanup{[&] { plan->open(); }};
    eventually([&] { return joining.load(); });
    f.control.reset(); f.image.reset(); f.c2.reset(); command_channel.reset();
    CHECK(!control.expired() && !image.expired() && !c2.expired());
    plan->open(); CHECK(*response(mode.get()) == ir::MFA_Mode::Unused);
    CHECK(control.expired() && image.expired() && c2.expired());

    // Reverse admission: closing/detached C2 wins even for invalid payloads.
    Fixture closed; closed.add_buffer(); CHECK(closed.image->enable() == ir::Return::Success);
    CHECK(closed.control->detachChannel(closed.c2) == ir::Return::Success);
    expect_error(closed.c2->send(standby_command()), mel::ErrorCode::InvalidState);
    CHECK(closed.image->enable() == ir::Return::Success);
    closed.control.reset();
    expect_error(closed.c2->send(standby_command()), mel::ErrorCode::InvalidState);

    auto teardown_plan = std::make_shared<ReceivePlan>();
    Fixture tearing_down(true, std::make_shared<TransformBackend>(Transform::none, teardown_plan));
    tearing_down.add_buffer(); CHECK(tearing_down.image->enable() == ir::Return::Success);
    teardown_plan->wait_for(&ReceivePlan::entered);
    std::atomic<bool> teardown_join{false};
    auto teardown = std::async(std::launch::async, [&, owner = std::move(tearing_down.control)]() mutable {
        test_join::Observe observed(teardown_join); owner.reset();
    });
    TestCleanup teardown_cleanup{[&] { teardown_plan->open(); }};
    eventually([&] { return teardown_join.load(); }); // Control destruction owns operations.
    expect_error(tearing_down.c2->send(standby_command()), mel::ErrorCode::InvalidState);
    teardown_plan->open(); teardown.get();

    Fixture many; many.add_buffer(); CHECK(many.image->enable() == ir::Return::Success);
    many.listener->wait_for(1);
    std::mutex mutex;
    std::vector<std::uint32_t> ids;
    CHECK(many.c2->registerMetadataCallback(StatusCallback{[&](auto& ch, auto* s) {
        CHECK(&ch == many.c2.get()); check_status(*s, s->getCommandID());
        std::lock_guard lock(mutex); ids.push_back(s->getCommandID());
    }}) == ir::Return::Success);
    WatchdogGate gate;
    std::vector<std::future<mel::RequestFor<ir::MFA_Mode>>> requests;
    TestCleanup requests_cleanup{[&] { gate.open(); }};
    for (std::uint32_t id = 0; id != 8; ++id)
        requests.push_back(std::async(std::launch::async, [&, id] {
            gate.enter_and_wait(); return many.c2->send(standby_command(id));
        }));
    gate.wait_for_entry(); gate.open();
    for (auto& request : requests) CHECK(*response(request.get()) == ir::MFA_Mode::Unused);
    std::sort(ids.begin(), ids.end()); CHECK(ids == (std::vector<std::uint32_t>{0,1,2,3,4,5,6,7}));
    CHECK(many.image->enable() == ir::Return::Fail);
    many.listener->require_quiet(); gate.check();
}

void mode_active_noninterference() {
    Fixture f;
    unsigned navigation = 0, defects = 0;
    CHECK(f.image->registerMetadataCallback(std::function<void(ir::Channel&, const ir::NavigationReportResp*)>{
        [&](auto&, auto*) { ++navigation; }}) == ir::Return::Success);
    CHECK(f.image->registerMetadataCallback(std::function<void(ir::Channel&, const ir::BadPixelList*)>{
        [&](auto&, auto*) { ++defects; }}) == ir::Return::Success);
    CHECK(response(f.image->send(navigation_test_data(10ns)))->getSystemTime() == 10ns);
    f.add_buffer(); CHECK(f.image->enable() == ir::Return::Success); f.listener->wait_for(1);
    const auto bytes = f.storage.front();
    const auto header = f.listener->at(0).header;
    CHECK(*response(f.c2->send(standby_command())) == ir::MFA_Mode::Unused);
    CHECK(f.storage.front() == bytes && navigation == 1 && defects == 1);
    expect_error(f.image->send(navigation_test_data(10ns)), mel::ErrorCode::InvalidState);
    CHECK(response(f.image->send(navigation_test_data(11ns)))->getSystemTime() == 11ns);
    CHECK(navigation == 2 && defects == 1);
    CHECK(f.buffers.front()->release() == ir::Return::Success);
    CHECK(f.image->enable() == ir::Return::Success); f.listener->wait_for(2);
    CHECK(f.image->disable() == ir::Return::Success);
    const auto next = f.listener->at(1).header;
    CHECK(header.getSensorNavState().empty() && next.getSensorNavState().empty());
    CHECK(header.getSensorInertialState().empty() && next.getSensorInertialState().empty());
}

void mode_fault_bit_retirement() {
    BitReports reports, recovered;
    auto plan = std::make_shared<ReceivePlan>();
    Fixture f(true, std::make_shared<TransformBackend>(Transform::none, plan));
    f.add_buffer();
    CHECK(f.image->enable() == ir::Return::Success);
    TestCleanup cleanup{[&] { plan->open(); }};
    plan->wait_for(&ReceivePlan::entered); plan->open(); plan->wait_for_exit();
    CHECK(f.c2->registerMetadataCallback(reports.callback()) == ir::Return::Success);
    CHECK(reports.last().getFaults().size() == 1);
    CHECK(*response(f.c2->send(ir::BIT_Command{17, {1}, {}, {}})) == ir::Return::Success);
    const auto failed = reports.last(); check_completion(failed, true, false);
    const auto history = gige_mel::provider_statistics(f.manager);
    CHECK(*response(f.c2->send(standby_command())) == ir::MFA_Mode::Unused);
    CHECK(reports.count() == 2); // Standby did not execute BIT or publish a new completion.
    CHECK(f.control->detachChannel(f.c2) == ir::Return::Success);
    f.c2 = std::dynamic_pointer_cast<ir::C2Channel>(f.control->attachChannel(f.config(ir::ChannelType::CommandAndControl)));
    CHECK(f.c2->registerMetadataCallback(recovered.callback()) == ir::Return::Success);
    CHECK(recovered.last().getFaults().empty()); // Existing observation retires the current fault.
    check_completion(recovered.last(), true, false); // Historical failed BIT remains.
    CHECK(recovered.last().getCompletedBITs().front().getTimeTag() == failed.getCompletedBITs().front().getTimeTag());
    CHECK(gige_mel::provider_statistics(f.manager).last_error == history.last_error);
    CHECK(gige_mel::provider_statistics(f.manager).stream_failures == history.stream_failures);
    CHECK(f.image->enable() == ir::Return::Success); f.listener->wait_for(1);
    CHECK(f.image->disable() == ir::Return::Success);
}