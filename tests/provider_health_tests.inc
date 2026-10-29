// Included in provider_tests.cpp's anonymous namespace to share the synthetic
// decorators and observable callback gates with the existing contract suite.
using HealthStatusCallback = std::function<void(ir::Channel&, const mel::MFA_Status*)>;
using HealthDetailedCallback = std::function<void(ir::Channel&, const mel::MFA_StatusDetailed*)>;

std::shared_ptr<ir::HealthStatusChannel> attach_health(Fixture& f) {
    auto config = f.config(ir::ChannelType::HealthAndStatus);
    config.setImgLstnr({});
    auto health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(f.control->attachChannel(config));
    CHECK(health);
    return health;
}

struct HealthReports {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<mel::MFA_Status> statuses;
    std::vector<mel::MFA_StatusDetailed> details;
    std::vector<char> order;
    std::vector<std::thread::id> threads;

    HealthStatusCallback status_callback() {
        return [this](auto&, auto* status) {
            { std::lock_guard lock(mutex);
              statuses.push_back(*status); order.push_back('S'); threads.push_back(std::this_thread::get_id()); }
            changed.notify_all();
        };
    }
    HealthDetailedCallback detailed_callback() {
        return [this](auto&, auto* detail) {
            { std::lock_guard lock(mutex); details.push_back(*detail); order.push_back('D'); }
            changed.notify_all();
        };
    }
    template <class Predicate> void wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 1500ms, predicate));
    }
    void quiet() {
        std::unique_lock lock(mutex);
        const auto count = order.size();
        CHECK(!changed.wait_for(lock, 150ms, [&] { return order.size() != count; }));
    }
    void state(mel::MFA_State value) {
        wait([&] { return !statuses.empty() && statuses.back().getMFAState() == value; });
    }
    static std::map<std::string, std::string> values(const mel::MFA_StatusDetailed& report) {
        std::map<std::string, std::string> result;
        for (const auto& pair : report.getStatus()) CHECK(result.emplace(pair.getName(), pair.getValue()).second);
        CHECK(result.size() == 12);
        return result;
    }
    void detail(const std::string& key, const std::string& value) {
        wait([&] { return !details.empty() && values(details.back()).at("gige_mel." + key) == value; });
    }
    void register_both(const std::shared_ptr<ir::HealthStatusChannel>& health) {
        CHECK(health->registerMetadataCallback(status_callback()) == ir::Return::Success);
        CHECK(health->registerMetadataCallback(detailed_callback()) == ir::Return::Success);
    }
};

void health_surface() {
    Fixture f;
    const auto before = f.control->getCapabilities();
    CHECK(f.control->init() == ir::Return::Success);
    const auto& caps = f.control->getCapabilities();
    CHECK(caps.size() == 3);
    for (std::size_t i = 0; i < caps.size(); ++i) {
        CHECK(caps[i].getChanID().getUUID() == before[i].getChanID().getUUID());
        for (std::size_t j = 0; j < i; ++j) CHECK(caps[i].getChanID().getUUID() != caps[j].getChanID().getUUID());
    }
    HealthReports reports;
    auto health = attach_health(f);
    auto check_cap = [](const auto& cap) {
        CHECK(cap.getChannelMetadataCapabilities() == (std::set<ir::ChannelMetadataCapabilityType>{
            ir::ChannelMetadataCapabilityType::ChannelCommsTestRep, ir::ChannelMetadataCapabilityType::MFAStatus,
            ir::ChannelMetadataCapabilityType::MFAStatusDetailed}));
        CHECK(cap.getWidth() == 0 && cap.getHeight() == 0 && cap.getRowPitch() == 0 && cap.getImageSize() == 0 &&
              cap.getBufferSize() == 0 && cap.getBitDepth() == 0 && cap.getNumberOfBands() == 0);
    };
    check_cap(health->getCapabilities());
    check_cap(caps.back());
    CHECK(!f.control->attachChannel(f.config(ir::ChannelType::HealthAndStatus)));
    auto invalid = f.config(ir::ChannelType::HealthAndStatus);
    invalid.setChanID(mel::UCI_ID{});
    CHECK(!f.control->attachChannel(invalid));
    invalid = f.config(ir::ChannelType::IRSTImage);
    invalid.setChannelType(ir::ChannelType::HealthAndStatus);
    CHECK(!f.control->attachChannel(invalid));
    CHECK(health->registerMetadataCallback(HealthStatusCallback{}) == ir::Return::BadPointer);
    CHECK(health->registerMetadataCallback(HealthDetailedCallback{}) == ir::Return::BadPointer);
    reports.register_both(health);
    CHECK(health->registerMetadataCallback(reports.status_callback()) == ir::Return::Fail);
    CHECK(health->registerMetadataCallback(reports.detailed_callback()) == ir::Return::Fail);
#define CHECK_HEALTH_UNSUPPORTED(T) \
    CHECK(health->registerMetadataCallback(std::function<void(ir::Channel&, const T*)>{[](auto&, auto*) {}}) == ir::Return::NotSupported)
    CHECK_HEALTH_UNSUPPORTED(ir::LFStatus);
    CHECK_HEALTH_UNSUPPORTED(mel::BIT_Status);
    CHECK_HEALTH_UNSUPPORTED(ir::SubsystemStatusResp);
    CHECK_HEALTH_UNSUPPORTED(mel::DiscreteStatus);
    CHECK_HEALTH_UNSUPPORTED(mel::MFA_SecurityAuditRecord);
    CHECK_HEALTH_UNSUPPORTED(ir::NUC_TempData);
#undef CHECK_HEALTH_UNSUPPORTED
    auto buffer = f.add_buffer(3072, false);
    CHECK(health->registerBuffer(buffer) == ir::Return::NotSupported);
    CHECK(health->unregisterBuffer(buffer) == ir::Return::NotSupported);
    CHECK(buffer->init(f.storage.back().data(), 3072, 77) == ir::Return::Success);
    CHECK(buffer->getContext() == 77);
    std::weak_ptr<ir::Buffer> weak = buffer;
    f.buffers.clear(); buffer.reset();
    CHECK(weak.expired());
    unsigned replies = 0;
    CHECK(health->registerMetadataCallback(CommsCallback{[&](auto&, auto* reply) {
        if (reply->getCommandID() == 401 && reply->getRequestID() == 402) ++replies;
    }}) == ir::Return::Success);
    CHECK(*response(health->sendKeepAliveRep()) == ir::Return::Success);
    CHECK(response(health->send(comms_request(401, 402)))->getRequestID() == 402);
    CHECK(replies == 1);
    CHECK(health->enable() == ir::Return::Success);
    CHECK(health->registerBuffer({}) == ir::Return::NotSupported);
    CHECK(health->unregisterBuffer({}) == ir::Return::NotSupported);
    CHECK(health->disable() == ir::Return::Success);
    CHECK(response(health->send(comms_request(401, 402)))->getCommandID() == 401);
    CHECK(replies == 2);
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(HealthStatusCallback{}) == ir::Return::Fail);
    CHECK(health->registerBuffer({}) == ir::Return::Fail);
    CHECK(health->unregisterBuffer({}) == ir::Return::Fail);
    CHECK(health->enable() == ir::Return::Fail && health->disable() == ir::Return::Fail);
    health = attach_health(f);
    reports.register_both(health); // Fresh registrations.
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
}

void health_lifecycle() {
    for (int first = 0; first < 3; ++first) {
        Fixture f;
        HealthReports reports;
        auto health = attach_health(f);
        if (first != 1) CHECK(health->registerMetadataCallback(reports.status_callback()) == ir::Return::Success);
        if (first != 0) CHECK(health->registerMetadataCallback(reports.detailed_callback()) == ir::Return::Success);
        reports.quiet();
        CHECK(health->enable() == ir::Return::Success);
        reports.wait([&] { return !reports.order.empty(); });
        if (first == 0) CHECK(health->registerMetadataCallback(reports.detailed_callback()) == ir::Return::Success);
        if (first == 1) CHECK(health->registerMetadataCallback(reports.status_callback()) == ir::Return::Success);
        reports.wait([&] { return reports.statuses.size() >= 3 && reports.details.size() >= 3; });
        for (int i = 0; i < 10; ++i) CHECK(health->enable() == ir::Return::Success);
        CHECK(health->disable() == ir::Return::Success);
        reports.quiet();
        const auto count = reports.statuses.size();
        for (auto thread : reports.threads) CHECK(thread == reports.threads.front() && thread != std::this_thread::get_id());
        if (first == 2) {
            CHECK(reports.order.size() % 2 == 0);
            for (std::size_t i = 0; i < reports.order.size(); i += 2) CHECK(reports.order[i] == 'S' && reports.order[i + 1] == 'D');
        }
        for (const auto& status : reports.statuses) {
            CHECK(status.getMFAState() == mel::MFA_State::Standby);
            CHECK(status.getStateTransitionStatus() == mel::StateTransitionStatus::NotTransitioning);
            CHECK(!status.getMFAStateDescription().empty() && !status.getMFAModeDescription().empty());
            CHECK(status.getMFAComponents().empty());
            CHECK(status.getAbout().getSerialNumber().empty() && status.getAbout().getHardwareVersion().empty() &&
                  status.getAbout().getBootloaderSoftwareVersion().empty());
            CHECK(status.getAbout().getSoftwareVersion() == "0.1.0");
        }
        f.add_buffer(); f.listener->auto_release = true;
        CHECK(f.image->enable() == ir::Return::Success);
        CHECK(health->enable() == ir::Return::Success);
        reports.wait([&] { return reports.statuses.size() > count && reports.statuses.back().getMFAState() == mel::MFA_State::Operate; });
        CHECK(health->disable() == ir::Return::Success);
        const auto frames = f.listener->count();
        f.listener->wait_for(frames + 2); // Health disable did not stop Image.
        CHECK(f.control->detachChannel(health) == ir::Return::Success);
        f.listener->wait_for(f.listener->count() + 2);
    }
    // Change core state between callbacks. Detailed must still reflect the
    // captured Standby sample, not re-sample after Image starts.
    Fixture f;
    auto plan = std::make_shared<CallbackGate>();
    HealthReports reports;
    auto health = attach_health(f);
    CHECK(health->registerMetadataCallback(HealthStatusCallback{[&](auto&, auto*) { plan->enter_and_wait(); }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(reports.detailed_callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    plan->wait_for_entry();
    f.add_buffer();
    CHECK(f.image->enable() == ir::Return::Success);
    f.listener->wait_for(1);
    plan->open();
    reports.wait([&] { return !reports.details.empty(); });
    CHECK(health->disable() == ir::Return::Success);
    CHECK(HealthReports::values(reports.details.front()).at("gige_mel.acquisition_state") == "idle");
}

void health_fault_recovery() {
    auto plan = std::make_shared<ReceivePlan>();
    Fixture f(true, std::make_shared<TransformBackend>(Transform::none, plan));
    HealthReports reports;
    auto health = attach_health(f); reports.register_both(health);
    CHECK(health->enable() == ir::Return::Success); reports.state(mel::MFA_State::Standby);
    f.add_buffer();
    CHECK(f.image->enable() == ir::Return::Success);
    plan->wait_for(&ReceivePlan::entered); reports.state(mel::MFA_State::Operate);
    plan->open(); plan->wait_for_exit();
    reports.state(mel::MFA_State::Degraded); reports.detail("acquisition_state", "faulted");
    CHECK(f.image->enable() == ir::Return::Fail);
    CHECK(f.image->disable() == ir::Return::Success);
    reports.state(mel::MFA_State::Standby); reports.detail("acquisition_state", "idle");
    reports.detail("stream_failures", "1"); reports.detail("last_error_history", plan->terminal.message);
    f.listener->auto_release = true;
    CHECK(f.image->enable() == ir::Return::Success); f.listener->wait_for(1);
    reports.state(mel::MFA_State::Operate);
    CHECK(health->disable() == ir::Return::Success);

    for (auto transform : {Transform::clock_reset_after_one, Transform::none}) {
        Fixture failed(true, std::make_shared<TransformBackend>(transform));
        HealthReports observed;
        auto h = attach_health(failed); observed.register_both(h);
        failed.add_buffer();
        failed.listener->throw_after_record = transform == Transform::none;
        CHECK(h->enable() == ir::Return::Success);
        CHECK(failed.image->enable() == ir::Return::Success);
        observed.state(mel::MFA_State::Degraded);
        observed.detail("stream_failures", transform == Transform::none ? "0" : "1");
        if (transform == Transform::none) observed.detail("callback_failures", "1");
        CHECK(failed.image->disable() == ir::Return::Success);
        observed.state(mel::MFA_State::Standby);
        CHECK(h->disable() == ir::Return::Success);
    }
}

void health_configuration() {
    auto probe = std::make_shared<ConfigurationProbe>();
    Fixture f(true, std::make_shared<TransformBackend>(Transform::none, nullptr, probe));
    f.add_buffer(640 * 480); // Capacity cannot explain invalid configuration.
    HealthReports reports;
    auto health = attach_health(f); reports.register_both(health);
    CHECK(health->enable() == ir::Return::Success);
    reports.state(mel::MFA_State::Standby);
    for (bool invalid : {false, true}) {
        probe->fail_readback = !invalid; probe->invalid_readback = invalid;
        auto result = f.c2->send(config_command()).get();
        CHECK(!result || *result.get() == ir::Return::Fail);
        reports.detail("configuration_valid", "false"); reports.state(mel::MFA_State::Degraded);
        CHECK(f.image->enable() == ir::Return::Fail);
        probe->fail_readback = false; probe->invalid_readback = false;
        CHECK(*response(f.c2->send(config_command())) == ir::Return::Success);
        reports.detail("configuration_valid", "true"); reports.state(mel::MFA_State::Standby);
    }
    const auto rejected = camera_command(701, 641, 48);
    CHECK(!response(f.c2->send(rejected))->getCommandSuccess());
    reports.detail("configuration_valid", "true");
    CHECK(health->disable() == ir::Return::Success);
    CHECK(reports.statuses.back().getMFAState() == mel::MFA_State::Standby);

    Fixture partial(false, std::make_shared<TransformBackend>(Transform::partial_apply_failure));
    CHECK(partial.control->init() == ir::Return::Success); partial.attach();
    expect_error(partial.c2->send(camera_command()), mel::ErrorCode::InsufficientRemoteResources);
    HealthReports actual;
    auto h = attach_health(partial); actual.register_both(h);
    CHECK(h->enable() == ir::Return::Success); actual.state(mel::MFA_State::Standby);
    CHECK(partial.image->getCapabilities().getWidth() == 96);
    actual.detail("configuration_valid", "true");
    partial.add_buffer(640 * 480);
    CHECK(partial.image->enable() == ir::Return::Success); actual.state(mel::MFA_State::Operate);
    CHECK(h->disable() == ir::Return::Success);
}

void health_nonterminal() {
    for (auto transform : {Transform::incomplete, Transform::none}) {
        auto plan = std::make_shared<ReceivePlan>();
        plan->inject_terminal = false; plan->timeouts_before_frame = 3;
        Fixture f(true, std::make_shared<TransformBackend>(transform, plan));
        HealthReports reports;
        auto health = attach_health(f); reports.register_both(health);
        f.add_buffer(); CHECK(f.image->enable() == ir::Return::Success);
        if (transform == Transform::none) {
            f.listener->wait_for(1);
            eventually([&] { return gige_mel::provider_statistics(f.manager).dropped_no_buffer > 0; });
        } else eventually([&] { return gige_mel::provider_statistics(f.manager).rejected_frames > 0; });
        CHECK(health->enable() == ir::Return::Success);
        reports.state(mel::MFA_State::Operate); reports.detail("stream_failures", "0");
        const auto bytes = transform == Transform::none ? copy_image(f.listener->at(0)) : std::vector<std::byte>{};
        CHECK(health->disable() == ir::Return::Success);
        CHECK(f.control->detachChannel(health) == ir::Return::Success);
        if (transform == Transform::none) CHECK(copy_image(f.listener->at(0)) == bytes);
    }
}

// Count EVERY backend/device/stream operation using one atomic. Baseline reads
// are captured only while idle, so Health's zero-I/O property is unambiguous.
class CountingStream final : public backend::IFrameStream {
public:
    std::unique_ptr<backend::IFrameStream> inner;
    std::shared_ptr<std::atomic<unsigned>> calls;
    backend::Status start(std::uint64_t generation) override { ++*calls; return inner->start(generation); }
    backend::Result<backend::FrameLease> receive(std::chrono::milliseconds timeout) override { ++*calls; return inner->receive(timeout); }
    void request_stop() noexcept override { ++*calls; inner->request_stop(); }
    void stop() noexcept override { ++*calls; inner->stop(); }
    backend::StreamStatistics statistics() const override { ++*calls; return inner->statistics(); }
};
class CountingDevice final : public backend::ICameraDevice {
public:
    std::shared_ptr<backend::ICameraDevice> inner;
    std::shared_ptr<std::atomic<unsigned>> calls;
    bool fail_start{false};
    backend::CameraCapabilities capabilities() const override { ++*calls; return inner->capabilities(); }
    backend::Result<backend::CameraConfiguration> configuration() const override { ++*calls; return inner->configuration(); }
    backend::Result<backend::CameraConfiguration> apply(const backend::CameraConfiguration& c) override { ++*calls; return inner->apply(c); }
    backend::Status close() override { ++*calls; return inner->close(); }
    backend::Result<std::unique_ptr<backend::IFrameStream>> create_stream() override {
        ++*calls;
        if (fail_start) return backend::Error{backend::ErrorCode::disconnected, "failed start without owned acquisition"};
        auto created = inner->create_stream();
        if (!created) return created.error();
        auto stream = std::make_unique<CountingStream>();
        stream->inner = std::move(created).value(); stream->calls = calls;
        return std::unique_ptr<backend::IFrameStream>(std::move(stream));
    }
};
class CountingBackend final : public backend::ICameraBackend {
public:
    std::shared_ptr<std::atomic<unsigned>> calls{std::make_shared<std::atomic<unsigned>>(0)};
    std::shared_ptr<backend::ICameraBackend> inner{backend::make_synthetic_backend()};
    std::shared_ptr<CountingDevice> device;
    std::string_view id() const noexcept override { ++*calls; return "counted-synthetic"; }
    backend::Result<std::vector<backend::DeviceInfo>> enumerate() override { ++*calls; return inner->enumerate(); }
    backend::Result<std::shared_ptr<backend::ICameraDevice>> open(std::string_view id) override {
        ++*calls;
        auto result = inner->open(id);
        if (!result) return result.error();
        device = std::make_shared<CountingDevice>(); device->inner = result.value(); device->calls = calls;
        return std::shared_ptr<backend::ICameraDevice>(device);
    }
};

void health_no_io() {
    auto backend = std::make_shared<CountingBackend>();
    Fixture f(true, backend);
    HealthReports reports;
    auto health = attach_health(f); reports.register_both(health);
    auto calls = backend->calls->load();
    CHECK(health->enable() == ir::Return::Success);
    reports.wait([&] { return reports.details.size() >= 3; });
    CHECK(health->disable() == ir::Return::Success);
    CHECK(backend->calls->load() == calls);
    reports.detail("backend_id", "counted-synthetic"); reports.detail("device_id", "synthetic-0");
    f.add_buffer(); backend->device->fail_start = true;
    CHECK(f.image->enable() == ir::Return::Fail);
    calls = backend->calls->load();
    CHECK(health->enable() == ir::Return::Success);
    reports.detail("last_error_history", "failed start without owned acquisition");
    reports.detail("stream_failures", "0");
    CHECK(health->disable() == ir::Return::Success);
    CHECK(backend->calls->load() == calls);
    CHECK(reports.statuses.back().getMFAState() == mel::MFA_State::Standby);
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    CHECK(backend->calls->load() == calls);
}

void health_shutdown() {
    for (bool detach : {false, true}) {
        Fixture f;
        auto gate = std::make_shared<CallbackGate>();
        std::atomic<unsigned> statuses{0}, details{0};
        auto health = attach_health(f);
        CHECK(health->registerMetadataCallback(HealthStatusCallback{[&](auto&, auto*) { ++statuses; gate->enter_and_wait(); }}) == ir::Return::Success);
        CHECK(health->registerMetadataCallback(HealthDetailedCallback{[&](auto&, auto*) { ++details; }}) == ir::Return::Success);
        CHECK(health->enable() == ir::Return::Success); gate->wait_for_entry();
        std::promise<void> submitted;
        auto done = std::async(std::launch::async, [&] {
            submitted.set_value();
            return detach ? f.control->detachChannel(health) : health->disable();
        });
        submitted.get_future().wait();
        const bool waited = done.wait_for(30ms) == std::future_status::timeout;
        gate->open();
        CHECK(done.wait_for(750ms) == std::future_status::ready);
        CHECK(done.get() == ir::Return::Success && waited);
        CHECK(statuses == 1 && details == 0); // Unadmitted Detailed suppressed.
        CHECK(health->registerMetadataCallback(HealthStatusCallback{}) == (detach ? ir::Return::Fail : ir::Return::BadPointer));
    }
    // Periodic stop alone is insufficient: inherited synchronous metadata must
    // also drain when Control detaches Health.
    Fixture f;
    auto health = attach_health(f);
    auto gate = std::make_shared<CallbackGate>();
    CHECK(health->registerMetadataCallback(CommsCallback{[&](auto&, auto*) { gate->enter_and_wait(); }}) == ir::Return::Success);
    auto sent = std::async(std::launch::async, [&] { return response(health->send(comms_request(81, 82))); });
    gate->wait_for_entry();
    auto detached = std::async(std::launch::async, [&] { return f.control->detachChannel(health); });
    // Public registration rejection observes closed admission, deterministically
    // establishing detach has begun before the callback gate is opened.
    eventually([&] { return health->registerMetadataCallback(HealthStatusCallback{}) == ir::Return::Fail; });
    CHECK(detached.wait_for(0ms) == std::future_status::timeout);
    gate->open();
    CHECK(sent.wait_for(750ms) == std::future_status::ready && sent.get()->getRequestID() == 82);
    CHECK(detached.wait_for(750ms) == std::future_status::ready && detached.get() == ir::Return::Success);
}

void health_reentry() {
    auto a_probe = std::make_shared<ConfigurationProbe>(), b_probe = std::make_shared<ConfigurationProbe>();
    Fixture a(true, std::make_shared<TransformBackend>(Transform::none, nullptr, a_probe));
    Fixture b(true, std::make_shared<TransformBackend>(Transform::none, nullptr, b_probe));
    const auto a_applies = a_probe->applies, b_applies = b_probe->applies;
    auto a_buffer = a.add_buffer(), b_buffer = b.add_buffer();
    a.listener->auto_release = true;
    CHECK(a.image->enable() == ir::Return::Success); a.listener->wait_for(1);
    std::atomic<unsigned> completed{0}, c2_statuses{0};
    HealthReports reports;
    auto health = attach_health(a), other = attach_health(b);
    CHECK(a.c2->registerMetadataCallback(StatusCallback{[&](auto&, auto*) { ++c2_statuses; }}) == ir::Return::Success);
    CHECK(b.c2->registerMetadataCallback(StatusCallback{[&](auto&, auto*) { ++c2_statuses; }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(HealthStatusCallback{[&](auto& channel, auto*) {
        check_callback_policy(a, a_buffer); check_callback_policy(b, b_buffer);
        for (auto* target : {&a, &b}) expect_error(target->c2->send(config_command()), mel::ErrorCode::InvalidState);
        for (auto* h : {health.get(), other.get()}) {
            CHECK(h->enable() == ir::Return::Fail && h->disable() == ir::Return::Fail);
            CHECK(h->registerBuffer({}) == ir::Return::Fail && h->unregisterBuffer({}) == ir::Return::Fail);
            CHECK(h->registerMetadataCallback(HealthDetailedCallback{}) == ir::Return::Fail);
            CHECK(h->getCapabilities().getWidth() == 0);
            CHECK(*response(h->sendKeepAliveRep()) == ir::Return::Success);
        }
        CHECK(response(channel.send(comms_request(12, 13)))->getRequestID() == 13);
        ++completed;
        throw std::runtime_error("deliberate Health consumer exception");
    }}) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(reports.detailed_callback()) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    reports.wait([&] { return reports.details.size() >= 3; });
    CHECK(health->disable() == ir::Return::Success);
    CHECK(completed >= 3 && c2_statuses == 0);
    CHECK(gige_mel::provider_statistics(a.manager).callback_failures == completed);
    CHECK(gige_mel::provider_statistics(a.manager).stream_failures == 0);
    CHECK(a_probe->applies == a_applies && b_probe->applies == b_applies);
    CHECK(HealthReports::values(reports.details.back()).at("gige_mel.acquisition_state") == "running");
    a.listener->wait_for(a.listener->count() + 2);
    CHECK(*response(b.c2->send(config_command())) == ir::Return::Success);
    CHECK(c2_statuses == 1);
    // Explicit buffer release remains callback-safe on Health as on Image.
    CHECK(b.image->enable() == ir::Return::Success); b.listener->wait_for(1);
    std::promise<ir::Return> released;
    CHECK(other->registerMetadataCallback(HealthStatusCallback{[&](auto&, auto*) {
        released.set_value(b_buffer->release());
    }}) == ir::Return::Success);
    CHECK(other->enable() == ir::Return::Success);
    auto release_result = released.get_future();
    CHECK(release_result.wait_for(750ms) == std::future_status::ready && release_result.get() == ir::Return::Success);
    CHECK(other->disable() == ir::Return::Success);
    b.listener->wait_for(2);
}

void health_owner_lifetime() {
    // Keep caller memory/listener alive; destroy the final external graph from
    // Health while both workers exist. Publication pins protect callback code.
    Fixture f;
    f.add_buffer(); f.listener->auto_release = true;
    CHECK(f.image->enable() == ir::Return::Success); f.listener->wait_for(1);
    auto health = attach_health(f);
    std::weak_ptr<ir::Control> weak_control = f.control;
    std::weak_ptr<ir::HealthStatusChannel> weak_health = health;
    auto gate = std::make_shared<CallbackGate>();
    std::promise<bool> completed;
    CHECK(health->registerMetadataCallback(HealthStatusCallback{[&](auto& channel, auto*) {
        gate->enter_and_wait();
        f.control.reset(); health.reset();
        completed.set_value(!weak_control.expired() && !weak_health.expired() &&
            *response(channel.sendKeepAliveRep()) == ir::Return::Success);
    }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success); gate->wait_for_entry(); gate->open();
    auto result = completed.get_future();
    CHECK(result.wait_for(750ms) == std::future_status::ready && result.get());
    eventually([&] { return weak_control.expired() && weak_health.expired(); });
    eventually([&] { return f.image->enable() == ir::Return::Fail; });
    f.listener->require_quiet(150ms);
    expect_error(f.image->sendKeepAliveRep(), mel::ErrorCode::InvalidState);

    Fixture external;
    external.add_buffer(); external.listener->auto_release = true;
    HealthReports reports;
    auto h = attach_health(external); reports.register_both(h);
    CHECK(external.image->enable() == ir::Return::Success);
    CHECK(h->enable() == ir::Return::Success); reports.state(mel::MFA_State::Operate);
    external.control.reset();
    eventually([&] { return h->enable() == ir::Return::Fail; });
    reports.quiet(); external.listener->require_quiet();
}