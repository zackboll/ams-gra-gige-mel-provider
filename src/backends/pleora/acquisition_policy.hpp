#pragma once

#include <chrono>
#include <cstddef>

namespace gige_mel::backend::pleora {

inline constexpr std::size_t pool_buffers = 4;
inline constexpr std::size_t maximum_frame_bytes = 64 * 1024 * 1024;
inline constexpr std::size_t maximum_pool_bytes = pool_buffers * maximum_frame_bytes;
inline constexpr std::chrono::milliseconds receive_polling_slice{10};
inline constexpr unsigned discovery_timeout_ms = 1000;

} // namespace gige_mel::backend::pleora
