#pragma once

#include "runtime.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

bool valid_layout(const backend::CameraConfiguration& c, const backend::CameraCapabilities& caps);
ir::ChannelCapability capability(const Runtime& runtime, ir::ChannelType type,
                                 const mel::UCI_ID& id, const ir::Config* config = nullptr);

} // namespace provider_detail
} // namespace gige_mel
