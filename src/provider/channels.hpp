#pragma once

#include "mel_types.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

struct Runtime;
class ChannelCore;

std::shared_ptr<ChannelCore> make_stacked_image_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config);
std::shared_ptr<ChannelCore> make_image_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config);
std::shared_ptr<ChannelCore> make_scheduling_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config);
std::shared_ptr<ChannelCore> make_c2_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config);
std::shared_ptr<ChannelCore> make_health_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config);

} // namespace provider_detail
} // namespace gige_mel
