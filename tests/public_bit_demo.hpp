#pragma once

// Public-header-only expectation shared by the native loader and installed client.
#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <irmel/library/irmel-types/Control.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

inline void demonstrate_bit(const std::shared_ptr<ams::iface::irmel::Control>& control,
                            bool expect_absent = false) {
    namespace ir = ams::iface::irmel;
    namespace mel = ams::iface::mel;
    using namespace std::chrono_literals;
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    ir::Config c2_config, health_config;
    bool c2_bit = false, health_bit = false;
    for (const auto& capability : control->getCapabilities()) {
        const auto& metadata = capability.getChannelMetadataCapabilities();
        if (capability.getChannelTypes() == std::vector{ir::ChannelType::CommandAndControl}) {
            c2_config.setChanID(capability.getChanID());
            c2_config.setChannelType(ir::ChannelType::CommandAndControl);
            c2_bit = metadata.contains(ir::ChannelMetadataCapabilityType::BITConfiguration) &&
                     metadata.contains(ir::ChannelMetadataCapabilityType::BITStatus);
        }
        if (capability.getChannelTypes() == std::vector{ir::ChannelType::HealthAndStatus}) {
            health_config.setChanID(capability.getChanID()); health_config.setChannelType(ir::ChannelType::HealthAndStatus);
            health_bit = metadata.contains(ir::ChannelMetadataCapabilityType::BITStatus);
        }
    }
    auto c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(c2_config));
    require(static_cast<bool>(c2), "C2 did not attach");
    std::shared_ptr<ir::HealthStatusChannel> health;
    std::mutex mutex;
    std::condition_variable changed;
    bool accepted = false, catalogue_seen = false, result_seen = false, health_seen = false, valid = true;
    const mel::UCI_ID expected({0x9c,0x85,0xc2,0x63,0x54,0x6e,0x4e,0x11,0x9b,0x71,0xd2,0x40,0x5a,0x13,0x80,0x01},
                               "gige_mel.provider_state.v1");
    const auto cleanup = [&] {
        if (health) { (void)health->disable(); (void)control->detachChannel(health); }
        (void)c2->disable(); (void)control->detachChannel(c2);
    };
    try {
        if (expect_absent) {
            require(!c2_bit && !health_bit, "baseline unexpectedly advertises BIT metadata");
            require(c2->registerMetadataCallback(std::function<void(ir::Channel&,const mel::BIT_Configuration*)>{
                [](auto&,auto*) {}}) == ir::Return::NotSupported, "baseline catalogue unexpectedly supported");
            require(c2->registerMetadataCallback(std::function<void(ir::Channel&,const mel::BIT_Status*)>{
                [](auto&,auto*) {}}) == ir::Return::NotSupported, "baseline status unexpectedly supported");
            health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(control->attachChannel(health_config));
            require(health && health->registerMetadataCallback(std::function<void(ir::Channel&,const mel::BIT_Status*)>{
                [](auto&,auto*) {}}) == ir::Return::NotSupported, "baseline Health BIT unexpectedly supported");
            auto result = c2->send(ir::BIT_Command{42,{1},{},{}}).get();
            require(!result && result.getError().getCode() == mel::ErrorCode::Unsupported,
                    "baseline valid initiation did not return Unsupported");
        } else {
            require(c2_bit && health_bit, "missing required C2/Health BIT metadata capability");
            require(c2->registerMetadataCallback(std::function<void(ir::Channel&,const mel::BIT_Configuration*)>{
                [&](auto&,auto* catalogue) {
                    catalogue_seen = catalogue->getBit().size() == 1 && catalogue->getBit().front().getBitID() == expected &&
                        catalogue->getBit().front().getBitItemName().size() == 2;
                }}) == ir::Return::Success && catalogue_seen, "BIT catalogue registration/mapping failed");
            require(c2->registerMetadataCallback(std::function<void(ir::Channel&,const mel::BIT_Status*)>{
                [&](auto&,auto* report) {
                    if (report->getCompletedBITs().empty()) return;
                    result_seen = report->getCompletedBITs().size() == 1 && report->getCompletedBITs().front().getBitID() == expected &&
                        report->getCompletedBITs().front().getResult() == mel::BIT_Result::Pass && report->getFaults().empty();
                }}) == ir::Return::Success, "C2 BIT status registration failed");
            require(c2->registerMetadataCallback(std::function<void(ir::Channel&,const ir::CommandStatus*)>{
                [&](auto&,auto* status) {
                    accepted = result_seen && status->getCommandID() == 42 && status->getState() == ir::CommandState::Accepted &&
                        status->getReasonID() == ir::CannotComply::NotSet && status->getReasonDescription().empty();
                }}) == ir::Return::Success, "CommandStatus registration failed");
            auto result = c2->send(ir::BIT_Command{42,{1},{},{}}).get();
            require(result && *result.get() == ir::Return::Success && accepted && result_seen,
                    "BIT execution acceptance or separate completed result missing");
            health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(control->attachChannel(health_config));
            require(static_cast<bool>(health), "Health did not attach");
            require(health->registerMetadataCallback(std::function<void(ir::Channel&,const mel::BIT_Status*)>{
                [&](auto&,auto* report) {
                    { std::lock_guard lock(mutex);
                      valid = valid && report->getCompletedBITs().size() == 1 &&
                          report->getCompletedBITs().front().getBitID() == expected && report->getFaults().empty();
                      health_seen = true; }
                    changed.notify_all();
                }}) == ir::Return::Success && health->enable() == ir::Return::Success, "Health BIT registration/enable failed");
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock,2s,[&] { return health_seen; }) && valid, "Health retained BIT/current faults missing");
        }
    } catch (...) { cleanup(); throw; }
    cleanup();
}