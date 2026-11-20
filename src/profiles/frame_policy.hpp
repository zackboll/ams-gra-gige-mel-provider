#pragma once

#include <gige_mel/backend.hpp>

namespace gige_mel::backend::profiles {

/// Checked source pool sizes in bytes for one run (not a pool allocation).
struct PoolPlan {
    std::size_t buffer_bytes{0}; ///< Capacity per buffer, from PayloadSize.
    std::size_t buffers{0};      ///< Caller-selected number of buffers.
    std::size_t total_bytes{0};  ///< Checked product of capacity and count.
};

/// @brief Check source pool requirements without allocating buffers.
/// @param[in] payload_size Native PayloadSize readback: positive and at least
/// the packed image size (checked width × bytes per pixel × height).
/// @param[in] frozen Admitted geometry for this acquisition.
/// @param[in] buffers Caller-selected, nonzero count.
/// @param[in] frame_limit Maximum bytes per buffer, inclusive.
/// @param[in] pool_limit Maximum total bytes, inclusive.
/// @param[in] format Selected representation for image size calculation.
/// @return Plan, unsupported for unusable payload/image facts, invalid_argument
/// for zero buffers, or resource_exhausted for overflow or resource limits.
/// @note Capacity is not the actual receive length or proof of frame layout.
Result<PoolPlan> plan_pool(std::int64_t payload_size, const CameraConfiguration& frozen,
                           std::size_t buffers, std::size_t frame_limit,
                           std::size_t pool_limit, PixelFormat format);

/// Adapter classification of SDK completion; success does not admit the image.
enum class NativeStatus {
    success,    ///< SDK reports a completed buffer.
    incomplete, ///< SDK reports incomplete reception; image accessors may be unusable.
    other       ///< Other SDK outcomes, not completed images.
};

/// Native payload category, distinct from PFNC sample representation. The shared
/// monochrome policy admits only a plain, single 2-D image without chunks.
enum class NativePayload { image, chunk_or_extended, multipart, other };

/// @brief Facts obtained from one SDK buffer, not a validated layout or lease.
/// Populate fields only from meaningful native observations: some SDK image
/// accessors require success status. Geometry is measured, not copied from the
/// profile's expected ROI. Preserve the native frame ID when available; it is
/// used in diagnostics and is not intrinsically an admission rule or sequence
/// guarantee. A particular profile may add policy (the teaching module tests parity).
/// @warning Presence, numeric offsets/lengths and a retained owner token do not
/// prove arbitrary C++ pointer provenance. The adapter must establish valid
/// storage from its SDK contract and keep it live through use.
struct NativeFrameFacts {
    NativeStatus status{NativeStatus::other};
    int raw_status{0};            ///< SDK enum value for diagnostics.
    NativePayload payload{NativePayload::other};
    int raw_payload{0};           ///< SDK payload enum value for diagnostics.
    bool has_chunks{false};
    unsigned parts{0};
    bool part_is_2d_image{false};
    std::uint32_t pixel_format{0}; ///< Native PFNC code.
    std::int64_t width{0}, height{0}, offset_x{0}, offset_y{0};
    std::int64_t padding_x{0}, padding_y{0}; ///< Native padding; X is bytes per row.
    std::size_t image_bytes{0};    ///< SDK-reported valid bytes of image part 0.
    std::size_t part_offset{0};    ///< Byte offset of image part 0 from the buffer base.
    std::size_t received_bytes{0}; ///< Actual SDK-reported received payload length.
    bool data_present{false};      ///< Adapter observed image data; not pointer proof.
    std::size_t capacity_bytes{0}; ///< Allocated buffer capacity, not valid byte count.
    std::uint64_t frame_id{0};
    std::uint32_t storage_bits{8};
    std::uint32_t significant_bits{8};
    ByteOrder byte_order{ByteOrder::not_applicable}; ///< Independently established sample order.
};

/// PFNC Mono8: one unpacked 8-bit sample per byte; no sample byte order.
inline constexpr std::uint32_t pfnc_mono8 = 0x01080001u;

/// PFNC Mono12 (not Mono12Packed/Mono12p): 12 significant bits in the low
/// bits of an unsigned 16-bit word (zero high nibble in the supported representation).
/// The shared policy requires independently established little-endian order;
/// neither this code nor host endianness proves it. Admission checks layout
/// facts, not individual sample bits.
inline constexpr std::uint32_t pfnc_mono12 = 0x01100005u;

/// Only accept permits delivery. Incomplete native reception is not the same
/// as a completed buffer with malformed facts or an unsupported layout.
enum class FrameVerdict { accept, incomplete, reject };

/// @brief Frame disposition and, on acceptance, checked source stride/span.
/// In the shared monochrome policy the frozen ROI must match; packed row bytes
/// are width × storage bytes per pixel (1 for Mono8, 2 for Mono12). Source
/// stride adds nonnegative X padding; Y padding is refused. The checked span
/// is `(height - 1) * stride + packed_row_bytes`, excluding final padding.
/// Require received bytes within capacity and the part within received bytes
/// (`image_bytes <= received_bytes - part_offset` after checking the offset).
/// A plain image starts at offset zero and uses the entire received length,
/// either span bytes or `height * stride` with final X padding. Neither spare
/// capacity nor unrelated prefix/trailer bytes are image data. The packed MEL
/// destination uses `height * packed_row_bytes`, dropping source row padding.
struct FrameDecision {
    FrameVerdict verdict{FrameVerdict::reject};
    ErrorCode code{ErrorCode::io_error}; ///< Rejection code (unsupported or inconsistent native facts).
    std::size_t row_stride{0};    ///< Accepted source bytes between row starts.
    std::size_t span_bytes{0};    ///< Accepted extent through the final image byte.
    std::string reason;           ///< Explanation for incomplete or rejected frames.
};
} // namespace gige_mel::backend::profiles
