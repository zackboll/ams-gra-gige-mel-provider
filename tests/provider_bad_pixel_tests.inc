// Included in provider_tests.cpp: same fixtures, watchdogs and public contracts.
using BadPixelCallback = std::function<void(ir::Channel&, const ir::BadPixelList*)>;

void bad_pixels_sessions() {
    Fixture f, other;
    for (const auto& cap : f.control->getCapabilities())
        CHECK(cap.getChannelMetadataCapabilities().contains(ir::ChannelMetadataCapabilityType::BadPixelList) ==
            (cap.getChannelTypes() == std::vector{ir::ChannelType::IRSTImage}));
    auto health = attach_health(f);
    CHECK(!health->getCapabilities().getChannelMetadataCapabilities().contains(ir::ChannelMetadataCapabilityType::BadPixelList));
    CHECK(!f.c2->getCapabilities().getChannelMetadataCapabilities().contains(ir::ChannelMetadataCapabilityType::BadPixelList));
    CHECK(f.image->getCapabilities().getChannelMetadataCapabilities().contains(ir::ChannelMetadataCapabilityType::BadPixelList));
    CHECK(f.image->registerMetadataCallback(BadPixelCallback{}) == ir::Return::BadPointer);
    unsigned calls = 0, other_calls = 0, camera_calls = 0, navigation_calls = 0, statuses = 0;
    bool valid = false;
    CHECK(f.image->registerMetadataCallback(std::function<void(ir::Channel&, const ir::CameraCommandResp*)>{
        [&](auto&, auto*) { ++camera_calls; }}) == ir::Return::Success);
    CHECK(f.image->registerMetadataCallback(std::function<void(ir::Channel&, const ir::NavigationReportResp*)>{
        [&](auto&, auto*) { ++navigation_calls; }}) == ir::Return::Success);
    CHECK(f.c2->registerMetadataCallback(std::function<void(ir::Channel&, const ir::CommandStatus*)>{
        [&](auto&, auto*) { ++statuses; }}) == ir::Return::Success);
    const auto before = gige_mel::provider_statistics(f.manager);
    CHECK(f.image->registerMetadataCallback(BadPixelCallback{[&](auto& channel, auto* list) {
        ++calls;
        valid = &channel == f.image.get() && list && list->getSize() == 0 &&
            list->getBadPixelCount() == 0 && list->getBadPixelList().empty();
    }}) == ir::Return::Success);
    CHECK(calls == 1 && valid && f.listener->count() == 0);
    CHECK(camera_calls == 0 && navigation_calls == 0 && statuses == 0);
    CHECK(gige_mel::provider_statistics(f.manager).callback_failures == before.callback_failures);
    CHECK(f.image->registerMetadataCallback(BadPixelCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
    CHECK(f.image->registerMetadataCallback(BadPixelCallback{}) == ir::Return::BadPointer);
    CHECK(other.image->registerMetadataCallback(BadPixelCallback{[&](auto&, auto*) { ++other_calls; }}) == ir::Return::Success);
    CHECK(other_calls == 1 && calls == 1);
    CHECK(response(f.image->send(navigation_test_data(1ns)))->getSystemTime() == 1ns);
    auto command = camera_command(71, 48, 32);
    command.setColumnOffset(4); command.setRowOffset(6);
    command.setFrameRate(60); command.setIntegrationTime(2ms);
    CHECK(response(f.c2->send(command))->getCommandSuccess());
    CHECK(calls == 1 && camera_calls == 1 && statuses == 1 && navigation_calls == 1);
    CHECK(f.image->getCapabilities().getChannelMetadataCapabilities().contains(ir::ChannelMetadataCapabilityType::BadPixelList));
    f.add_buffer();
    CHECK(f.image->enable() == ir::Return::Success); f.listener->wait_for(1);
    CHECK(f.image->disable() == ir::Return::Success);
    const auto frame = f.listener->at(0); check_pattern(frame);
    CHECK(frame.header.getWidth() == 48 && frame.header.getHeight() == 32);
    CHECK(frame.header.getColumnOffset() == 4 && frame.header.getRowOffset() == 6);
    CHECK(frame.header.getIntegrationTime() == 2ms && frame.header.getBitsPerPixel() == 8);
    CHECK(frame.header.getSensorNavState().empty() && frame.header.getSensorInertialState().empty());
    CHECK(frame.buffer->release() == ir::Return::Success);
    CHECK(calls == 1);
    const auto old = f.image;
    CHECK(f.control->detachChannel(old) == ir::Return::Success);
    CHECK(old->registerMetadataCallback(BadPixelCallback{}) == ir::Return::Fail);
    CHECK(old->registerMetadataCallback(BadPixelCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
    f.image = std::dynamic_pointer_cast<ir::ImageChannel>(f.control->attachChannel(f.config(ir::ChannelType::IRSTImage)));
    CHECK(f.image && f.image != old);
    CHECK(f.image->registerMetadataCallback(BadPixelCallback{[&](auto&, auto* list) {
        ++calls; valid = list && list->getSize() == 0 && list->getBadPixelCount() == 0 && list->getBadPixelList().empty();
    }}) == ir::Return::Success);
    CHECK(calls == 2 && valid);
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
}

void bad_pixels_reentry_and_throw() {
    Fixture f, other;
    std::atomic<bool> image_reentry_rejected{false};
    f.listener->hook = [&] {
        image_reentry_rejected = other.image->registerMetadataCallback(BadPixelCallback{[](auto&, auto*) {}}) == ir::Return::Fail;
    };
    TestCleanup stop_image{[&] { (void)f.image->disable(); }};
    f.add_buffer(); f.listener->auto_release = true;
    CHECK(f.image->enable() == ir::Return::Success); f.listener->wait_for(1);
    CHECK(image_reentry_rejected);
    const auto first = f.listener->count();
    const auto failures = gige_mel::provider_statistics(f.manager).callback_failures;
    bool rejected = false;
    CHECK(f.image->registerMetadataCallback(BadPixelCallback{[&](auto&, auto*) {
        rejected = f.image->registerMetadataCallback(BadPixelCallback{}) == ir::Return::Fail &&
            other.image->registerMetadataCallback(BadPixelCallback{[](auto&, auto*) {}}) == ir::Return::Fail &&
            f.image->disable() == ir::Return::Fail && f.image->enable() == ir::Return::Fail &&
            other.control->detachChannel(other.image) == ir::Return::Fail && f.control->init() == ir::Return::Fail;
        auto camera = f.c2->send(camera_command()).get();
        auto navigation = f.image->send(navigation_test_data(1ns)).get();
        rejected = rejected && !camera && camera.getError().getCode() == mel::ErrorCode::InvalidState &&
            !navigation && navigation.getError().getCode() == mel::ErrorCode::InvalidState;
        throw std::runtime_error("deliberate BadPixelList consumer exception");
    }}) == ir::Return::Success);
    CHECK(rejected && gige_mel::provider_statistics(f.manager).callback_failures == failures + 1);
    CHECK(f.image->registerMetadataCallback(BadPixelCallback{[](auto&, auto*) {}}) == ir::Return::Fail);
    f.listener->wait_for(first + 2); // Throwing metadata must not stop the image worker.
    CHECK(f.image->disable() == ir::Return::Success);
    CHECK(response(f.image->send(navigation_test_data(1ns)))->getSystemTime() == 1ns);
    bool c2_reentry_rejected = false;
    CHECK(f.c2->registerMetadataCallback(std::function<void(ir::Channel&, const ir::CommandStatus*)>{[&](auto&, auto*) {
        c2_reentry_rejected = other.image->registerMetadataCallback(BadPixelCallback{[](auto&, auto*) {}}) == ir::Return::Fail;
    }}) == ir::Return::Success);
    CHECK(response(f.c2->send(camera_command()))->getCommandSuccess());
    CHECK(c2_reentry_rejected);
    CHECK(other.image->registerMetadataCallback(BadPixelCallback{[](auto&, auto*) {}}) == ir::Return::Success);
}

void bad_pixels_detach_and_lifetime() {
    Fixture f;
    WatchdogGate gate;
    std::atomic<unsigned> calls{0};
    auto registration = std::async(std::launch::async, [&] {
        return f.image->registerMetadataCallback(BadPixelCallback{[&](auto&, auto*) { ++calls; gate.enter_and_wait(); }});
    });
    TestCleanup cleanup{[&] { gate.open(); }};
    gate.wait_for_entry();
    // Independent operations complete while the registering thread is in the callback.
    auto configure = std::async(std::launch::async, [&] { return f.c2->send(camera_command()).get(); });
    TestCleanup configure_cleanup{[&] { gate.open(); }};
    CHECK(configure.wait_for(750ms) == std::future_status::ready && configure.get());
    auto detach = std::async(std::launch::async, [&] { return f.control->detachChannel(f.image); });
    TestCleanup detach_cleanup{[&] { gate.open(); }};
    eventually([&] { return !f.image->sendKeepAliveRep().get(); });
    CHECK(detach.wait_for(0ns) == std::future_status::timeout);
    auto late = std::async(std::launch::async, [&] {
        return f.image->registerMetadataCallback(BadPixelCallback{[](auto&, auto*) {}});
    });
    TestCleanup late_cleanup{[&] { gate.open(); }};
    gate.open();
    CHECK(registration.get() == ir::Return::Success && detach.get() == ir::Return::Success);
    CHECK(late.get() == ir::Return::Fail);
    gate.check(); CHECK(calls == 1);
    CHECK(f.image->registerMetadataCallback(BadPixelCallback{[&](auto&, auto*) { ++calls; }}) == ir::Return::Fail);
    CHECK(calls == 1);

    Fixture pinned;
    WatchdogGate lifetime;
    std::weak_ptr<ir::Control> weak = pinned.control;
    auto pending = std::async(std::launch::async, [&] {
        return pinned.image->registerMetadataCallback(BadPixelCallback{[&](auto&, auto*) { lifetime.enter_and_wait(); }});
    });
    TestCleanup lifetime_cleanup{[&] { lifetime.open(); }};
    lifetime.wait_for_entry(); pinned.control.reset(); CHECK(!weak.expired());
    lifetime.open(); CHECK(pending.get() == ir::Return::Success); lifetime.check();
    CHECK(weak.expired());
    CHECK(pinned.image->registerMetadataCallback(BadPixelCallback{[](auto&, auto*) {}}) == ir::Return::Fail);

    // Both legal serializations of a truly simultaneous registration/detach.
    for (unsigned i = 0; i < 24; ++i) {
        Fixture racing;
        std::promise<void> start; auto ready = start.get_future().share();
        std::atomic<unsigned> count{0};
        auto reg = std::async(std::launch::async, [&] {
            ready.wait(); return racing.image->registerMetadataCallback(BadPixelCallback{[&](auto&, auto*) { ++count; }});
        });
        auto detached = std::async(std::launch::async, [&] { ready.wait(); return racing.control->detachChannel(racing.image); });
        start.set_value();
        const auto result = reg.get(); CHECK(detached.get() == ir::Return::Success);
        CHECK((result == ir::Return::Success && count == 1) || (result == ir::Return::Fail && count == 0));
        CHECK(racing.image->registerMetadataCallback(BadPixelCallback{[&](auto&, auto*) { ++count; }}) == ir::Return::Fail);
        CHECK(count == (result == ir::Return::Success ? 1U : 0U));
    }
}

void bad_pixels_noninterference() {
    HealthReports before, after;
    BitReports bit_reports;
    mel::BIT_Status completed;
    auto counted = std::make_shared<CountingBackend>();
    Fixture f(true, counted);
    CHECK(*response(f.c2->send(ir::BIT_Command{9, {1}, {}, {}})) == ir::Return::Success);
    CHECK(f.c2->registerMetadataCallback(BitCallback{[&](auto&, auto* r) { completed = *r; }}) == ir::Return::Success);
    auto health = attach_health(f); before.register_both(health);
    CHECK(health->enable() == ir::Return::Success); before.wait([&] { return !before.details.empty(); });
    CHECK(health->disable() == ir::Return::Success); CHECK(f.control->detachChannel(health) == ir::Return::Success);
    const auto calls = counted->calls->load();
    const auto stats = gige_mel::provider_statistics(f.manager);
    unsigned notifications = 0;
    CHECK(f.image->registerMetadataCallback(BadPixelCallback{[&](auto&, auto*) { ++notifications; }}) == ir::Return::Success);
    CHECK(notifications == 1 && counted->calls->load() == calls);
    CHECK(gige_mel::provider_statistics(f.manager).last_error == stats.last_error);
    ir::ModeCmd standby;
    standby.setState(mel::MFA_State::Standby); standby.setMode(ir::MFA_Mode::Unused);
    CHECK(*response(f.c2->send(standby)) == ir::MFA_Mode::Unused);
    health = attach_health(f); after.register_both(health);
    CHECK(health->registerMetadataCallback(bit_reports.callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success); after.wait([&] { return !after.details.empty(); });
    bit_reports.wait(1);
    CHECK(health->disable() == ir::Return::Success); CHECK(f.control->detachChannel(health) == ir::Return::Success);
    CHECK(HealthReports::values(before.details.back()) == HealthReports::values(after.details.back()));
    CHECK(before.statuses.back().getMFAState() == after.statuses.back().getMFAState());
    CHECK(completed.getCompletedBITs().front().getTimeTag() == bit_reports.last().getCompletedBITs().front().getTimeTag());
    CHECK(completed.getCompletedBITs().front().getResult() == bit_reports.last().getCompletedBITs().front().getResult());
    CHECK(bit_reports.last().getFaults().empty() && counted->calls->load() == calls && notifications == 1);

    Fixture retained;
    retained.add_buffer();
    CHECK(retained.image->enable() == ir::Return::Success); retained.listener->wait_for(1);
    const auto frame = retained.listener->at(0); const auto bytes = copy_image(frame);
    unsigned retained_calls = 0;
    bool valid = false;
    CHECK(retained.image->registerMetadataCallback(BadPixelCallback{[&](auto& channel, auto* list) {
        ++retained_calls;
        valid = &channel == retained.image.get() && list && list->getSize() == 0 &&
            list->getBadPixelCount() == 0 && list->getBadPixelList().empty();
    }}) == ir::Return::Success);
    // Full registered pool: publishing metadata must not release/overwrite the frame.
    CHECK(retained_calls == 1 && valid && copy_image(frame) == bytes); check_pattern(frame);
    CHECK(frame.header.getSystemTime() > 0ns && frame.header.getIntegrationTime() == 1ms);
    CHECK(frame.header.getWidth() == 64 && frame.header.getHeight() == 48);
    CHECK(frame.header.getSensorNavState().empty() && frame.header.getSensorInertialState().empty());
    CHECK(retained.image->disable() == ir::Return::Success);
    CHECK(copy_image(frame) == bytes && retained_calls == 1);
    CHECK(frame.buffer->release() == ir::Return::Success);
}
