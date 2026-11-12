#pragma once

#include <cstddef>

namespace gige_mel::backend::aravis {

// Host resource policy for the Aravis adapter, independent of camera-model
// feature translation. A run allocates exactly pool_buffers SDK buffers of
// PayloadSize bytes each, once, before the acquisition-start command. No
// additional pixel buffers are allocated after pool setup; each delivered
// lease still allocates its small shared owner record (not pixel storage).
inline constexpr std::size_t pool_buffers = 4;
inline constexpr std::size_t maximum_frame_bytes = std::size_t{64} << 20;
inline constexpr std::size_t maximum_pool_bytes = std::size_t{256} << 20;

} // namespace gige_mel::backend::aravis
