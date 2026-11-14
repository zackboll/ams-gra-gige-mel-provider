#include "frame_policy.hpp"
#include <limits>

namespace gige_mel::backend::profiles {
namespace {
Error fail(ErrorCode code, std::string message) { return {code, std::move(message)}; }
}
Result<PoolPlan> plan_pool(std::int64_t payload_size, const CameraConfiguration& frozen, std::size_t buffers,
                           std::size_t frame_limit, std::size_t pool_limit, PixelFormat format) {
    if (payload_size <= 0)
        return fail(ErrorCode::unsupported, "PayloadSize returned " + std::to_string(payload_size) +
                                                "; a positive payload is required; nothing was allocated");
    const auto payload = static_cast<std::uint64_t>(payload_size);
    const auto layout = image_layout(format, frozen.width, frozen.height);
    if (!layout)
        return fail(ErrorCode::unsupported, "the frozen configuration has an empty or overflowing image; nothing was allocated");
    const auto image = layout->image_bytes;
    if (payload < image)
        return fail(ErrorCode::unsupported, "PayloadSize " + std::to_string(payload) + " bytes is smaller than the " +
                                                std::to_string(frozen.width) + "x" + std::to_string(frozen.height) +
                                                (format == PixelFormat::mono12 ? " Mono12 image (" : " Mono8 image (") +
                                                std::to_string(image) + " bytes); nothing was allocated");
    if (payload > std::numeric_limits<std::size_t>::max() || payload > frame_limit)
        return fail(ErrorCode::resource_exhausted, "PayloadSize " + std::to_string(payload) +
                                                       " bytes exceeds the per-frame limit of " + std::to_string(frame_limit) +
                                                       " bytes; nothing was allocated");
    if (buffers == 0) return fail(ErrorCode::invalid_argument, "the source pool needs at least one buffer");
    const auto per_buffer = static_cast<std::size_t>(payload);
    if (per_buffer > std::numeric_limits<std::size_t>::max() / buffers || per_buffer * buffers > pool_limit)
        return fail(ErrorCode::resource_exhausted, std::to_string(buffers) + " buffers of " + std::to_string(per_buffer) +
                                                       " bytes exceed the pool limit of " + std::to_string(pool_limit) +
                                                       " bytes; nothing was allocated");
    return PoolPlan{per_buffer, buffers, per_buffer * buffers};
}

} // namespace gige_mel::backend::profiles
