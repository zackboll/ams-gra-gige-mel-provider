#pragma once

#include <gige_mel/backend.hpp>

namespace gige_mel::backend::profiles {

// Checked pool requirement for one run: payload (PayloadSize readback) must be
// positive, at least the checked selected-format image size, at most
// frame_limit, and buffers*payload at most pool_limit. The caller supplies its
// host resource policy; this profile chooses no buffer count or budget. Every
// product is overflow-checked. ErrorCode::unsupported/resource_exhausted with
// the values; nothing is allocated by this function.
struct PoolPlan {
    std::size_t buffer_bytes{0};
    std::size_t buffers{0};
    std::size_t total_bytes{0};
};
Result<PoolPlan> plan_pool(std::int64_t payload_size, const CameraConfiguration& frozen,
                           std::size_t buffers, std::size_t frame_limit,
                           std::size_t pool_limit, PixelFormat format);

// Facts decoded from one popped SDK buffer through public accessors only.
// Fields after `status` are meaningful only when the adapter could read them
// (the SDK's image accessors require SUCCESS status); see validate_frame().
enum class NativeStatus { success, incomplete, other };
enum class NativePayload { image, chunk_or_extended, multipart, other };
struct NativeFrameFacts {
    NativeStatus status{NativeStatus::other};
    int raw_status{0};            // SDK enum value, for diagnostics only
    NativePayload payload{NativePayload::other};
    int raw_payload{0};
    bool has_chunks{false};
    unsigned parts{0};
    bool part_is_2d_image{false};
    std::uint32_t pixel_format{0}; // PFNC code
    std::int64_t width{0}, height{0}, offset_x{0}, offset_y{0};
    std::int64_t padding_x{0}, padding_y{0};
    std::size_t image_bytes{0};    // SDK-reported valid bytes of part 0
    std::size_t part_offset{0};    // part 0 data address minus buffer data address
    std::size_t received_bytes{0}; // SDK-reported received payload size
    bool data_present{false};      // non-null image data pointer at or after the buffer base
    std::size_t capacity_bytes{0}; // allocated size of this pool buffer
    std::uint64_t frame_id{0};
    std::uint32_t storage_bits{8};
    std::uint32_t significant_bits{8};
    ByteOrder byte_order{ByteOrder::not_applicable};
};
// PFNC Mono8 (0x01080001): monochrome, 8 bits per pixel, unpacked.
inline constexpr std::uint32_t pfnc_mono8 = 0x01080001u;
inline constexpr std::uint32_t pfnc_mono12 = 0x01100005u;

enum class FrameVerdict { accept, incomplete, reject };
struct FrameDecision {
    FrameVerdict verdict{FrameVerdict::reject};
    ErrorCode code{ErrorCode::io_error}; // reject: unsupported (payload/format/layout) or io_error (inconsistent facts)
    std::size_t row_stride{0};    // accepted frames: bytes between rows
    std::size_t span_bytes{0};    // accepted: (height-1)*row_stride + packed row bytes
    std::string reason;           // incomplete/reject: precise explanation
};
} // namespace gige_mel::backend::profiles
