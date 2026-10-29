#include <gige_mel/provider.hpp>

#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
namespace backend = gige_mel::backend;
using namespace std::chrono_literals;

namespace {

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression)) {                                                                         \
            throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) +           \
                                     ": " #expression);                                              \
        }                                                                                            \
    } while (false)

template <class T> std::shared_ptr<T> response(mel::RequestFor<T> future) {
    CHECK(future.wait_for(0ms) == std::future_status::ready);
    auto result = future.get();
    CHECK(result);
    CHECK(result.get());
    return result.get();
}

template <class T> void expect_error(mel::RequestFor<T> future, mel::ErrorCode code) {
    CHECK(future.wait_for(0ms) == std::future_status::ready);
    auto result = future.get();
    CHECK(!result);
    CHECK(result.getError().getCode() == code);
    CHECK(!result.getError().getDescription().empty());
}

template <class Predicate> void eventually(Predicate predicate) {
    std::mutex mutex;
    std::condition_variable tick;
    std::unique_lock lock(mutex);
    const auto deadline = std::chrono::steady_clock::now() + 750ms;
    while (!predicate()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        tick.wait_for(lock, 1ms);
    }
}

ir::CameraCommand camera_command(std::uint32_t id = 17, std::uint32_t width = 64,
                                 std::uint32_t height = 48) {
    ir::CameraCommand command;
    command.setCommandID(id);
    command.setWidth(width);
    command.setHeight(height);
    command.setFrameRate(120);
    command.setIntegrationTime(1ms);
    command.setImageType(ir::ImageType::Staring);
    command.setNumberOfStages(0);
    command.setColumnOffset(0);
    command.setRowOffset(0);
    return command;
}

struct Delivered {
    ir::FrameHeader header;
    std::shared_ptr<ir::Buffer> buffer;
};

struct CallbackGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false};
    bool leave{false};

    void enter_and_wait() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        (void)changed.wait_for(lock, 1s, [&] { return leave; });
    }
    void wait_for_entry() {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 750ms, [&] { return entered; }));
    }
    void open() {
        { std::lock_guard lock(mutex); leave = true; }
        changed.notify_all();
    }
};

class Listener final : public ir::ImageListener {
public:
    // Set only before enable or after disable has returned.
    bool auto_release{false};
    bool throw_after_record{false};
    std::function<void()> hook;

    void onImage(const ir::Channel&, const ir::FrameHeader& header,
                 std::shared_ptr<ir::Buffer> buffer) override {
        {
            std::lock_guard lock(mutex_);
            records_.push_back({header, buffer});
        }
        if (hook) hook();
        if (auto_release && buffer->release() != ir::Return::Success) {
            throw std::runtime_error("Listener could not release its delivered buffer");
        }
        {
            std::lock_guard lock(mutex_);
            ++completed_;
        }
        changed_.notify_all();
        if (throw_after_record) throw std::runtime_error("Deliberate consumer exception after retention");
    }

    void wait_for(std::size_t count) {
        std::unique_lock lock(mutex_);
        CHECK(changed_.wait_for(lock, 750ms, [&] { return completed_ >= count; }));
    }
    std::size_t count() const {
        std::lock_guard lock(mutex_);
        return completed_;
    }
    Delivered at(std::size_t index) const {
        std::lock_guard lock(mutex_);
        return records_.at(index);
    }
    void require_quiet(std::chrono::milliseconds interval = 30ms) {
        std::unique_lock lock(mutex_);
        const auto before = completed_;
        CHECK(!changed_.wait_for(lock, interval, [&] { return completed_ != before; }));
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Delivered> records_;
    std::size_t completed_{0};
};

struct Fixture {
    // Storage is declared first and destroyed last, after worker quiescence and
    // every buffer reference. No test accidentally relies on provider ownership
    // of memory supplied through Buffer::init.
    std::vector<std::vector<std::byte>> storage;
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::shared_ptr<ir::ImageChannel> image;
    std::shared_ptr<ir::C2Channel> c2;
    std::vector<std::shared_ptr<ir::Buffer>> buffers;
    std::shared_ptr<Listener> listener{std::make_shared<Listener>()};

    explicit Fixture(bool initialize = true,
                     std::shared_ptr<backend::ICameraBackend> adapter = {}) {
        manager = adapter ? gige_mel::make_api_manager({}, std::move(adapter))
                          : getAPI_Manager("synthetic");
        CHECK(manager);
        control = ir::getControl("synthetic", manager);
        CHECK(control);
        if (initialize) {
            CHECK(control->init() == ir::Return::Success);
            attach();
            CHECK(response(c2->send(camera_command()))->getCommandSuccess());
        }
    }

    ~Fixture() {
        if (image) {
            try { (void)image->disable(); } catch (...) {}
        }
        for (const auto& buffer : buffers) {
            try { (void)buffer->release(); } catch (...) {}
        }
        if (control) {
            try { if (image) (void)control->detachChannel(image); } catch (...) {}
            try { if (c2) (void)control->detachChannel(c2); } catch (...) {}
        }
        image.reset();
        c2.reset();
        control.reset();
        buffers.clear();
        listener.reset();
        manager.reset();
    }

    ir::Config config(ir::ChannelType type) const {
        const auto& caps = control->getCapabilities();
        const auto found = std::find_if(caps.begin(), caps.end(), [type](const auto& cap) {
            const auto& types = cap.getChannelTypes();
            return std::find(types.begin(), types.end(), type) != types.end();
        });
        CHECK(found != caps.end());
        ir::Config selected;
        selected.setChanID(found->getChanID());
        selected.setChannelType(type);
        selected.setImgLstnr(listener);
        return selected;
    }

    void attach() {
        image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(config(ir::ChannelType::IRSTImage)));
        c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(config(ir::ChannelType::CommandAndControl)));
        CHECK(image);
        CHECK(c2);
    }

    std::shared_ptr<ir::Buffer> add_buffer(std::size_t size = 3072, bool register_it = true) {
        storage.emplace_back(size);
        auto buffer = ir::getBuffer("synthetic", manager);
        CHECK(buffer);
        CHECK(buffer->init(storage.back().data(), size, static_cast<std::int64_t>(buffers.size())) == ir::Return::Success);
        buffers.push_back(buffer);
        if (register_it) CHECK(image->registerBuffer(buffer) == ir::Return::Success);
        return buffer;
    }

    void release_buffers() {
        for (const auto& buffer : buffers) (void)buffer->release();
    }
};

std::vector<std::byte> copy_image(const Delivered& delivered) {
    const auto* first = static_cast<const std::byte*>(delivered.buffer->getImageAddress());
    return {first, first + static_cast<std::size_t>(delivered.header.getWidth()) * delivered.header.getHeight()};
}

void check_pattern(const Delivered& delivered) {
    const auto bytes = copy_image(delivered);
    for (std::uint32_t y = 0; y < delivered.header.getHeight(); ++y) {
        for (std::uint32_t x = 0; x < delivered.header.getWidth(); ++x) {
            CHECK(bytes[static_cast<std::size_t>(y) * delivered.header.getWidth() + x] ==
                  static_cast<std::byte>((static_cast<std::uint64_t>(x) + y + delivered.header.getFrameID()) & 0xffU));
        }
    }
}

void factory_initialization_and_channel_identity() {
    CHECK(!getAPI_Manager("unknown"));
    CHECK(!ir::getControl("synthetic", {}));
    Fixture fixture(false);
    CHECK(ir::getControl("synthetic", fixture.manager) == fixture.control);
    CHECK(!ir::getControl("other", fixture.manager));
    CHECK(!ir::getBuffer("other", fixture.manager));
    CHECK(fixture.control->getCapabilities().empty());
    CHECK(!fixture.control->attachChannel(ir::Config{}));
    CHECK(fixture.control->init("unknown-profile") == ir::Return::NotSupported);
    CHECK(fixture.control->init("mono8-bootstrap") == ir::Return::Success);
    CHECK(fixture.control->init() == ir::Return::Success);
    CHECK(fixture.control->getCapabilities().size() == 2);
    auto selected = fixture.config(ir::ChannelType::IRSTImage);
    auto no_listener = selected;
    no_listener.setImgLstnr({});
    CHECK(!fixture.control->attachChannel(no_listener));
    auto unknown = selected;
    unknown.setChanID(mel::UCI_ID{});
    CHECK(!fixture.control->attachChannel(unknown));
    unknown = selected;
    unknown.setChannelType(ir::ChannelType::HealthAndStatus);
    CHECK(!fixture.control->attachChannel(unknown));
    fixture.attach();
    CHECK(!fixture.control->attachChannel(selected));
    CHECK(!fixture.control->attachChannel(fixture.config(ir::ChannelType::CommandAndControl)));
    CHECK(fixture.control->detachChannel({}) == ir::Return::BadPointer);
    const auto cap = fixture.image->getCapabilities();
    CHECK(cap.getWidth() == 64 && cap.getHeight() == 48);
    CHECK(cap.getBitDepth() == 8 && cap.getRowPitch() == 64);
    CHECK(cap.getBufferSize() == 3072 && cap.getImageSize() == 3072);
    CHECK(cap.getNumberOfBands() == 1 && cap.getFormat() == ir::PixelFormat::Mono);
    CHECK(*response(fixture.c2->sendKeepAliveRep()) == ir::Return::Success);
    CHECK(fixture.image->enable() == ir::Return::Fail);
    CHECK(fixture.c2->enable() == ir::Return::Success);
    CHECK(fixture.c2->disable() == ir::Return::Success);
}

void buffer_validation_affinity_and_mutation() {
    Fixture fixture;
    CHECK(fixture.image->registerBuffer({}) == ir::Return::BadPointer);
    auto empty = ir::getBuffer("synthetic", fixture.manager);
    CHECK(empty->init(nullptr, 3072, 0) == ir::Return::BadPointer);
    CHECK(fixture.image->registerBuffer(empty) == ir::Return::BadPointer);
    auto tiny = fixture.add_buffer(3071, false);
    CHECK(fixture.image->registerBuffer(tiny) == ir::Return::BadPointer);
    auto registered = fixture.add_buffer(6144);
    CHECK(registered->release() == ir::Return::Fail);
    CHECK(fixture.image->registerBuffer(registered) == ir::Return::Fail);
    CHECK(registered->init(fixture.storage.back().data(), 6144, 99) == ir::Return::Fail);
    registered->setFlags({ir::BufferFlag::GPU});
    registered->addFlag(ir::BufferFlag::RDMA);
    std::vector<ir::BufferFlag> flags;
    CHECK(registered->getFlags(flags) == ir::Return::Success);
    CHECK(flags == std::vector<ir::BufferFlag>{ir::BufferFlag::Host});

    auto overlapping = ir::getBuffer("synthetic", fixture.manager);
    CHECK(overlapping->init(fixture.storage.back().data() + 1, 3072, 0) == ir::Return::Success);
    CHECK(fixture.image->registerBuffer(overlapping) == ir::Return::Fail);
    auto other_manager = getAPI_Manager("synthetic");
    auto foreign = ir::getBuffer("synthetic", other_manager);
    CHECK(foreign->init(fixture.storage.back().data(), 6144, 0) == ir::Return::Success);
    CHECK(fixture.image->registerBuffer(foreign) == ir::Return::BadPointer);

    auto gpu = fixture.add_buffer(3072, false);
    gpu->setFlags({ir::BufferFlag::GPU});
    CHECK(fixture.image->registerBuffer(gpu) == ir::Return::NotSupported);
    gpu->setFlags({ir::BufferFlag::Host, ir::BufferFlag::RDMA});
    CHECK(fixture.image->registerBuffer(gpu) == ir::Return::NotSupported);
    gpu->setFlags({ir::BufferFlag::Host});
    CHECK(fixture.image->registerBuffer(gpu) == ir::Return::Success);
    CHECK(fixture.c2->registerBuffer(empty) == ir::Return::NotSupported);
    CHECK(fixture.image->unregisterBuffer(empty) == ir::Return::BadPointer);
    CHECK(fixture.image->unregisterBuffer(registered) == ir::Return::Success);
    CHECK(registered->init(fixture.storage[1].data(), 6144, 99) == ir::Return::Success);
    CHECK(registered->getContext() == 99);
}

void retained_buffers_starvation_and_release_recovery() {
    Fixture fixture;
    fixture.add_buffer();
    fixture.add_buffer();
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(2);
    const auto first = fixture.listener->at(0);
    const auto second = fixture.listener->at(1);
    CHECK(first.buffer != second.buffer);
    const auto first_copy = copy_image(first);
    const auto second_copy = copy_image(second);
    eventually([&] { return gige_mel::provider_statistics(fixture.manager).dropped_no_buffer > 0; });
    CHECK(copy_image(first) == first_copy);
    CHECK(copy_image(second) == second_copy);
    CHECK(fixture.listener->count() == 2);
    CHECK(first.buffer->release() == ir::Return::Success);
    fixture.listener->wait_for(3);
    CHECK(fixture.listener->at(2).buffer == first.buffer);
    CHECK(copy_image(second) == second_copy);
    CHECK(fixture.image->disable() == ir::Return::Success);
    CHECK(fixture.image->unregisterBuffer(second.buffer) == ir::Return::Fail);
    CHECK(fixture.image->enable() == ir::Return::Fail);
    CHECK(second.buffer->release() == ir::Return::Success);
    CHECK(second.buffer->release() == ir::Return::Fail);
    CHECK(first.buffer->release() == ir::Return::Success);
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(4);
    CHECK(fixture.image->disable() == ir::Return::Success);
    fixture.listener->require_quiet();
}

void disable_waits_for_callback_and_quiesces() {
    Fixture fixture;
    fixture.add_buffer();
    const auto gate = std::make_shared<CallbackGate>();
    fixture.listener->auto_release = true;
    fixture.listener->hook = [gate] { gate->enter_and_wait(); };
    CHECK(fixture.image->enable() == ir::Return::Success);
    gate->wait_for_entry();
    auto disabled = std::async(std::launch::async, [&] { return fixture.image->disable(); });
    const bool waited_for_callback = disabled.wait_for(20ms) == std::future_status::timeout;
    gate->open();
    CHECK(disabled.wait_for(750ms) == std::future_status::ready);
    CHECK(disabled.get() == ir::Return::Success);
    CHECK(waited_for_callback);
    CHECK(fixture.listener->count() == 1);
    fixture.listener->require_quiet();
}

void detach_preserves_delivered_buffer() {
    Fixture fixture;
    auto buffer = fixture.add_buffer();
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(1);
    const auto held = fixture.listener->at(0);
    const auto snapshot = copy_image(held);
    CHECK(fixture.control->detachChannel(fixture.image) == ir::Return::Success);
    CHECK(fixture.control->detachChannel(fixture.image) == ir::Return::BadPointer);
    CHECK(fixture.image->enable() == ir::Return::Fail);
    expect_error(fixture.image->sendKeepAliveRep(), mel::ErrorCode::InvalidState);
    expect_error(fixture.image->send(camera_command()), mel::ErrorCode::InvalidState);
    CHECK(buffer->init(fixture.storage.front().data(), 3072, 5) == ir::Return::Fail);
    fixture.image.reset();
    fixture.c2.reset();
    fixture.control.reset();
    fixture.manager.reset();
    CHECK(copy_image(held) == snapshot);
    CHECK(buffer->release() == ir::Return::Success);
    CHECK(buffer->release() == ir::Return::Fail);
    CHECK(buffer->init(fixture.storage.front().data(), 3072, 5) == ir::Return::Success);
}

void camera_commands_validate_full_snapshots() {
    Fixture fixture;
    auto command = camera_command(101, 40, 24);
    command.setColumnOffset(8);
    command.setRowOffset(4);
    command.setIntegrationTime(2ms);
    const auto reply = response(fixture.c2->send(command));
    CHECK(reply->getCommandID() == 101 && reply->getCommandSuccess());
    CHECK(reply->getTimestamp().count() > 0);
    CHECK(fixture.image->getCapabilities().getWidth() == 40);
    CHECK(fixture.c2->getCapabilities().getHeight() == 24);

    auto invalid = command;
    invalid.setCommandID(102);
    invalid.setWidth(0);
    auto denied = response(fixture.image->send(invalid));
    CHECK(!denied->getCommandSuccess() && denied->getCommandID() == 102);
    invalid = command;
    invalid.setFrameRate(0);
    CHECK(!response(fixture.c2->send(invalid))->getCommandSuccess());
    invalid = command;
    invalid.setIntegrationTime(1s);
    CHECK(!response(fixture.image->send(invalid))->getCommandSuccess());
    invalid = command;
    invalid.setNumberOfStages(1);
    CHECK(!response(fixture.c2->send(invalid))->getCommandSuccess());
    CHECK(fixture.image->getCapabilities().getWidth() == 40);
    CHECK(fixture.image->getCapabilities().getHeight() == 24);
    fixture.add_buffer();
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(1);
    expect_error(fixture.c2->send(camera_command()), mel::ErrorCode::InvalidState);
    expect_error(fixture.image->send(camera_command()), mel::ErrorCode::InvalidState);
    CHECK(fixture.image->disable() == ir::Return::Success);
    const auto header = fixture.listener->at(0).header;
    CHECK(header.getWidth() == 40 && header.getHeight() == 24);
    CHECK(header.getColumnOffset() == 8 && header.getRowOffset() == 4);
    CHECK(header.getIntegrationTime() == 2ms);
}

void larger_configuration_requires_larger_buffers() {
    Fixture fixture;
    auto old = fixture.add_buffer();
    const auto discovery_width = fixture.control->getCapabilities().front().getWidth();
    CHECK(response(fixture.image->send(camera_command(222, 128, 96)))->getCommandSuccess());
    CHECK(fixture.image->getCapabilities().getBufferSize() == 128 * 96);
    CHECK(fixture.control->getCapabilities().front().getWidth() == discovery_width);
    CHECK(fixture.image->enable() == ir::Return::Fail);
    CHECK(fixture.image->unregisterBuffer(old) == ir::Return::Success);
    fixture.add_buffer(128 * 96);
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(1);
    CHECK(fixture.image->disable() == ir::Return::Success);
    CHECK(fixture.listener->at(0).header.getWidth() == 128);
    check_pattern(fixture.listener->at(0));
}

void metadata_callbacks_are_bounded_and_reentry_is_rejected() {
    Fixture fixture;
    using Comms = std::function<void(ir::Channel&, const ir::ChannelCommsTestRep*)>;
    using Camera = std::function<void(ir::Channel&, const ir::CameraCommandResp*)>;
    CHECK(fixture.image->registerMetadataCallback(Comms{}) == ir::Return::BadPointer);
    CHECK(fixture.image->registerMetadataCallback(Camera{}) == ir::Return::BadPointer);
    std::uint32_t comms_id = 0, request_id = 0, camera_id = 0;
    ir::Return reentrant_disable = ir::Return::Success;
    mel::ErrorCode reentrant_command = mel::ErrorCode::None;
    Comms comms = [&](ir::Channel& channel, const ir::ChannelCommsTestRep* reply) {
        comms_id = reply->getCommandID();
        request_id = reply->getRequestID();
        reentrant_disable = channel.disable();
        auto nested = fixture.c2->send(camera_command()).get();
        if (!nested) reentrant_command = nested.getError().getCode();
    };
    CHECK(fixture.image->registerMetadataCallback(comms) == ir::Return::Success);
    CHECK(fixture.image->registerMetadataCallback(comms) == ir::Return::Fail);
    ir::ChannelCommsTestReq request;
    request.setCommandID(51);
    request.setRequestID(93);
    const auto answer = response(fixture.image->send(request));
    CHECK(answer->getCommandID() == 51 && answer->getRequestID() == 93);
    CHECK(comms_id == 51 && request_id == 93);
    CHECK(reentrant_disable == ir::Return::Fail);
    CHECK(reentrant_command == mel::ErrorCode::InvalidState);
    Camera camera = [&](ir::Channel& channel, const ir::CameraCommandResp* reply) {
        camera_id = reply->getCommandID();
        reentrant_disable = channel.disable();
        auto nested = fixture.image->send(camera_command()).get();
        if (!nested) reentrant_command = nested.getError().getCode();
    };
    CHECK(fixture.image->registerMetadataCallback(camera) == ir::Return::Success);
    CHECK(fixture.image->registerMetadataCallback(camera) == ir::Return::Fail);
    CHECK(response(fixture.image->send(camera_command(919)))->getCommandSuccess());
    CHECK(camera_id == 919 && reentrant_disable == ir::Return::Fail);
    CHECK(reentrant_command == mel::ErrorCode::InvalidState);
    CHECK(response(fixture.c2->send(camera_command(920)))->getCommandSuccess());
    CHECK(camera_id == 920); // C2 response metadata uses the attached image channel.
}

void image_callback_mutations_fail_without_deadlock() {
    Fixture fixture;
    fixture.add_buffer();
    struct Results { std::atomic<int> disabled{-1}, command_error{-1}, initialized{-1}; };
    const auto results = std::make_shared<Results>();
    fixture.listener->hook = [&, results] {
        results->disabled = static_cast<int>(fixture.image->disable());
        results->initialized = static_cast<int>(fixture.control->init());
        auto nested = fixture.c2->send(camera_command()).get();
        if (!nested) results->command_error = static_cast<int>(nested.getError().getCode());
    };
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(1);
    CHECK(fixture.image->disable() == ir::Return::Success);
    CHECK(results->disabled == static_cast<int>(ir::Return::Fail));
    CHECK(results->initialized == static_cast<int>(ir::Return::Fail));
    CHECK(results->command_error == static_cast<int>(mel::ErrorCode::InvalidState));
}

void unsupported_surfaces_return_real_errors() {
    Fixture fixture;
    constexpr auto unsupported = mel::ErrorCode::Unsupported;
    expect_error(fixture.c2->send(ir::BIT_Command{}), unsupported);
    expect_error(fixture.c2->send(ir::ConfigSetCommand{}), unsupported);
    expect_error(fixture.c2->send(ir::ModeCmd{}), unsupported);
    expect_error(fixture.c2->send(ir::CalibrationConfigurationCmd{}), unsupported);
    expect_error(fixture.c2->send(ir::CalibrationStatusCmd{}), unsupported);
    expect_error(fixture.c2->send(ir::CameraProtectCmd{}), unsupported);
    expect_error(fixture.c2->send(ir::EraseCommand{}), unsupported);
    expect_error(fixture.c2->send(ir::SystemTrackDataResponse{}), unsupported);
    expect_error(fixture.image->send(mel::NavigationReport{}), unsupported);
    CHECK(fixture.image->registerMetadataCallback(
        std::function<void(ir::Channel&, const ir::BadPixelList*)>{[](auto&, auto*) {}}) == ir::Return::NotSupported);
    CHECK(fixture.c2->registerMetadataCallback(
        std::function<void(ir::Channel&, const mel::BIT_Status*)>{[](auto&, auto*) {}}) == ir::Return::NotSupported);
}

void callback_exception_preserves_explicit_lease() {
    Fixture fixture;
    auto buffer = fixture.add_buffer();
    fixture.listener->throw_after_record = true;
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(1);
    eventually([&] { return gige_mel::provider_statistics(fixture.manager).callback_failures == 1; });
    CHECK(fixture.image->disable() == ir::Return::Success);
    const auto stats = gige_mel::provider_statistics(fixture.manager);
    CHECK(stats.delivered_frames == 1 && !stats.last_error.empty());
    fixture.listener->require_quiet();
    CHECK(fixture.image->unregisterBuffer(buffer) == ir::Return::Fail);
    CHECK(fixture.image->enable() == ir::Return::Fail);
    CHECK(buffer->release() == ir::Return::Success);
    CHECK(buffer->release() == ir::Return::Fail);
    fixture.listener->throw_after_record = false;
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(2);
    CHECK(fixture.image->disable() == ir::Return::Success);
}

void destroying_control_detaches_retained_channels() {
    Fixture fixture;
    fixture.add_buffer();
    fixture.listener->auto_release = true;
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(2);
    fixture.control.reset();
    fixture.listener->require_quiet();
    CHECK(fixture.image->enable() == ir::Return::Fail);
    CHECK(fixture.c2->enable() == ir::Return::Fail);
    expect_error(fixture.image->sendKeepAliveRep(), mel::ErrorCode::InvalidState);
    CHECK(fixture.buffers.front()->release() == ir::Return::Fail);
}

void old_detached_handle_does_not_clear_replacement_state() {
    Fixture fixture;
    auto old_image = fixture.image;
    CHECK(fixture.control->detachChannel(old_image) == ir::Return::Success);
    fixture.image = std::dynamic_pointer_cast<ir::ImageChannel>(
        fixture.control->attachChannel(fixture.config(ir::ChannelType::IRSTImage)));
    CHECK(fixture.image);
    fixture.add_buffer();
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(1);
    old_image.reset();
    expect_error(fixture.c2->send(camera_command()), mel::ErrorCode::InvalidState);
    CHECK(fixture.image->disable() == ir::Return::Success);
}

void dropping_control_in_callback_during_disable_finishes() {
    Fixture fixture;
    fixture.add_buffer();
    const auto gate = std::make_shared<CallbackGate>();
    fixture.listener->auto_release = true;
    fixture.listener->hook = [&, gate] {
        gate->enter_and_wait();
        fixture.control.reset();
    };
    CHECK(fixture.image->enable() == ir::Return::Success);
    gate->wait_for_entry();
    auto disabled = std::async(std::launch::async, [&] { return fixture.image->disable(); });
    const bool waited = disabled.wait_for(20ms) == std::future_status::timeout;
    gate->open();
    CHECK(disabled.wait_for(750ms) == std::future_status::ready);
    CHECK(disabled.get() == ir::Return::Success);
    CHECK(waited);
    CHECK(!fixture.control);
    CHECK(fixture.image->enable() == ir::Return::Fail);
    fixture.listener->require_quiet();
}

void detach_drains_metadata_callback_and_closes_admission() {
    Fixture fixture;
    const auto gate = std::make_shared<CallbackGate>();
    const auto count = std::make_shared<std::atomic<unsigned>>(0);
    using Callback = std::function<void(ir::Channel&, const ir::ChannelCommsTestRep*)>;
    CHECK(fixture.image->registerMetadataCallback(Callback{[gate, count](auto&, auto*) {
        ++*count;
        gate->enter_and_wait();
    }}) == ir::Return::Success);
    auto sent = std::async(std::launch::async, [image = fixture.image] {
        ir::ChannelCommsTestReq request;
        request.setCommandID(334);
        return response(image->send(request));
    });
    gate->wait_for_entry();
    auto detached = std::async(std::launch::async, [control = fixture.control, image = fixture.image] {
        return control->detachChannel(image);
    });
    const bool waited = detached.wait_for(20ms) == std::future_status::timeout;
    gate->open();
    CHECK(sent.wait_for(750ms) == std::future_status::ready);
    CHECK(sent.get()->getCommandID() == 334);
    CHECK(detached.wait_for(750ms) == std::future_status::ready);
    CHECK(detached.get() == ir::Return::Success);
    CHECK(waited);
    expect_error(fixture.image->send(ir::ChannelCommsTestReq{}), mel::ErrorCode::InvalidState);
    CHECK(*count == 1);
}

enum class Transform {
    padding,
    incomplete,
    wrong_generation,
    wrong_clock,
    first_incomplete_bad_stream,
    clock_reset_after_one,
    clock_regression_after_one,
    partial_apply_failure,
    readback_failure_after_apply
};

class TransformStream final : public backend::IFrameStream {
public:
    TransformStream(std::unique_ptr<backend::IFrameStream> inner, Transform transform)
        : inner_(std::move(inner)), transform_(transform) {}
    backend::Status start(std::uint64_t generation) override { return inner_->start(generation); }
    void request_stop() noexcept override { inner_->request_stop(); }
    void stop() noexcept override { inner_->stop(); }
    backend::StreamStatistics statistics() const override { return inner_->statistics(); }
    backend::Result<backend::FrameLease> receive(std::chrono::milliseconds timeout) override {
        auto result = inner_->receive(timeout);
        if (!result) return result.error();
        auto lease = std::make_shared<backend::FrameLease>(std::move(result).value());
        auto descriptor = lease->descriptor();
        const auto frame_number = received_++;
        if (transform_ == Transform::padding) {
            descriptor.row_stride_bytes += 7;
            auto storage = std::make_shared<std::vector<std::byte>>(
                descriptor.row_stride_bytes * descriptor.height, std::byte{0xee});
            for (std::size_t row = 0; row < descriptor.height; ++row) {
                std::memcpy(storage->data() + row * descriptor.row_stride_bytes,
                            lease->bytes().data() + row * lease->descriptor().row_stride_bytes,
                            descriptor.width);
            }
            return backend::FrameLease(descriptor, std::span<const std::byte>{*storage}, storage);
        }
        if (transform_ == Transform::incomplete) descriptor.status = backend::FrameStatus::incomplete;
        if (transform_ == Transform::wrong_generation) ++descriptor.configuration_generation;
        if (transform_ == Transform::wrong_clock) descriptor.timestamp.domain = backend::ClockDomain::device;
        if (transform_ == Transform::first_incomplete_bad_stream && frame_number == 0) {
            descriptor.status = backend::FrameStatus::incomplete;
            descriptor.stream_generation += 1000;
        }
        if (transform_ == Transform::clock_reset_after_one && frame_number > 0) {
            ++descriptor.timestamp.clock_generation;
        }
        if (transform_ == Transform::clock_regression_after_one && frame_number > 0) {
            descriptor.timestamp.value = 0ns;
        }
        return backend::FrameLease(descriptor, lease->bytes(), lease);
    }

private:
    std::unique_ptr<backend::IFrameStream> inner_;
    Transform transform_;
    std::uint64_t received_{0};
};

class TransformDevice final : public backend::ICameraDevice {
public:
    TransformDevice(std::shared_ptr<backend::ICameraDevice> inner, Transform transform)
        : inner_(std::move(inner)), transform_(transform) {}
    backend::CameraCapabilities capabilities() const override { return inner_->capabilities(); }
    backend::Result<backend::CameraConfiguration> configuration() const override {
        if (readback_failed_.load()) {
            return backend::Error{backend::ErrorCode::io_error, "Injected configuration readback failure after apply"};
        }
        return inner_->configuration();
    }
    backend::Result<backend::CameraConfiguration> apply(const backend::CameraConfiguration& config) override {
        if (transform_ == Transform::partial_apply_failure) {
            auto partial = config;
            partial.width = 96;
            partial.height = 72;
            auto actual = inner_->apply(partial);
            if (!actual) return actual.error();
            return backend::Error{backend::ErrorCode::io_error, "Injected failure after changing ROI to 96x72"};
        }
        auto actual = inner_->apply(config);
        if (actual && transform_ == Transform::readback_failure_after_apply) {
            readback_failed_ = true;
        }
        return actual;
    }
    backend::Status close() override { return inner_->close(); }
    backend::Result<std::unique_ptr<backend::IFrameStream>> create_stream() override {
        auto created = inner_->create_stream();
        if (!created) return created.error();
        std::unique_ptr<backend::IFrameStream> stream =
            std::make_unique<TransformStream>(std::move(created).value(), transform_);
        return stream;
    }

private:
    std::shared_ptr<backend::ICameraDevice> inner_;
    Transform transform_;
    std::atomic<bool> readback_failed_{false};
};

class TransformBackend final : public backend::ICameraBackend {
public:
    explicit TransformBackend(Transform transform) : transform_(transform) {}
    std::string_view id() const noexcept override { return "decorated-synthetic"; }
    backend::Result<std::vector<backend::DeviceInfo>> enumerate() override { return inner_->enumerate(); }
    backend::Result<std::shared_ptr<backend::ICameraDevice>> open(std::string_view id) override {
        auto opened = inner_->open(id);
        if (!opened) return opened.error();
        std::shared_ptr<backend::ICameraDevice> device =
            std::make_shared<TransformDevice>(std::move(opened).value(), transform_);
        return device;
    }

private:
    std::shared_ptr<backend::ICameraBackend> inner_{backend::make_synthetic_backend()};
    Transform transform_;
};

void alternate_backend_padded_rows_are_repacked() {
    Fixture fixture(true, std::make_shared<TransformBackend>(Transform::padding));
    auto buffer = fixture.add_buffer();
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(1);
    CHECK(fixture.image->disable() == ir::Return::Success);
    const auto delivered = fixture.listener->at(0);
    CHECK(delivered.buffer == buffer);
    CHECK(delivered.buffer->getImageAddress() == fixture.storage.front().data());
    CHECK(fixture.image->getCapabilities().getRowPitch() == 64);
    check_pattern(delivered);
    CHECK(gige_mel::provider_statistics(fixture.manager).rejected_frames == 0);
}

void incomplete_and_mismatched_frames_are_never_published() {
    for (const auto transform : {Transform::incomplete, Transform::wrong_generation, Transform::wrong_clock}) {
        Fixture fixture(true, std::make_shared<TransformBackend>(transform));
        fixture.add_buffer();
        CHECK(fixture.image->enable() == ir::Return::Success);
        eventually([&] { return gige_mel::provider_statistics(fixture.manager).rejected_frames >= 2; });
        CHECK(fixture.image->disable() == ir::Return::Success);
        const auto stats = gige_mel::provider_statistics(fixture.manager);
        CHECK(stats.delivered_frames == 0 && stats.rejected_frames >= 2);
        CHECK(fixture.listener->count() == 0);
        CHECK(fixture.buffers.front()->release() == ir::Return::Fail);
    }
}

void rejected_first_frame_does_not_poison_generation() {
    Fixture fixture(true, std::make_shared<TransformBackend>(Transform::first_incomplete_bad_stream));
    fixture.add_buffer();
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(1);
    CHECK(fixture.image->disable() == ir::Return::Success);
    CHECK(fixture.listener->at(0).header.getFrameID() == 2);
    CHECK(gige_mel::provider_statistics(fixture.manager).rejected_frames == 1);
    check_pattern(fixture.listener->at(0));
}

void clock_reset_or_regression_stops_publication() {
    for (const auto transform : {Transform::clock_reset_after_one, Transform::clock_regression_after_one}) {
        Fixture fixture(true, std::make_shared<TransformBackend>(transform));
        fixture.add_buffer();
        fixture.listener->auto_release = true;
        CHECK(fixture.image->enable() == ir::Return::Success);
        fixture.listener->wait_for(1);
        eventually([&] { return gige_mel::provider_statistics(fixture.manager).stream_failures == 1; });
        CHECK(fixture.image->disable() == ir::Return::Success);
        const auto statistics = gige_mel::provider_statistics(fixture.manager);
        CHECK(statistics.delivered_frames == 1);
        CHECK(fixture.listener->count() == 1);
        CHECK(!statistics.last_error.empty());
    }
}

void partial_apply_failure_uses_actual_readback() {
    Fixture fixture(false, std::make_shared<TransformBackend>(Transform::partial_apply_failure));
    CHECK(fixture.control->init() == ir::Return::Success);
    fixture.attach();
    CHECK(fixture.image->getCapabilities().getWidth() == 64);
    // The backend changes the camera to 96x72, then reports an I/O failure
    // instead of satisfying this requested 128x96 configuration.
    expect_error(fixture.c2->send(camera_command(701, 128, 96)),
                 mel::ErrorCode::InsufficientRemoteResources);
    const auto actual = fixture.image->getCapabilities();
    CHECK(actual.getWidth() == 96 && actual.getHeight() == 72);
    CHECK(actual.getBufferSize() == 96 * 72 && actual.getRowPitch() == 96);
    CHECK(fixture.c2->getCapabilities().getWidth() == 96);
    fixture.add_buffer(96 * 72);
    CHECK(fixture.image->enable() == ir::Return::Success);
    fixture.listener->wait_for(1);
    CHECK(fixture.image->disable() == ir::Return::Success);
    const auto delivered = fixture.listener->at(0);
    CHECK(delivered.header.getWidth() == 96 && delivered.header.getHeight() == 72);
    check_pattern(delivered);
}

void failed_readback_blocks_enable_even_with_sufficient_buffers() {
    Fixture fixture(false, std::make_shared<TransformBackend>(Transform::readback_failure_after_apply));
    CHECK(fixture.control->init() == ir::Return::Success);
    fixture.attach();
    // Allocate enough for the requested geometry so capacity cannot explain the
    // enable failure. The device changes, but its actual state cannot be read.
    fixture.add_buffer(128 * 96);
    expect_error(fixture.image->send(camera_command(702, 128, 96)),
                 mel::ErrorCode::InsufficientRemoteResources);
    CHECK(fixture.image->getCapabilities().getWidth() == 64); // Last known state, not desired state.
    CHECK(fixture.image->enable() == ir::Return::Fail);
    CHECK(fixture.listener->count() == 0);
    const auto statistics = gige_mel::provider_statistics(fixture.manager);
    CHECK(statistics.delivered_frames == 0);
    CHECK(!statistics.last_error.empty());
    CHECK(fixture.buffers.front()->release() == ir::Return::Fail);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"factory initialization and channel identity", factory_initialization_and_channel_identity},
        {"registered memory validation and affinity", buffer_validation_affinity_and_mutation},
        {"retained buffers, starvation, release recovery", retained_buffers_starvation_and_release_recovery},
        {"disable waits for callback and quiesces", disable_waits_for_callback_and_quiesces},
        {"detach preserves explicit buffer lease", detach_preserves_delivered_buffer},
        {"camera commands validate full snapshots", camera_commands_validate_full_snapshots},
        {"larger configuration requires larger buffers", larger_configuration_requires_larger_buffers},
        {"metadata callbacks and reentry rejection", metadata_callbacks_are_bounded_and_reentry_is_rejected},
        {"image callback mutations fail without deadlock", image_callback_mutations_fail_without_deadlock},
        {"unsupported surfaces return real errors", unsupported_surfaces_return_real_errors},
        {"callback exception preserves explicit lease", callback_exception_preserves_explicit_lease},
        {"control teardown detaches retained channels", destroying_control_detaches_retained_channels},
        {"old handle cannot change replacement acquisition", old_detached_handle_does_not_clear_replacement_state},
        {"drop control in callback during disable", dropping_control_in_callback_during_disable_finishes},
        {"detach drains metadata callbacks", detach_drains_metadata_callback_and_closes_admission},
        {"alternate backend padded-row repacking", alternate_backend_padded_rows_are_repacked},
        {"reject incomplete and mismatched frames", incomplete_and_mismatched_frames_are_never_published},
        {"first rejected frame cannot poison generation", rejected_first_frame_does_not_poison_generation},
        {"clock reset and regression stop publication", clock_reset_or_regression_stops_publication},
        {"partial apply failure uses actual readback", partial_apply_failure_uses_actual_readback},
        {"failed readback blocks acquisition", failed_readback_blocks_enable_even_with_sufficient_buffers},
    };
    std::size_t passed = 0;
    for (const auto& [name, run] : tests) {
        try {
            run();
            ++passed;
            std::cout << "PASS " << name << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << passed << " provider tests passed\n";
    return 0;
}
