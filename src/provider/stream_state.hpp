#pragma once

#include "runtime.hpp"
#include "host_buffer.hpp"
#include <irmel/library/irmel-types/ImageListener.h>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/** @brief One enabled acquisition's strongly owned worker context.
 * Retains Runtime, listener, destination wrappers and exactly one active
 * source (Image or Stack). Configuration, generation, location and buffer
 * membership are frozen at enable; no per-frame configuration lookup can
 * relabel queued data. The worker owns this state independently of the channel
 * and uses weak channel/Control links, pinning them only for delivery.
 *
 * Stop sets the atomic stopping flag and requests source cancellation before
 * joining. A terminal worker failure sets failed, but logical acquisition
 * ownership persists until cleanup (disable, Standby or detach). Runtime's weak
 * Health publication is cleared under state_mutex before the channel drops its
 * strong owner. No stop or failure implicitly releases delivered host buffers.
 */
struct StreamState {
    std::shared_ptr<Runtime> runtime;
    std::unique_ptr<backend::IFrameStream> stream;
    std::unique_ptr<backend::IStackedImageStream> stack_stream;
    std::shared_ptr<ir::ImageListener> listener;
    std::vector<std::shared_ptr<HostBuffer>> buffers;
    backend::CameraConfiguration configuration;
    std::uint64_t generation{};
    mel::ComponentLocation location;
    std::atomic<bool> stopping{false};
    std::atomic<bool> failed{false};
};

} // namespace provider_detail
} // namespace gige_mel
