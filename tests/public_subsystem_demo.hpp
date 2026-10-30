#pragma once

// Public-header-only Subsystem expectation shared by the native loader and the
// installed consumer. Includes no provider implementation or backend header.
#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <irmel/library/irmel-types/Control.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <vector>

// Exact provider software subsystem inventory, version mapping and idle state.
inline void check_public_subsystem_idle(const ams::iface::irmel::SubsystemStatusResp& report) {
    namespace ir = ams::iface::irmel;
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    const auto csci = report.getCSCI();
    require(report.getSubsystemId() == 1 && report.getCriticality() == 0 && report.getSubsystemCount() == 0 &&
            report.getSubsystems().empty() && report.getCsciCount() == 1 && csci.size() == 1,
            "unexpected provider software subsystem inventory");
    const auto version = csci.front().getVersion();
    require(csci.front().getCsci() == "ams-gra-gige-mel-provider" && csci.front().getCriticality() == 0 &&
            version.getSource() == 0 && version.getMajorRevision() == 0 && version.getMinorRevision() == 1 &&
            version.getEngineeringRevision() == 0, "unexpected CSCI identity or version mapping");
    require(report.getFailureLevel() == ir::Failure::Available && csci.front().getFailureLevel() == ir::Failure::Available &&
            csci.front().getMode() == ir::CSCIMode::Idle && csci.front().getConnectionEstablished(),
            "unexpected idle subsystem current state");
}

inline void demonstrate_subsystem(const std::shared_ptr<ams::iface::irmel::Control>& control,
                                  bool expect_absent = false) {
    namespace ir = ams::iface::irmel;
    using namespace std::chrono_literals;
    using Callback = std::function<void(ir::Channel&, const ir::SubsystemStatusResp*)>;
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    ir::Config health_config, c2_config;
    bool health_found = false, advertised = false;
    for (const auto& capability : control->getCapabilities()) {
        if (capability.getChannelTypes() == std::vector{ir::ChannelType::HealthAndStatus}) {
            health_config.setChanID(capability.getChanID());
            health_config.setChannelType(ir::ChannelType::HealthAndStatus);
            health_found = true;
            advertised = capability.getChannelMetadataCapabilities().contains(
                ir::ChannelMetadataCapabilityType::SubsystemStatusResp);
        }
        if (capability.getChannelTypes() == std::vector{ir::ChannelType::CommandAndControl}) {
            c2_config.setChanID(capability.getChanID());
            c2_config.setChannelType(ir::ChannelType::CommandAndControl);
        }
    }
    require(health_found, "missing required HealthAndStatus discovery capability");
    auto health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(control->attachChannel(health_config));
    require(static_cast<bool>(health), "advertised Health channel did not attach");
    std::shared_ptr<ir::C2Channel> c2;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<ir::SubsystemStatusResp> reports; // Outlives the drains in cleanup.
    const auto cleanup = [&] {
        (void)health->disable(); (void)control->detachChannel(health);
        if (c2) (void)control->detachChannel(c2);
    };
    try {
        if (expect_absent) {
            require(!advertised, "baseline unexpectedly advertises SubsystemStatusResp metadata");
            require(health->registerMetadataCallback(Callback{[](auto&, auto*) {}}) == ir::Return::NotSupported,
                    "baseline valid SubsystemStatusResp registration did not return NotSupported");
        } else {
            require(advertised, "missing required SubsystemStatusResp Health metadata capability");
            require(health->registerMetadataCallback(Callback{[&](auto&, auto* report) {
                { std::lock_guard lock(mutex); reports.push_back(*report); }
                changed.notify_all();
            }}) == ir::Return::Success, "SubsystemStatusResp registration failed");
            require(health->enable() == ir::Return::Success, "Health enable failed");
            {
                std::unique_lock lock(mutex);
                require(changed.wait_for(lock, 2s, [&] { return reports.size() >= 2; }), "Subsystem reports missing");
            }
            require(health->disable() == ir::Return::Success, "Health disable failed"); // Quiesced baseline.
            std::vector<ir::SubsystemStatusResp> initial;
            { std::lock_guard lock(mutex); initial = reports; }
            const auto revision = initial.front().getCSCI().front().getBIT_report();
            for (std::size_t i = 0; i < initial.size(); ++i) {
                check_public_subsystem_idle(initial[i]);
                require(initial[i].getCSCI().front().getBIT_report() == revision, "BIT_report changed across unchanged reports");
                if (i > 0) require(initial[i].getStatusSeqNum() == initial[i - 1].getStatusSeqNum() + 1,
                                   "statusSeqNum not sequential across unchanged reports");
            }
            c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(c2_config));
            require(static_cast<bool>(c2), "C2 did not attach");
            auto result = c2->send(ir::BIT_Command{77,{1},{},{}}).get();
            require(result && *result.get() == ir::Return::Success, "BIT execution was not accepted");
            require(health->enable() == ir::Return::Success, "Health re-enable failed");
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 2s, [&] {
                for (auto i = initial.size(); i < reports.size(); ++i)
                    if (reports[i].getCSCI().front().getBIT_report() != revision) return true;
                return false;
            }), "Subsystem report with updated BIT_report missing after BIT execution");
        }
    } catch (...) { cleanup(); throw; }
    cleanup();
}
