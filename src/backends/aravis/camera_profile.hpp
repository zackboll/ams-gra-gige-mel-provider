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

// Checked numeric conversion. Policy: a native value converts only when it is
// finite, in range, and within 4 ULP (relative) of an integer in the target
// unit. That tolerance covers the representation error of a correctly rounded
// double only: a real fractional rate (e.g. 30.000300003 Hz) or sub-nanosecond
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
// Prevalidation uses the open-time `envelope` (sensor geometry, minimum and
// increment grids, advertised scalar rate/exposure bounds) and the coupled
// limit offset + size <= sensor. Dynamic or coupled native maxima (which may
// depend on other features) are revalidated by the SDK at each write: the
// adapter enables Aravis range checking on its own device. `current` supplies
// present values (ordering, unchanged values) and the preconditions.
// ROI order per axis: offset first when it fits the current size, otherwise
// size first, so every intermediate ROI stays inside the sensor. Timing order:
// rate first when the period lengthens, exposure first otherwise. Unchanged
// integer values are not rewritten; floats are always written.
Result<std::vector<NativeWrite>> plan_writes(const CameraConfiguration& request, const NativeSnapshot& current,
                                             const NativeSnapshot& envelope);

} // namespace gige_mel::backend::aravis_profile
