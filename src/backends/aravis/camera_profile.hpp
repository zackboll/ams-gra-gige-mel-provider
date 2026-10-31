#pragma once

// Narrow camera-control profile translation for the optional Aravis adapter.
// SDK-free: it converts native facts (already read by aravis_backend.cpp) into
// the project's integer-Hz / nanosecond / staring-Mono8 contract and plans the
// native writes for a request. Internal and uninstalled.

#include <gige_mel/backend.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gige_mel::backend::aravis_profile {

// Profile "aravis-mono8-free-running-v1" (see docs/aravis-integration.md).
inline constexpr std::string_view profile_name = "aravis-mono8-free-running-v1";

inline constexpr std::string_view sensor_width_feature = "SensorWidth";
inline constexpr std::string_view sensor_height_feature = "SensorHeight";
inline constexpr std::string_view width_feature = "Width";
inline constexpr std::string_view height_feature = "Height";
inline constexpr std::string_view offset_x_feature = "OffsetX";
inline constexpr std::string_view offset_y_feature = "OffsetY";
inline constexpr std::string_view frame_rate_feature = "AcquisitionFrameRate";
// The first implemented name is used; both are SFNC microsecond features.
inline constexpr std::string_view exposure_features[] = {"ExposureTime", "ExposureTimeAbs"};

struct IntegerFeature {
    std::int64_t value{0};
    std::int64_t minimum{0};
    std::int64_t maximum{0};
    std::int64_t increment{0};
};

struct FloatFeature {
    double value{0};
    double minimum{0};
    double maximum{0};
    std::optional<double> increment; // only when the camera advertises one
    std::optional<std::string> unit; // GenICam <Unit>, when declared
};

// Every value is a native readback. Optional members are features that may be
// absent from a camera; absence is part of the profile decision.
struct NativeSnapshot {
    std::int64_t sensor_width{0};
    std::int64_t sensor_height{0};
    IntegerFeature width, height, offset_x, offset_y;
    FloatFeature frame_rate_hz;  // AcquisitionFrameRate, Hz
    FloatFeature exposure_us;    // exposure_feature, microseconds
    std::string exposure_feature;
    std::string pixel_format;
    bool mono8_available{false};
    std::optional<std::string> trigger_selector;
    std::optional<std::string> trigger_mode;
    std::optional<std::string> exposure_auto;
    std::optional<bool> frame_rate_enable;
};

// Checked numeric conversion. Policy: a native value v converts to the integer
// n in the target unit only when it is finite, in range, and
//     |v - n| <= 4 * DBL_EPSILON * max(1, |n|)
// (a relative epsilon tolerance; not a count of adjacent representable
// doubles). That covers the representation error of a correctly rounded double
// only: a real fractional rate (e.g. 30.000300003 Hz) or sub-nanosecond
// exposure fails with ErrorCode::unsupported naming the feature and value.
// Non-finite or negative native values are ErrorCode::io_error (native fault).
Result<std::uint32_t> frame_rate_from_native(double hz, std::string_view feature = frame_rate_feature);
Result<std::chrono::nanoseconds> exposure_from_native(double microseconds,
                                                      std::string_view feature = exposure_features[1]);
// Requests: invalid values are ErrorCode::invalid_argument.
Result<double> frame_rate_to_native(std::uint32_t hz);
Result<double> exposure_to_native(std::chrono::nanoseconds exposure);

// Required stopped/manual preconditions. Never normalized by the adapter.
Status check_preconditions(const NativeSnapshot& native);
// Structural invariants of the native LIMITS that this profile consumes in
// arithmetic, checked on every snapshot before use (no caller is trusted to
// have validated it): sensor extent 1..UINT32_MAX; for each ROI feature an
// increment 1..UINT32_MAX, minimum <= maximum, a grid origin (minimum) that is
// a multiple of the increment and lies in [0, sensor] (sizes) or
// [0, sensor - 1] (offsets); finite, nonnegative, ordered rate and exposure
// bounds; and any advertised scalar increment finite and positive. Violations
// are ErrorCode::unsupported naming the feature and value; nothing is clamped
// or fabricated. Current values are deliberately NOT required to lie inside
// the reported bounds (truthful readback and explicit recovery need that).
Status check_limits(const NativeSnapshot& native);
// Current-value facts needed to plan safe intermediate writes: current ROI
// size in [1, sensor] and offset in [0, sensor - size] per axis
// (ErrorCode::unsupported), and finite, nonnegative current rate/exposure
// (ErrorCode::io_error). Current values need not be on the native grid or
// inside the reported bounds, and need not be representable by the
// project contract (a fractional current rate still permits recovery).
Status check_current(const NativeSnapshot& native);
// Validated capability envelope; fails rather than fabricating values.
Result<CameraCapabilities> capabilities_from(const NativeSnapshot& native);
// Truthful current configuration, or an explicit representation error.
Result<CameraConfiguration> configuration_from(const NativeSnapshot& native);

struct NativeWrite {
    std::string feature;
    bool is_float{false};
    std::int64_t integer{0};
    double real{0};
};

// Validates the complete request before any write and returns an ordered plan.
//
// `current` is the FRESH native snapshot read by this apply(); every limit
// used for prevalidation comes from it, after check_preconditions,
// check_limits and check_current. `envelope` is the open-time snapshot behind
// the published, immutable CameraCapabilities; it is used only to detect an
// incompatible profile change. If the sensor extent, any ROI increment or the
// selected exposure feature differs from it, the request is rejected
// (ErrorCode::unsupported, "reopen") before any write instead of silently
// using obsolete facts or publishing stale capabilities.
//
// Fresh limits treated as final before writes: the ROI minimum/increment grid,
// a positive requested size, the sensor extent and offset + size <= sensor;
// each WRITTEN ROI feature's fresh maximum (R2: unless an EARLIER write of
// this plan DECREASES its axis companion, the only supported way a coupled
// ROI maximum can rise); the frame-rate minimum and maximum; the exposure
// minimum and increment; advertised scalar increments; and the exposure
// maximum whenever the exposure is written before the rate. Deferred to the
// SDK range check at the write (the adapter enables Aravis range checking on
// its own device): a ROI maximum after such a companion decrease, and the
// exposure maximum when the plan lengthens the period first. This profile
// does not model a rate maximum raised by an earlier write in the same
// request; such a request is rejected before writes and can be applied in two
// steps.
//
// ROI order per axis: offset first when it fits the current size, otherwise
// size first, so every intermediate ROI stays inside the sensor. Timing order:
// rate first when the period lengthens, exposure first otherwise. Unchanged
// integer values are not rewritten; floats are always written.
Result<std::vector<NativeWrite>> plan_writes(const CameraConfiguration& request, const NativeSnapshot& current,
                                             const NativeSnapshot& envelope);

// ---------------------------------------------------------------------------
// Acquisition profile (GIGE-MEL-009). SDK-free decisions consumed by the
// adapter's stream; the SDK mechanics stay in aravis_backend.cpp.

// Read-only acquisition admission over the FRESH snapshot that start() read
// under the device lock (GIGE-MEL-009-R1, finding C). Reuses, in order, the
// existing responsibilities: check_preconditions, check_limits (structural
// native limits), check_current (current ROI geometry inside the sensor and
// finite current timing), the same immutable-profile comparison apply() uses
// against `envelope` (sensor extent, any ROI increment or the selected
// exposure feature changed -> the existing "close and reopen" diagnostic),
// and finally configuration_from() (representability). Returns the validated
// configuration to freeze for the run. It plans no writes and applies no
// write-ordering or coupled-maximum rule; dynamic maxima and scalar bounds
// stay governed by the existing profile rules, and current values still need
// not lie on the advertised grid or inside the reported bounds.
Result<CameraConfiguration> acquisition_preflight(const NativeSnapshot& current, const NativeSnapshot& envelope);

// Fixed source pool and explicit allocation limits. A run allocates exactly
// pool_buffers SDK buffers of PayloadSize bytes each, once, before the
// acquisition-start command. No additional pixel buffers are allocated after
// pool setup; each delivered lease still allocates its small shared owner
// record (not pixel storage).
inline constexpr std::size_t pool_buffers = 4;
inline constexpr std::size_t maximum_frame_bytes = std::size_t{64} << 20; // 64 MiB per buffer
inline constexpr std::size_t maximum_pool_bytes = std::size_t{256} << 20; // 256 MiB per run
// The only accepted native acquisition mode (no SingleFrame/MultiFrame).
inline constexpr std::string_view acquisition_mode = "Continuous";

// Checked pool requirement for one run: payload (PayloadSize readback) must be
// positive, at least width*height for the frozen Mono8 configuration, at most
// maximum_frame_bytes, and buffers*payload at most maximum_pool_bytes. Every
// product is overflow-checked. ErrorCode::unsupported/resource_exhausted with
// the values; nothing is allocated by this function.
struct PoolPlan {
    std::size_t buffer_bytes{0};
    std::size_t buffers{0};
    std::size_t total_bytes{0};
};
Result<PoolPlan> plan_pool(std::int64_t payload_size, const CameraConfiguration& frozen,
                           std::size_t buffers = pool_buffers, std::size_t frame_limit = maximum_frame_bytes,
                           std::size_t pool_limit = maximum_pool_bytes);

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
};
// PFNC Mono8 (0x01080001): monochrome, 8 bits per pixel, unpacked.
inline constexpr std::uint32_t pfnc_mono8 = 0x01080001u;

enum class FrameVerdict { accept, incomplete, reject };
struct FrameDecision {
    FrameVerdict verdict{FrameVerdict::reject};
    ErrorCode code{ErrorCode::io_error}; // reject: unsupported (payload/format/layout) or io_error (inconsistent facts)
    std::size_t row_stride{0};    // accepted frames: bytes between rows
    std::size_t span_bytes{0};    // accepted frames: row_stride * height
    std::string reason;           // incomplete/reject: precise explanation
};
// The ONLY payload accepted is one complete, single-part, chunk-free 2-D Mono8
// image whose geometry equals the frozen configuration, with no Y padding and
// X padding representable as row stride, and whose SDK-reported valid bytes
// cover row_stride * height (no fabricated final-row padding). All arithmetic
// is overflow-checked. Non-success SDK status is `incomplete`; everything
// else that is not acceptable is `reject` with the reason.
FrameDecision validate_frame(const NativeFrameFacts& facts, const CameraConfiguration& frozen);

} // namespace gige_mel::backend::aravis_profile
