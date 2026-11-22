#include "channels.hpp"
#include "channel_core.hpp"
#include "unsupported_callbacks.hpp"
#include <irmel/library/stacked-image/StackedImageChannel.h>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

class StackedImageChannelImpl final : public ChannelCore, public ir::StackedImageChannel {
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::registerMetadataCallback;
    ir::Return registerMetadataCallback(const Callback<ir::LOS3D_KinematicsType>&) override {
        return ir::Return::NotSupported;
    }
};

std::shared_ptr<ChannelCore> make_stacked_image_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config) {
    return std::make_shared<StackedImageChannelImpl>(runtime, config);
}

} // namespace provider_detail
} // namespace gige_mel
