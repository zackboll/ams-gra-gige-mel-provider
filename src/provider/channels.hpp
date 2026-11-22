#pragma once

#include "mel_types.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

struct Runtime;
class ChannelCore;

/// Construct a stopped Stack attachment after discovery/listener validation.
/// Like the other private factories, only allocates and retains Runtime and a
/// config copy; Control performs publication and owns the attachment set.
std::shared_ptr<ChannelCore> make_stacked_image_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config);
/// Construct the stopped Image attachment with its listener, without opening a source.
std::shared_ptr<ChannelCore> make_image_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config);
/// Construct a qualified Scheduling attachment, without opening a task session.
std::shared_ptr<ChannelCore> make_scheduling_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config);
/// Construct the C2 attachment without applying configuration or executing BIT.
std::shared_ptr<ChannelCore> make_c2_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config);
/// Construct the Health attachment without starting its publisher.
std::shared_ptr<ChannelCore> make_health_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config);

} // namespace provider_detail
} // namespace gige_mel
