#pragma once

// Pinned public MEL headers only; shared by dlopen and installed-package clients.
#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <irmel/library/irmel-types/Control.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

inline void demonstrate_mode(const std::shared_ptr<ams::iface::irmel::Control>& control,
                             bool expect_unsupported = false) {
    namespace ir = ams::iface::irmel;
    namespace mel = ams::iface::mel;
    using namespace std::chrono_literals;
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    ir::Config c2_config, health_config;
    for (const auto& capability : control->getCapabilities()) {
        if (capability.getChannelTypes() == std::vector{ir::ChannelType::CommandAndControl}) {
            c2_config.setChanID(capability.getChanID()); c2_config.setChannelType(ir::ChannelType::CommandAndControl);
        }
        if (capability.getChannelTypes() == std::vector{ir::ChannelType::HealthAndStatus}) {
            health_config.setChanID(capability.getChanID()); health_config.setChannelType(ir::ChannelType::HealthAndStatus);
        }
    }
    auto c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(c2_config));
    require(static_cast<bool>(c2), "ModeCmd C2 discovery/attachment failed");
    std::shared_ptr<ir::HealthStatusChannel> health;
    std::vector<ir::CommandStatus> statuses;
    std::mutex mutex;
    std::condition_variable changed;
    bool observed = false, valid = true;
    const auto cleanup = [&] {
        if (health) { (void)health->disable(); (void)control->detachChannel(health); }
        (void)control->detachChannel(c2);
    };
    try {
        require(c2->registerMetadataCallback(std::function<void(ir::Channel&, const ir::CommandStatus*)>{
            [&](auto&, auto* status) { statuses.push_back(*status); }}) == ir::Return::Success,
            "ModeCmd status registration failed");
        ir::ModeCmd command;
        command.setCommandID(0); command.setState(mel::MFA_State::Standby); command.setMode(ir::MFA_Mode::Unused);
        auto future = c2->send(command);
        require(future.wait_for(0ms) == std::future_status::ready, "Standby future not ready");
        auto result = future.get();
        if (expect_unsupported) {
            require(!result && result.getError().getCode() == mel::ErrorCode::Unsupported && statuses.empty(),
                    "unchanged starting main must reject Standby with Unsupported and no stub status");
        } else {
            require(result && *result.get() == ir::MFA_Mode::Unused && statuses.size() == 1,
                    "Standby did not return Unused and one status");
            const auto& accepted = statuses.front();
            require(accepted.getCommandID() == 0 && accepted.getState() == ir::CommandState::Accepted &&
                    accepted.getReasonID() == ir::CannotComply::NotSet && accepted.getReasonDescription().empty(),
                    "Standby acceptance correlation failed");
            command.setCommandID(UINT32_MAX); command.setState(mel::MFA_State::Operate); command.setMode(ir::MFA_Mode::TaskSched);
            auto rejected = c2->send(command).get();
            require(!rejected && rejected.getError().getCode() == mel::ErrorCode::Unsupported && statuses.size() == 2,
                    "Operate/TaskSched did not return typed Unsupported and one status");
            const auto& status = statuses.back();
            require(status.getCommandID() == UINT32_MAX && status.getState() == ir::CommandState::Rejected &&
                    status.getReasonID() == ir::CannotComply::CapabilityUnavailable && !status.getReasonDescription().empty(),
                    "Operate rejection correlation failed");
        }
        // No Image is attached or enabled in this focused demonstration. Verify
        // actual published state/counters too, not a fabricated command state.
        health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(control->attachChannel(health_config));
        require(static_cast<bool>(health), "ModeCmd Health attachment failed");
        require(health->registerMetadataCallback(std::function<void(ir::Channel&, const mel::MFA_Status*)>{
            [&](auto&, auto* report) {
                std::lock_guard lock(mutex); valid = valid && report->getMFAState() == mel::MFA_State::Standby;
            }}) == ir::Return::Success, "ModeCmd Health state registration failed");
        require(health->registerMetadataCallback(std::function<void(ir::Channel&, const mel::MFA_StatusDetailed*)>{
            [&](auto&, auto* report) {
                { std::lock_guard lock(mutex);
                  unsigned checked = 0;
                  for (const auto& field : report->getStatus()) {
                      if (field.getName() == "gige_mel.acquisition_state") { valid = valid && field.getValue() == "idle"; ++checked; }
                      if (field.getName() == "gige_mel.delivered_frames") { valid = valid && field.getValue() == "0"; ++checked; }
                  }
                  valid = valid && checked == 2; observed = true; }
                changed.notify_all();
            }}) == ir::Return::Success && health->enable() == ir::Return::Success,
            "ModeCmd Health detail registration/enable failed");
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 2s, [&] { return observed; }) && valid,
                "ModeCmd unexpectedly caused Image acquisition");
    } catch (...) { cleanup(); throw; }
    cleanup();
}