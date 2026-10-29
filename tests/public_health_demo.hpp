#pragma once

// A consumer demonstration: published MEL declarations only, no provider API.
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

inline void demonstrate_health(const std::shared_ptr<ams::iface::irmel::Control>& control,
                               bool expect_absent = false) {
    namespace ir = ams::iface::irmel;
    namespace mel = ams::iface::mel;
    using namespace std::chrono_literals;
    ir::Config config;
    config.setChannelType(ir::ChannelType::HealthAndStatus);
    bool advertised = false;
    for (const auto& cap : control->getCapabilities()) {
        if (cap.getChannelTypes() == std::vector{ir::ChannelType::HealthAndStatus}) {
            config.setChanID(cap.getChanID()); advertised = true;
        }
    }
    if (expect_absent) {
        // Also try the feature's manager-local UUID, not just a default ID.
        mel::UCI_ID id;
        std::array<std::uint8_t, mel::UUID_SIZE> uuid{};
        uuid.back() = 3; id.setUUID(uuid); config.setChanID(id);
        if (advertised || control->attachChannel(config)) throw std::runtime_error("baseline unexpectedly supports Health");
        return;
    }
    if (!advertised) throw std::runtime_error("missing required HealthAndStatus discovery capability");
    std::mutex mutex;
    std::condition_variable changed;
    bool status_seen = false, detailed_seen = false, valid = true;
    auto health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(control->attachChannel(config));
    if (!health) throw std::runtime_error("advertised Health channel did not attach");
    auto cleanup = [&] { (void)health->disable(); (void)control->detachChannel(health); };
    try {
        const auto status = health->registerMetadataCallback(
            std::function<void(ir::Channel&, const mel::MFA_Status*)>{[&](auto&, auto* report) {
                { std::lock_guard lock(mutex);
                  valid = valid && report->getMFAState() == mel::MFA_State::Standby &&
                      report->getStateTransitionStatus() == mel::StateTransitionStatus::NotTransitioning;
                  status_seen = true; }
                changed.notify_all();
            }});
        const auto detailed = health->registerMetadataCallback(
            std::function<void(ir::Channel&, const mel::MFA_StatusDetailed*)>{[&](auto&, auto* report) {
                { std::lock_guard lock(mutex);
                  bool schema = false, idle = false;
                  for (const auto& pair : report->getStatus()) {
                      if (pair.getName() == "gige_mel.health_schema" && pair.getValue() == "1") schema = true;
                      if (pair.getName() == "gige_mel.acquisition_state" && pair.getValue() == "idle") idle = true;
                  }
                  valid = valid && status_seen && schema && idle && report->getStatus().size() == 12;
                  detailed_seen = true; }
                changed.notify_all();
            }});
        if (status != ir::Return::Success || detailed != ir::Return::Success || health->enable() != ir::Return::Success)
            throw std::runtime_error("Health registration/enable failed");
        {
            std::unique_lock lock(mutex);
            if (!changed.wait_for(lock, 2s, [&] { return status_seen && detailed_seen; }) || !valid)
                throw std::runtime_error("Health did not report current Standby status and schema-1 diagnostics");
        }
    } catch (...) { cleanup(); throw; }
    cleanup();
}