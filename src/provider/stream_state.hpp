#pragma once

#include "runtime.hpp"
#include "host_buffer.hpp"
#include <irmel/library/irmel-types/ImageListener.h>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

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
