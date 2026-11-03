#pragma once

// Published upstream MEL only; shared by native loader and installed consumer.
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/Control.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>
#include <stdexcept>

inline void demonstrate_bad_pixels(const std::shared_ptr<ams::iface::irmel::Control>& control,
                                   bool expect_unsupported = false) {
    namespace ir = ams::iface::irmel;
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    struct NoImages final : ir::ImageListener {
        unsigned calls = 0;
        void onImage(const ir::Channel&, const ir::FrameHeader&, std::shared_ptr<ir::Buffer>) override { ++calls; }
    };
    auto listener = std::make_shared<NoImages>();
    ir::Config config;
    for (const auto& cap : control->getCapabilities()) {
        const bool image = cap.getChannelTypes() == std::vector{ir::ChannelType::IRSTImage};
        require(cap.getChannelMetadataCapabilities().contains(ir::ChannelMetadataCapabilityType::BadPixelList) ==
            (image && !expect_unsupported), "BadPixelList discovery must be truthful and Image-only");
        if (image) {
            config.setChanID(cap.getChanID()); config.setChannelType(ir::ChannelType::IRSTImage);
            config.setImgLstnr(listener);
        }
    }
    auto image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(config));
    require(static_cast<bool>(image), "BadPixelList Image attach failed");
    unsigned calls = 0;
    bool valid = false;
    try {
        require(image->getCapabilities().getChannelMetadataCapabilities().contains(ir::ChannelMetadataCapabilityType::BadPixelList)
            == !expect_unsupported, "BadPixelList attachment capability mismatch");
        const auto result = image->registerMetadataCallback(
            std::function<void(ir::Channel&, const ir::BadPixelList*)>{[&](auto& channel, auto* list) {
                ++calls;
                valid = &channel == image.get() && list && list->getSize() == 0 &&
                    list->getBadPixelCount() == 0 && list->getBadPixelList().empty();
            }});
        require(result == (expect_unsupported ? ir::Return::NotSupported : ir::Return::Success),
            "BadPixelList registration unexpected result");
        require(expect_unsupported ? calls == 0 : calls == 1 && valid, "BadPixelList one-shot zero-entry payload missing");
        require(listener->calls == 0, "BadPixelList unexpectedly acquired images");
    } catch (...) { (void)control->detachChannel(image); throw; }
    require(control->detachChannel(image) == ir::Return::Success, "BadPixelList detach failed");
    require(expect_unsupported ? calls == 0 : calls == 1, "BadPixelList repeated after detach");
}
