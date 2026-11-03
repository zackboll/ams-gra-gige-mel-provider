#pragma once

// Published MEL only: shared by dlopen and separately installed clients.
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/Control.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>
#include <chrono>
#include <stdexcept>

// TEST DATA, not a real GPS measurement or a qualified solution.
inline ams::iface::mel::NavigationReport navigation_test_data(
    std::chrono::nanoseconds time = std::chrono::nanoseconds{123456789012345}) {
    namespace mel = ams::iface::mel;
    mel::NavigationReport report;
    report.setSystemTime(time);
    report.setState(mel::PositionSolutionState::GPS);
    report.setLatitude(0.5); report.setLongitude(-1.0);
    report.setAltitude(1234.5); report.setAltitudeMSL(1200.25);
    report.setAttitude(mel::Euler{0.1, -0.2, 0.3});
    report.setAttitudeRate(mel::AttitudeRate{mel::Euler{0.01, -0.02, 0.03}, std::chrono::nanoseconds{7}});
    report.setSpeed(mel::NorthEastDown{10, -20, 30});
    report.setAcceleration(mel::NorthEastDown{-1, 2, -3});
    report.setWanderAngle(-0.1); report.setMagneticHeading(0.7);
    report.setPositionVelocityCovarianceUncertainty(mel::PositionVelocityCovariance{
        1, -0.1, 0.2, 2, -0.3, 3, -0.4, 0.5, -0.6, 0.7, -0.8, 0.9, 4, -1, 1.1, 5, -1.2, 6});
    return report;
}

inline void demonstrate_navigation(const std::shared_ptr<ams::iface::irmel::Control>& control,
                                   bool expect_unsupported = false) {
    namespace ir = ams::iface::irmel;
    namespace mel = ams::iface::mel;
    using namespace std::chrono_literals;
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    struct NoImages final : ir::ImageListener {
        unsigned calls = 0;
        void onImage(const ir::Channel&, const ir::FrameHeader&, std::shared_ptr<ir::Buffer>) override { ++calls; }
    };
    auto listener = std::make_shared<NoImages>();
    ir::Config config;
    bool discovered = false;
    for (const auto& cap : control->getCapabilities()) {
        const bool image = cap.getChannelTypes() == std::vector{ir::ChannelType::IRSTImage};
        require(cap.getChannelMetadataCapabilities().contains(ir::ChannelMetadataCapabilityType::NavigationReportResp) ==
            (image && !expect_unsupported), "navigation capability must be Image-only and truthful");
        if (image) {
            config.setChanID(cap.getChanID()); config.setChannelType(ir::ChannelType::IRSTImage);
            config.setImgLstnr(listener); discovered = true;
        }
    }
    require(discovered, "Image discovery failed");
    auto image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(config));
    require(static_cast<bool>(image), "navigation Image attach failed");
    unsigned calls = 0;
    ir::NavigationReportResp observed;
    bool correct_channel = true;
    try {
        const auto registered = image->registerMetadataCallback(
            std::function<void(ir::Channel&, const ir::NavigationReportResp*)>{[&](auto& channel, auto* reply) {
                ++calls; correct_channel = correct_channel && &channel == image.get() && reply;
                if (reply) observed = *reply;
            }});
        require(registered == (expect_unsupported ? ir::Return::NotSupported : ir::Return::Success),
            "navigation callback registration unexpected result");
        const auto report = navigation_test_data();
        auto future = image->send(report);
        require(future.wait_for(0ns) == std::future_status::ready, "navigation future not ready");
        auto result = future.get();
        if (expect_unsupported) {
            require(!result && result.getError().getCode() == mel::ErrorCode::Unsupported && calls == 0,
                "unchanged baseline must return typed Unsupported");
        } else {
            require(result && result.get() && calls == 1 && correct_channel, "navigation acceptance/callback missing");
            const auto& reply = *result.get();
            require(reply.getSystemTime() == report.getSystemTime() && reply.getCommandID() == 0 && reply.getReqId() == 0,
                "navigation acknowledgement is not an exact time echo with unassigned IDs");
            require(observed.getSystemTime() == reply.getSystemTime() && observed.getCommandID() == reply.getCommandID() &&
                observed.getReqId() == reply.getReqId(), "navigation callback differs from future");
            auto stale = image->send(report);
            require(stale.wait_for(0ns) == std::future_status::ready, "stale navigation future not ready");
            auto rejected = stale.get();
            require(!rejected && rejected.getError().getCode() == mel::ErrorCode::InvalidState && calls == 1,
                "stale navigation must fail without callback");
        }
        // No buffers, enable(), SDK calls or image acquisition needed.
        require(listener->calls == 0, "navigation unexpectedly acquired images");
    } catch (...) { (void)control->detachChannel(image); throw; }
    require(control->detachChannel(image) == ir::Return::Success, "navigation detach failed");
}
