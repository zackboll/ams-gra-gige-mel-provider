#include "channels.hpp"
#include "channel_core.hpp"
#include "unsupported_callbacks.hpp"
#include <irmel/library/stacked-image/StackedImageChannel.h>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/** @brief Delivers one source-owned, declared Stack product per copy, not aggregated Image frames.
 * Discovery requires validated optional Stack support; attach requires the
 * full discovered identity and a StackedImageListener. Shares the exclusive
 * acquisition slot and session overlap protection with Image. Provides common
 * communications metadata only, with no LOS3D or per-plane callback fallback.
 * @see ChannelCore::run_stacks
 */
class StackedImageChannelImpl final : public ChannelCore, public ir::StackedImageChannel {
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::registerMetadataCallback;
    /// Return NotSupported without retaining the LOS3D callable or invoking it.
    ir::Return registerMetadataCallback(const Callback<ir::LOS3D_KinematicsType>&) override {
        return ir::Return::NotSupported;
    }
};

std::shared_ptr<ChannelCore> make_stacked_image_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config) {
    return std::make_shared<StackedImageChannelImpl>(runtime, config);
}

} // namespace provider_detail
} // namespace gige_mel
