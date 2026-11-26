#pragma once

#include <gige_mel/backend.hpp>

namespace gige_mel::backend::synthetic {

// SDK-free profile policy. The legacy fixture's packed layout varies with ROI;
// the explicitly selected two-stream profile has fixed native image properties.
inline StreamTopology multi_stream_topology() {
    return {{{StreamId{0}}, {StreamId{1}}}, {
        {ImageProductId{100}, StreamId{0}, PixelFormat::mono8,
         CameraConfiguration{64, 48, 0, 0, 10}, 64, 64 * 48},
        {ImageProductId{200}, StreamId{1}, PixelFormat::mono12,
         CameraConfiguration{80, 60, 0, 0, 5}, 168, 59 * 168 + 160},
    }};
}

inline std::uint64_t sample(PixelFormat format, std::uint32_t x, std::uint32_t y,
                            std::uint64_t frame_id) noexcept {
    return format == PixelFormat::mono12 ? (17 * x + 31 * y + frame_id) & 0xfffU
                                         : (x + y + frame_id) & 0xffU;
}

} // namespace gige_mel::backend::synthetic