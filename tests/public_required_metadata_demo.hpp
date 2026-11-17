#pragma once

// Public-header-only GIGE-MEL-041 classification check, shared by the native
// loader and the installed consumer. Includes no provider implementation or
// backend header. DiscreteStatus and MFA_SecurityAuditRecord are @Required but
// have no pinned ChannelMetadataCapabilityType enumerator; this provider keeps
// both NotSupported and unadvertised (BLOCKED_BY_UPSTREAM_CAPABILITY_ENUM).
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <irmel/library/irmel-types/Control.h>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

inline void demonstrate_required_metadata_blocked(const std::shared_ptr<ams::iface::irmel::Control>& control) {
    namespace ir = ams::iface::irmel;
    namespace mel = ams::iface::mel;
    using Type = ir::ChannelMetadataCapabilityType;
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    const std::set<Type> expected{Type::ChannelCommsTestRep, Type::MFAStatus, Type::MFAStatusDetailed,
                                  Type::BITStatus, Type::SubsystemStatusResp};
    const std::set<Type> reserved{Type::Reserved3, Type::Reserved5, Type::Reserved9, Type::Reserved10};
    ir::Config config;
    bool found = false;
    for (const auto& capability : control->getCapabilities()) {
        for (auto value : capability.getChannelMetadataCapabilities())
            require(!reserved.contains(value), "a reserved metadata capability value is advertised");
        if (capability.getChannelTypes() == std::vector{ir::ChannelType::HealthAndStatus}) {
            require(capability.getChannelMetadataCapabilities() == expected, "unexpected Health metadata capability set");
            config.setChanID(capability.getChanID());
            config.setChannelType(ir::ChannelType::HealthAndStatus);
            found = true;
        }
    }
    require(found, "missing required HealthAndStatus discovery capability");
    auto health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(control->attachChannel(config));
    require(static_cast<bool>(health), "advertised Health channel did not attach");
    bool invoked = false;
    const auto discrete = health->registerMetadataCallback(
        std::function<void(ir::Channel&, const mel::DiscreteStatus* const)>{[&](auto&, auto*) { invoked = true; }});
    const auto audit = health->registerMetadataCallback(
        std::function<void(ir::Channel&, const mel::MFA_SecurityAuditRecord* const)>{[&](auto&, auto*) { invoked = true; }});
    const bool attached_unchanged = health->getCapabilities().getChannelMetadataCapabilities() == expected;
    const auto detached = control->detachChannel(health);
    require(discrete == ir::Return::NotSupported, "DiscreteStatus registration did not return NotSupported");
    require(audit == ir::Return::NotSupported, "MFA_SecurityAuditRecord registration did not return NotSupported");
    require(attached_unchanged, "attached Health capability set changed");
    require(detached == ir::Return::Success && !invoked, "blocked registration invoked a callback or detach failed");
}
