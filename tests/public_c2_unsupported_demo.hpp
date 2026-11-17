#pragma once

// Public-header-only GIGE-MEL-042 check, shared by the native dlopen loader and
// the installed consumer. Includes no provider implementation or backend header.
// Exercises all five unsupported C2 overloads through the loaded provider:
// ready typed Unsupported futures and, with a registered CommandStatus callback,
// one correlated Rejected/CapabilityUnavailable status each. Capabilities stay
// unadvertised. `expect_baseline` verifies the unchanged starting-main contract
// instead (Unsupported futures, no status).
#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/irmel-types/Control.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

inline void demonstrate_c2_unsupported(const std::shared_ptr<ams::iface::irmel::Control>& control,
                                       bool expect_baseline = false) {
    namespace ir = ams::iface::irmel;
    namespace mel = ams::iface::mel;
    using namespace std::chrono_literals;
    using Type = ir::ChannelMetadataCapabilityType;
    const auto require = [](bool value, const std::string& message) { if (!value) throw std::runtime_error(message); };
    const std::set<Type> expected{Type::ChannelCommsTestRep, Type::CommandStatus, Type::BITConfiguration, Type::BITStatus};
    ir::Config config;
    bool found = false;
    for (const auto& capability : control->getCapabilities()) {
        require(!capability.getChannelMetadataCapabilities().contains(Type::CameraProtectCmdResp),
                "camera protection response is advertised");
        if (capability.getChannelTypes() != std::vector{ir::ChannelType::CommandAndControl}) continue;
        require(capability.getChannelMetadataCapabilities() == expected, "unexpected C2 metadata capability set");
        config.setChanID(capability.getChanID());
        config.setChannelType(ir::ChannelType::CommandAndControl);
        found = true;
    }
    require(found, "missing C2 discovery capability");
    auto c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(config));
    require(static_cast<bool>(c2), "advertised C2 channel did not attach");
    std::vector<ir::CommandStatus> statuses;
    const auto cleanup = [&] { (void)control->detachChannel(c2); };
    try {
        // A future that is not immediately ready, holds a response object, or
        // carries any error other than Unsupported fails the demonstration.
        const auto expect_unsupported = [&](auto future, const char* name) {
            require(future.wait_for(0ms) == std::future_status::ready, std::string(name) + " future not ready");
            auto result = future.get();
            require(!result && result.getError().getCode() == mel::ErrorCode::Unsupported &&
                    !result.getError().getDescription().empty(), std::string(name) + " did not return Unsupported");
            return result.getError().getDescription();
        };
        require(c2->registerMetadataCallback(std::function<void(ir::Channel&, const ir::CommandStatus*)>{
            [&](auto&, auto* status) { statuses.push_back(*status); }}) == ir::Return::Success,
            "CommandStatus registration failed");
        constexpr std::uint32_t max = std::numeric_limits<std::uint32_t>::max();
        std::vector<std::pair<std::uint32_t, std::string>> sent;
        // IDs 0..4 and UINT32_MAX..3 (wrapping): exact 32-bit correlation.
        for (std::uint32_t id : {0U, max}) {
            ir::CalibrationConfigurationCmd calibration_configuration; calibration_configuration.setCommandID(id);
            sent.emplace_back(id, expect_unsupported(c2->send(calibration_configuration), "CalibrationConfigurationCmd"));
            ir::CalibrationStatusCmd calibration_status; calibration_status.setCommandID(id + 1U);
            sent.emplace_back(id + 1U, expect_unsupported(c2->send(calibration_status), "CalibrationStatusCmd"));
            ir::CameraProtectCmd protect; protect.setCommandID(id + 2U); protect.setProtectFlag(true);
            sent.emplace_back(id + 2U, expect_unsupported(c2->send(protect), "CameraProtectCmd"));
            ir::EraseCommand erase; erase.setCommandID(id + 3U); erase.setEraseCommandType(ir::EraseCommandType::Zeroize);
            sent.emplace_back(id + 3U, expect_unsupported(c2->send(erase), "EraseCommand"));
            ir::SystemTrackDataResponse track; track.setCommandID(id + 4U);
            mel::RequestFor<ir::CommandStatus> track_future = c2->send(track); // Exact upstream response type.
            sent.emplace_back(id + 4U, expect_unsupported(std::move(track_future), "SystemTrackDataResponse"));
        }
        if (expect_baseline) {
            require(statuses.empty(), "baseline unexpectedly published CommandStatus for an unsupported command");
        } else {
            require(statuses.size() == sent.size(), "unsupported requests did not each publish one CommandStatus");
            for (std::size_t i = 0; i < sent.size(); ++i) {
                const auto& status = statuses[i];
                require(status.getCommandID() == sent[i].first && status.getState() == ir::CommandState::Rejected &&
                        status.getReasonID() == ir::CannotComply::CapabilityUnavailable &&
                        status.getReasonDescription() == sent[i].second,
                        "unsupported CommandStatus correlation/state/reason/description mismatch");
            }
        }
        require(c2->getCapabilities().getChannelMetadataCapabilities() == expected, "attached C2 capability set changed");
        require(c2->registerMetadataCallback(std::function<void(ir::Channel&, const ir::CameraProtectCmdResp*)>{
            [](auto&, auto*) {}}) == ir::Return::NotSupported, "CameraProtectCmdResp registration is not NotSupported");
        require(c2->registerMetadataCallback(std::function<void(ir::Channel&, const mel::CalibrationStatus*)>{
            [](auto&, auto*) {}}) == ir::Return::NotSupported, "CalibrationStatus registration is not NotSupported");
    } catch (...) { cleanup(); throw; }
    const auto delivered = statuses.size();
    require(control->detachChannel(c2) == ir::Return::Success, "C2 detach failed");
    ir::EraseCommand after; after.setCommandID(7);
    auto closed = c2->send(after);
    require(closed.wait_for(0ms) == std::future_status::ready, "detached future not ready");
    auto closed_result = closed.get();
    require(!closed_result && closed_result.getError().getCode() == mel::ErrorCode::InvalidState &&
            statuses.size() == delivered, "detached unsupported request was not InvalidState without status");
}
