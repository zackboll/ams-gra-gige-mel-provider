#pragma once

// Pinned public MEL headers only; shared by dlopen and installed-package clients.
#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/Control.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <vector>

inline void demonstrate_mode(const std::shared_ptr<ams::iface::irmel::Control>& control,
                             const std::shared_ptr<ams::iface::irmel::Buffer>& buffer,
                             bool expect_active_rejected = false) {
    namespace ir = ams::iface::irmel;
    namespace mel = ams::iface::mel;
    using namespace std::chrono_literals;
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    struct Retain final : ir::ImageListener {
        std::mutex mutex;
        std::condition_variable changed;
        unsigned count = 0;
        std::shared_ptr<ir::Buffer> held;
        void onImage(const ir::Channel&, const ir::FrameHeader&, std::shared_ptr<ir::Buffer> buffer) override {
            { std::lock_guard lock(mutex); held = std::move(buffer); ++count; }
            changed.notify_all();
        }
        bool wait(unsigned n) {
            std::unique_lock lock(mutex);
            return changed.wait_for(lock, 2s, [&] { return count >= n; });
        }
    };
    auto listener = std::make_shared<Retain>();
    ir::Config c2_config, image_config, health_config;
    for (const auto& capability : control->getCapabilities()) {
        ir::Config* config = nullptr;
        if (capability.getChannelTypes() == std::vector{ir::ChannelType::CommandAndControl}) config = &c2_config;
        if (capability.getChannelTypes() == std::vector{ir::ChannelType::IRSTImage}) config = &image_config;
        if (capability.getChannelTypes() == std::vector{ir::ChannelType::HealthAndStatus}) config = &health_config;
        if (!config) continue;
        config->setChanID(capability.getChanID()); config->setChannelType(capability.getChannelTypes().front());
    }
    image_config.setImgLstnr(listener);
    // Cleanup stops/unbinds the caller's Buffer before storage is destroyed.
    std::vector<std::byte> storage(64 * 48);
    auto c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(c2_config));
    auto image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(image_config));
    auto health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(control->attachChannel(health_config));
    std::vector<ir::CommandStatus> statuses;
    bool identity = true;
    std::mutex mutex;
    std::condition_variable changed;
    bool idle = false;
    const auto cleanup = [&] {
        if (health) { (void)health->disable(); (void)control->detachChannel(health); }
        if (image) {
            (void)image->disable();
            if (buffer) { (void)buffer->release(); (void)image->unregisterBuffer(buffer); }
            (void)control->detachChannel(image);
        }
        if (c2) (void)control->detachChannel(c2);
    };
    try {
        require(c2 && image && health && buffer, "ModeCmd channel/buffer creation failed");
        ir::CameraCommand setup;
        setup.setWidth(64); setup.setHeight(48); setup.setFrameRate(120);
        setup.setIntegrationTime(1ms); setup.setImageType(ir::ImageType::Staring);
        auto configured = image->send(setup).get();
        require(configured && configured.get()->getCommandSuccess(), "ModeCmd camera setup failed");
        require(buffer->init(storage.data(), storage.size(), 17) == ir::Return::Success &&
            image->registerBuffer(buffer) == ir::Return::Success, "ModeCmd buffer registration failed");
        require(c2->registerMetadataCallback(std::function<void(ir::Channel&, const ir::CommandStatus*)>{
            [&](auto& channel, auto* status) { identity = identity && &channel == c2.get(); statuses.push_back(*status); }
        }) == ir::Return::Success, "ModeCmd status registration failed");
        require(image->enable() == ir::Return::Success && listener->wait(1), "ModeCmd initial capture failed");
        const auto bytes = storage;
        ir::ModeCmd command;
        command.setCommandID(UINT32_MAX); command.setState(mel::MFA_State::Standby); command.setMode(ir::MFA_Mode::Unused);
        auto future = c2->send(command);
        require(future.wait_for(0ms) == std::future_status::ready, "Standby future not ready");
        auto result = future.get();
        require(statuses.size() == 1 && identity && statuses.back().getCommandID() == UINT32_MAX,
            "Standby status count/identity/correlation failed");
        const auto& status = statuses.back();
        if (expect_active_rejected) {
            require(!result && result.getError().getCode() == mel::ErrorCode::InvalidState &&
                status.getState() == ir::CommandState::Rejected && status.getReasonID() == ir::CannotComply::StateOrSettings &&
                !status.getReasonDescription().empty(), "starting main must reject active Standby");
            require(image->enable() == ir::Return::Success, "baseline rejection changed enabled state");
            require(image->disable() == ir::Return::Success, "baseline explicit disable failed");
        } else {
            require(result && *result.get() == ir::MFA_Mode::Unused && status.getState() == ir::CommandState::Accepted &&
                status.getReasonID() == ir::CannotComply::NotSet && status.getReasonDescription().empty(),
                "Standby transition did not accept");
            // Were the old acquisition still enabled this would return Success.
            require(image->enable() == ir::Return::Fail, "Standby did not disable Image or implicitly released buffer");
        }
        require(storage == bytes, "Standby corrupted held bytes");
        command.setCommandID(0);
        auto idle_ack = c2->send(command);
        require(idle_ack.wait_for(0ms) == std::future_status::ready, "idle Standby future not ready");
        auto acknowledged = idle_ack.get();
        require(acknowledged && *acknowledged.get() == ir::MFA_Mode::Unused && statuses.size() == 2 &&
            statuses.back().getCommandID() == 0 && statuses.back().getState() == ir::CommandState::Accepted &&
            statuses.back().getReasonID() == ir::CannotComply::NotSet && statuses.back().getReasonDescription().empty(),
            "idempotent idle Standby correlation failed");
        command.setState(mel::MFA_State::Operate); command.setMode(ir::MFA_Mode::TaskSched);
        auto rejected_future = c2->send(command);
        require(rejected_future.wait_for(0ms) == std::future_status::ready, "Operate future not ready");
        auto rejected = rejected_future.get();
        require(!rejected && rejected.getError().getCode() == mel::ErrorCode::Unsupported && statuses.size() == 3 &&
            statuses.back().getCommandID() == 0 && statuses.back().getState() == ir::CommandState::Rejected &&
            statuses.back().getReasonID() == ir::CannotComply::CapabilityUnavailable &&
            !statuses.back().getReasonDescription().empty(), "Operate rejection correlation failed");
        require(health->registerMetadataCallback(std::function<void(ir::Channel&, const mel::MFA_Status*)>{
            [&](auto&, auto* report) {
                { std::lock_guard lock(mutex); idle = report->getMFAState() == mel::MFA_State::Standby; }
                changed.notify_all();
            }}) == ir::Return::Success && health->enable() == ir::Return::Success, "ModeCmd Health registration failed");
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 2s, [&] { return idle; }), "ModeCmd did not leave observed Standby");
        }
        require(health->disable() == ir::Return::Success, "ModeCmd Health disable failed");
        require(buffer->release() == ir::Return::Success, "held buffer explicit release failed");
        require(image->enable() == ir::Return::Success && listener->wait(2), "ModeCmd restart capture failed");
        require(image->disable() == ir::Return::Success && buffer->release() == ir::Return::Success,
            "ModeCmd final disable/release failed");
    } catch (...) { cleanup(); throw; }
    cleanup();
}