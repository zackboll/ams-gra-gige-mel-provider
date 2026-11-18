#pragma once

// Published MEL declarations only. The exported default synthetic profile has
// no qualified LOS source even though the common adapter can publish LOS.
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/Control.h>
#include <irmel/library/irmel-types/ImageListener.h>
#include <stdexcept>

inline void demonstrate_no_los(const std::shared_ptr<ams::iface::irmel::Control>& control) {
    namespace ir = ams::iface::irmel;
    const auto require = [](bool v) { if (!v) throw std::runtime_error("default LOS must be unsupported and unadvertised"); };
    struct NoImages final : ir::ImageListener {
        void onImage(const ir::Channel&, const ir::FrameHeader&, std::shared_ptr<ir::Buffer>) override {
            throw std::runtime_error("disabled LOS probe received image");
        }
    };
    ir::Config config;
    config.setImgLstnr(std::make_shared<NoImages>());
    for (const auto& cap : control->getCapabilities()) {
        const auto& m = cap.getChannelMetadataCapabilities();
        require(!m.contains(ir::ChannelMetadataCapabilityType::LineOfSightEuler));
        require(!m.contains(ir::ChannelMetadataCapabilityType::LineOfSightReport));
        require(!m.contains(ir::ChannelMetadataCapabilityType::LineOfSightQuaternion));
        require(!m.contains(ir::ChannelMetadataCapabilityType::LOS3DKinematicsType));
        if (cap.getChannelTypes() == std::vector{ir::ChannelType::IRSTImage}) {
            config.setChanID(cap.getChanID()); config.setChannelType(ir::ChannelType::IRSTImage);
        }
    }
    auto image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(config));
    require(static_cast<bool>(image));
    try {
        bool called = false;
        require(image->registerMetadataCallback(std::function<void(ir::Channel&, const ir::LineOfSightEuler*)>{
            [&](auto&, auto*) { called = true; }}) == ir::Return::NotSupported);
        require(image->registerMetadataCallback(std::function<void(ir::Channel&, const ir::LineOfSightReport*)>{
            [&](auto&, auto*) { called = true; }}) == ir::Return::NotSupported);
        require(image->registerMetadataCallback(std::function<void(ir::Channel&, const ir::LineOfSightQuaternion*)>{}) == ir::Return::NotSupported);
        require(image->registerMetadataCallback(std::function<void(ir::Channel&, const ir::LOS3D_KinematicsType*)>{}) == ir::Return::NotSupported);
        const auto metadata = image->getCapabilities().getChannelMetadataCapabilities();
        require(!metadata.contains(ir::ChannelMetadataCapabilityType::LineOfSightEuler));
        require(!metadata.contains(ir::ChannelMetadataCapabilityType::LineOfSightReport));
        require(!called);
        require(control->detachChannel(image) == ir::Return::Success);
    } catch (...) { (void)control->detachChannel(image); throw; }
}