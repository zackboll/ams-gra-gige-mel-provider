#include "camera_profile.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace gige_mel::backend::aravis_profile {
namespace {

Error fail(ErrorCode code, std::string message) { return Error{code, std::move(message)}; }

// Shortest round-trip decimal text, so diagnostics show the exact native value.
std::string text(double value) {
    std::array<char, 64> buffer{};
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("<unprintable>");
}

// Representation-error tolerance, a relative epsilon bound:
//     |value - integer| <= 4 * DBL_EPSILON * max(1, |integer|).
// (Not exactly four adjacent representable doubles at every magnitude.) It
// cannot absorb a genuine fraction (30.000300003 Hz differs by ~1e-5 relative).
bool near_integer(double value, double integer) {
    const double scale = std::max(1.0, std::fabs(integer));
    return std::fabs(value - integer) <= 4.0 * std::numeric_limits<double>::epsilon() * scale;
}

constexpr double two_pow_63 = 9223372036854775808.0;

Status unit_is(const FloatFeature& feature, std::string_view name, std::initializer_list<std::string_view> accepted) {
    if (!feature.unit || feature.unit->empty()) return {};
    for (const auto unit : accepted)
        if (*feature.unit == unit) return {};
    return fail(ErrorCode::unsupported, std::string(name) + " declares unit '" + *feature.unit +
                                            "'; the profile requires " + std::string(*accepted.begin()));
}

Result<std::uint32_t> dimension(std::int64_t value, std::string_view feature) {
    if (value <= 0 || value > std::numeric_limits<std::uint32_t>::max())
        return fail(ErrorCode::unsupported, std::string(feature) + " reads back " + std::to_string(value) +
                                                " pixels, outside the representable range 1..4294967295");
    return static_cast<std::uint32_t>(value);
}

Result<std::uint32_t> offset(std::int64_t value, std::string_view feature) {
    if (value < 0 || value > std::numeric_limits<std::uint32_t>::max())
        return fail(ErrorCode::unsupported, std::string(feature) + " reads back " + std::to_string(value) +
                                                " pixels, outside the representable range 0..4294967295");
    return static_cast<std::uint32_t>(value);
}

constexpr std::int64_t uint32_max = std::numeric_limits<std::uint32_t>::max();

// Only called after integer_limits() established a positive 32-bit increment.
std::uint32_t increment(const IntegerFeature& feature) { return static_cast<std::uint32_t>(feature.increment); }

// Native ROI limits consumed by arithmetic. `sensor` is already 1..UINT32_MAX,
// so every later sum/difference of these values fits comfortably in int64.
Status integer_limits(const IntegerFeature& f, std::string_view name, std::int64_t sensor, bool is_offset) {
    const std::string feature(name);
    if (f.increment <= 0 || f.increment > uint32_max)
        return fail(ErrorCode::unsupported, feature + " advertises increment " + std::to_string(f.increment) +
                                                "; a positive 32-bit increment is required");
    if (f.minimum > f.maximum)
        return fail(ErrorCode::unsupported, feature + " advertises minimum " + std::to_string(f.minimum) +
                                                " above its maximum " + std::to_string(f.maximum));
    // A size grid origin may be at most the sensor extent; an offset origin
    // must leave room for at least one pixel.
    const std::int64_t origin_limit = is_offset ? sensor - 1 : sensor;
    if (f.minimum < 0 || f.minimum > origin_limit)
        return fail(ErrorCode::unsupported, feature + " advertises minimum " + std::to_string(f.minimum) +
                                                ", outside the supported grid origins [0, " + std::to_string(origin_limit) +
                                                "] for a sensor extent of " + std::to_string(sensor));
    if (f.minimum % f.increment != 0)
        return fail(ErrorCode::unsupported, feature + " minimum " + std::to_string(f.minimum) +
                                                " is not a multiple of its increment " + std::to_string(f.increment) +
                                                "; the capability contract cannot represent this grid");
    return {};
}

Status float_limits(const FloatFeature& f, std::string_view name, std::string_view unit, bool nanosecond_bounded) {
    const std::string feature(name);
    if (!std::isfinite(f.minimum) || !std::isfinite(f.maximum) || f.minimum < 0 || f.maximum < f.minimum ||
        (nanosecond_bounded && f.maximum * 1000.0 >= two_pow_63))
        return fail(ErrorCode::unsupported, feature + " advertises invalid or unrepresentable bounds [" + text(f.minimum) +
                                                ", " + text(f.maximum) + "] " + std::string(unit));
    if (f.increment && !(std::isfinite(*f.increment) && *f.increment > 0))
        return fail(ErrorCode::unsupported, feature + " advertises invalid increment " + text(*f.increment) + " " +
                                                std::string(unit) + "; a finite positive increment is required");
    return {};
}

Status current_axis(const IntegerFeature& size, const IntegerFeature& off, std::int64_t sensor, std::string_view size_name,
                    std::string_view off_name) {
    // Ordered so that no subtraction runs before its operands are bounded.
    if (size.value < 1 || size.value > sensor)
        return fail(ErrorCode::unsupported, std::string(size_name) + " reads back " + std::to_string(size.value) +
                                                ", outside [1, " + std::to_string(sensor) +
                                                "]; safe intermediate ROI writes cannot be established");
    if (off.value < 0 || off.value > sensor - size.value)
        return fail(ErrorCode::unsupported, std::string(off_name) + " reads back " + std::to_string(off.value) + " with " +
                                                std::string(size_name) + " " + std::to_string(size.value) +
                                                ", outside the sensor extent " + std::to_string(sensor) +
                                                "; safe intermediate ROI writes cannot be established");
    return {};
}

// Requires check_limits() and check_current() on the snapshot `feature` came
// from: value <= UINT32_MAX and minimum/upper/increment are bounded, so the
// subtraction and remainder below cannot overflow.
Status integer_request(std::int64_t value, const IntegerFeature& feature, std::int64_t upper, std::string_view name) {
    if (value < feature.minimum || value > upper)
        return fail(ErrorCode::invalid_argument, std::string(name) + " " + std::to_string(value) + " is outside [" +
                                                     std::to_string(feature.minimum) + ", " + std::to_string(upper) +
                                                     "]; nothing was written");
    if (feature.increment <= 0 || (value - feature.minimum) % feature.increment != 0)
        return fail(ErrorCode::invalid_argument, std::string(name) + " " + std::to_string(value) +
                                                     " is not on the native grid " + std::to_string(feature.minimum) +
                                                     " + k*" + std::to_string(feature.increment) + "; nothing was written");
    return {};
}

// `maximum_is_final` is false only where a maximum may legitimately depend on
// an earlier write of the same plan (see plan_writes); the SDK range check then
// enforces it at the write.
Status float_request(double value, const FloatFeature& feature, std::string_view name, std::string_view unit,
                     bool maximum_is_final) {
    if (!(value >= feature.minimum && (!maximum_is_final || value <= feature.maximum)))
        return fail(ErrorCode::invalid_argument, std::string(name) + " " + text(value) + " " + std::string(unit) +
                                                     " is outside the native bounds [" + text(feature.minimum) + ", " +
                                                     text(feature.maximum) + "]; nothing was written");
    if (feature.increment) {
        const double steps = (value - feature.minimum) / *feature.increment;
        if (!near_integer(steps, std::nearbyint(steps)))
            return fail(ErrorCode::invalid_argument, std::string(name) + " " + text(value) + " " + std::string(unit) +
                                                         " is not on the advertised increment " + text(*feature.increment) +
                                                         " from " + text(feature.minimum) + "; nothing was written");
    }
    return {};
}

} // namespace

Result<std::uint32_t> frame_rate_from_native(double hz, std::string_view feature) {
    if (!std::isfinite(hz) || hz < 0)
        return fail(ErrorCode::io_error, std::string(feature) + " returned an invalid native value " + text(hz) + " Hz");
    const double rounded = std::nearbyint(hz);
    if (rounded < 1 || rounded > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        !near_integer(hz, rounded))
        return fail(ErrorCode::unsupported, std::string(feature) + " reads back " + text(hz) +
                                                " Hz, which is not representable as a positive integer Hz; "
                                                "the value was not rounded");
    return static_cast<std::uint32_t>(rounded);
}

Result<std::chrono::nanoseconds> exposure_from_native(double microseconds, std::string_view feature) {
    if (!std::isfinite(microseconds) || microseconds < 0)
        return fail(ErrorCode::io_error,
                    std::string(feature) + " returned an invalid native value " + text(microseconds) + " us");
    const double ns = microseconds * 1000.0;
    const double rounded = std::nearbyint(ns);
    if (!(rounded < two_pow_63))
        return fail(ErrorCode::unsupported, std::string(feature) + " reads back " + text(microseconds) +
                                                " us, which overflows a 64-bit nanosecond count");
    if (rounded < 1 || !near_integer(ns, rounded))
        return fail(ErrorCode::unsupported, std::string(feature) + " reads back " + text(microseconds) +
                                                " us, which is not representable as a positive whole number of "
                                                "nanoseconds; the value was not rounded");
    return std::chrono::nanoseconds{static_cast<std::int64_t>(rounded)};
}

Result<double> frame_rate_to_native(std::uint32_t hz) {
    if (hz == 0) return fail(ErrorCode::invalid_argument, "frame rate must be at least 1 Hz; nothing was written");
    return static_cast<double>(hz); // exact for every uint32_t
}

Result<double> exposure_to_native(std::chrono::nanoseconds exposure) {
    if (exposure.count() <= 0)
        return fail(ErrorCode::invalid_argument, "exposure must be positive; nothing was written");
    // Larger counts (> ~104 days) could not be converted exactly to double.
    if (exposure.count() > (std::int64_t{1} << 53))
        return fail(ErrorCode::invalid_argument, "exposure " + std::to_string(exposure.count()) +
                                                     " ns cannot be passed exactly as microseconds; nothing was written");
    return static_cast<double>(exposure.count()) / 1000.0;
}


Status check_preconditions(const NativeSnapshot& n) {
    const auto unsupported = [](std::string message) { return fail(ErrorCode::unsupported, std::move(message)); };
    if (n.pixel_format != "Mono8")
        return unsupported("PixelFormat is '" + n.pixel_format + "'; profile " + std::string(profile_name) +
                           " requires the camera to be set to Mono8 beforehand (the adapter never changes it)");
    if (!n.mono8_available) return unsupported("PixelFormat entry Mono8 is not available");
    if (n.trigger_selector && *n.trigger_selector != "FrameStart")
        return unsupported("TriggerSelector is '" + *n.trigger_selector +
                           "'; the profile inspects FrameStart only and does not change the selector");
    if (n.trigger_mode && *n.trigger_mode != "Off")
        return unsupported("TriggerMode is '" + *n.trigger_mode + "'; the profile requires free-running "
                           "acquisition (TriggerMode Off), which the adapter does not set");
    if (n.exposure_auto && *n.exposure_auto != "Off")
        return unsupported("ExposureAuto is '" + *n.exposure_auto + "'; the profile requires manual exposure");
    if (n.frame_rate_enable && !*n.frame_rate_enable)
        return unsupported("AcquisitionFrameRateEnable is false; the profile requires an enabled frame-rate control");
    if (auto unit = unit_is(n.frame_rate_hz, frame_rate_feature, {"Hz", "fps"}); !unit) return unit;
    if (auto unit = unit_is(n.exposure_us, n.exposure_feature, {"us", "\xC2\xB5s"}); !unit) return unit;
    return {};
}

Status check_limits(const NativeSnapshot& n) {
    const auto sw = dimension(n.sensor_width, sensor_width_feature);
    if (!sw) return sw.error();
    const auto sh = dimension(n.sensor_height, sensor_height_feature);
    if (!sh) return sh.error();
    const struct { const IntegerFeature& feature; std::string_view name; std::int64_t sensor; bool is_offset; } rois[] = {
        {n.width, width_feature, n.sensor_width, false},
        {n.height, height_feature, n.sensor_height, false},
        {n.offset_x, offset_x_feature, n.sensor_width, true},
        {n.offset_y, offset_y_feature, n.sensor_height, true}};
    for (const auto& roi : rois)
        if (auto ok = integer_limits(roi.feature, roi.name, roi.sensor, roi.is_offset); !ok) return ok;
    if (auto ok = float_limits(n.frame_rate_hz, frame_rate_feature, "Hz", false); !ok) return ok;
    return float_limits(n.exposure_us, n.exposure_feature, "us", true);
}

Status check_current(const NativeSnapshot& n) {
    // check_limits() bounds the sensor extent first; without it the axis
    // checks below could compare against an unrepresentable extent.
    if (auto ok = dimension(n.sensor_width, sensor_width_feature); !ok) return ok.error();
    if (auto ok = dimension(n.sensor_height, sensor_height_feature); !ok) return ok.error();
    if (auto ok = current_axis(n.width, n.offset_x, n.sensor_width, width_feature, offset_x_feature); !ok) return ok;
    if (auto ok = current_axis(n.height, n.offset_y, n.sensor_height, height_feature, offset_y_feature); !ok) return ok;
    if (!std::isfinite(n.frame_rate_hz.value) || n.frame_rate_hz.value < 0)
        return fail(ErrorCode::io_error, std::string(frame_rate_feature) + " returned an invalid native value " +
                                             text(n.frame_rate_hz.value) + " Hz");
    if (!std::isfinite(n.exposure_us.value) || n.exposure_us.value < 0)
        return fail(ErrorCode::io_error,
                    n.exposure_feature + " returned an invalid native value " + text(n.exposure_us.value) + " us");
    return {};
}

Result<CameraCapabilities> capabilities_from(const NativeSnapshot& n) {
    if (auto ok = check_preconditions(n); !ok) return ok.error();
    if (auto ok = check_limits(n); !ok) return ok.error();
    CameraCapabilities caps;
    caps.sensor_width = static_cast<std::uint32_t>(n.sensor_width);
    caps.sensor_height = static_cast<std::uint32_t>(n.sensor_height);
    caps.width_increment = increment(n.width);
    caps.height_increment = increment(n.height);
    caps.offset_x_increment = increment(n.offset_x);
    caps.offset_y_increment = increment(n.offset_y);

    const auto& rate = n.frame_rate_hz;
    // Integer-Hz envelope inside the native bounds; not a claim that every
    // integer in it reads back exactly (see quantization policy).
    const double low = std::max(1.0, std::ceil(rate.minimum));
    const double high = std::min(std::floor(rate.maximum), static_cast<double>(std::numeric_limits<std::uint32_t>::max()));
    if (high < low)
        return fail(ErrorCode::unsupported, std::string(frame_rate_feature) + " bounds [" + text(rate.minimum) + ", " +
                                                text(rate.maximum) + "] Hz contain no integer rate");
    caps.minimum_frame_rate_hz = static_cast<std::uint32_t>(low);
    caps.maximum_frame_rate_hz = static_cast<std::uint32_t>(high);

    const auto& exposure = n.exposure_us; // bounds validated by check_limits()
    caps.minimum_exposure = std::chrono::nanoseconds{
        std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(exposure.minimum * 1000.0)))};
    caps.mono8 = true; // check_preconditions established Mono8 is current and available
    caps.external_trigger = false;
    caps.external_buffer_acquisition = false;
    return caps;
}

Result<CameraConfiguration> configuration_from(const NativeSnapshot& n) {
    CameraConfiguration c;
    const auto w = dimension(n.width.value, width_feature);
    const auto h = dimension(n.height.value, height_feature);
    const auto x = offset(n.offset_x.value, offset_x_feature);
    const auto y = offset(n.offset_y.value, offset_y_feature);
    for (const auto* r : {&w, &h, &x, &y})
        if (!*r) return r->error();
    const auto rate = frame_rate_from_native(n.frame_rate_hz.value);
    if (!rate) return rate.error();
    const auto exposure = exposure_from_native(n.exposure_us.value, n.exposure_feature);
    if (!exposure) return exposure.error();
    c.width = w.value();
    c.height = h.value();
    c.offset_x = x.value();
    c.offset_y = y.value();
    c.frame_rate_hz = rate.value();
    c.exposure = exposure.value();
    return c;
}

namespace {

// Fresh maximum of one ROI feature, as validated against the ORDERED plan.
// Supported coupling: a feature's maximum may depend on its axis companion,
// and only a smaller companion can raise it (a smaller offset can raise the
// size maximum; a smaller size can raise the offset maximum). The maximum is
// therefore deferred to the SDK range check ONLY when `relaxed` - an earlier
// write of this plan decreases the companion. Otherwise (first write on the
// axis, companion unchanged, increased, or written later) the fresh maximum
// is final and enforced before any write.
Status roi_maximum(std::int64_t requested, const IntegerFeature& feature, bool relaxed, std::string_view name,
                   std::string_view companion) {
    if (relaxed || requested <= feature.maximum) return {};
    return fail(ErrorCode::invalid_argument, std::string(name) + " " + std::to_string(requested) +
                                                 " is above the fresh native maximum " + std::to_string(feature.maximum) +
                                                 "; no earlier write of this request decreases " + std::string(companion) +
                                                 ", so the bound cannot be relaxed; nothing was written");
}

// One ROI axis. Final limits (fresh snapshot): the native minimum and
// increment grid, a positive size, the sensor extent, the coupled sum
// offset + size <= sensor, and each written feature's fresh maximum unless an
// earlier write of this plan decreases its companion (see roi_maximum). An
// unchanged feature is not written and its current value is not judged
// against new limits (truthful readback and recovery). The SDK range check
// stays enabled for deferred maxima and later native changes.
// Requires check_limits() and check_current() on the snapshot.
Status plan_axis(std::uint32_t size, std::uint32_t off, const IntegerFeature& size_feature,
                 const IntegerFeature& off_feature, std::int64_t sensor, std::string_view size_name,
                 std::string_view off_name, std::vector<NativeWrite>& writes) {
    const auto requested_size = static_cast<std::int64_t>(size);
    const auto requested_offset = static_cast<std::int64_t>(off);
    // Adapter contract: a ROI has at least one pixel, independent of a
    // permissive native minimum.
    if (requested_size < 1)
        return fail(ErrorCode::invalid_argument, std::string(size_name) + " 0 is not a positive ROI size; nothing was written");
    if (auto ok = integer_request(requested_size, size_feature, sensor, size_name); !ok) return ok;
    // 1 <= requested_size <= sensor <= UINT32_MAX: the difference is in [0, sensor).
    if (auto ok = integer_request(requested_offset, off_feature, sensor - requested_size, off_name); !ok) return ok;
    const NativeWrite size_write{std::string(size_name), false, requested_size, 0};
    const NativeWrite offset_write{std::string(off_name), false, requested_offset, 0};
    const bool size_changes = requested_size != size_feature.value;
    const bool offset_changes = requested_offset != off_feature.value;
    // Offset first is safe when the new offset fits with the CURRENT size;
    // otherwise the size necessarily shrinks and is written first. Either
    // order keeps every intermediate ROI inside the sensor. Overflow-free
    // form of requested_offset + current_size <= sensor: check_current()
    // bounded the current size to [1, sensor].
    const bool offset_first = requested_offset <= sensor - size_feature.value;
    if (size_changes) {
        const bool relaxed = offset_first && offset_changes && requested_offset < off_feature.value;
        if (auto ok = roi_maximum(requested_size, size_feature, relaxed, size_name, off_name); !ok) return ok;
    }
    if (offset_changes) {
        const bool relaxed = !offset_first && size_changes && requested_size < size_feature.value;
        if (auto ok = roi_maximum(requested_offset, off_feature, relaxed, off_name, size_name); !ok) return ok;
    }
    if (offset_first) {
        if (offset_changes) writes.push_back(offset_write);
        if (size_changes) writes.push_back(size_write);
    } else {
        if (size_changes) writes.push_back(size_write);
        if (offset_changes) writes.push_back(offset_write);
    }
    return {};
}

// Profile facts that define the published capabilities. A change is reported,
// not absorbed: the capability snapshot is immutable for the open device.
Status unchanged_profile(const NativeSnapshot& n, const NativeSnapshot& envelope) {
    const auto changed = [](std::string what, const std::string& then, const std::string& now) {
        return fail(ErrorCode::unsupported, what + " changed from " + then + " at open to " + now +
                                                "; the published capabilities no longer describe the camera; "
                                                "close and reopen the device; nothing was written");
    };
    if (n.sensor_width != envelope.sensor_width)
        return changed(std::string(sensor_width_feature), std::to_string(envelope.sensor_width), std::to_string(n.sensor_width));
    if (n.sensor_height != envelope.sensor_height)
        return changed(std::string(sensor_height_feature), std::to_string(envelope.sensor_height),
                       std::to_string(n.sensor_height));
    const struct { const IntegerFeature& now; const IntegerFeature& then; std::string_view name; } rois[] = {
        {n.width, envelope.width, width_feature},
        {n.height, envelope.height, height_feature},
        {n.offset_x, envelope.offset_x, offset_x_feature},
        {n.offset_y, envelope.offset_y, offset_y_feature}};
    for (const auto& roi : rois)
        if (roi.now.increment != roi.then.increment)
            return changed(std::string(roi.name) + " increment", std::to_string(roi.then.increment),
                           std::to_string(roi.now.increment));
    if (n.exposure_feature != envelope.exposure_feature)
        return changed("selected exposure feature", "'" + envelope.exposure_feature + "'", "'" + n.exposure_feature + "'");
    return {};
}

} // namespace

Result<std::vector<NativeWrite>> plan_writes(const CameraConfiguration& c, const NativeSnapshot& n,
                                             const NativeSnapshot& envelope) {
    // Structural safety of the fresh snapshot first: nothing below performs
    // arithmetic on an unchecked native fact.
    if (auto ok = check_preconditions(n); !ok) return ok.error();
    if (auto ok = check_limits(n); !ok) return ok.error();
    if (auto ok = check_current(n); !ok) return ok.error();
    if (auto ok = unchanged_profile(n, envelope); !ok) return ok.error();
    // Every limit below is the FRESH native readback.
    std::vector<NativeWrite> roi_x, roi_y, timing;
    if (auto ok = plan_axis(c.width, c.offset_x, n.width, n.offset_x, n.sensor_width, width_feature, offset_x_feature, roi_x);
        !ok)
        return ok.error();
    if (auto ok = plan_axis(c.height, c.offset_y, n.height, n.offset_y, n.sensor_height, height_feature, offset_y_feature,
                            roi_y);
        !ok)
        return ok.error();
    const auto rate = frame_rate_to_native(c.frame_rate_hz);
    if (!rate) return rate.error();
    const auto exposure = exposure_to_native(c.exposure);
    if (!exposure) return exposure.error();
    // Rate first only when the period lengthens; a longer period may raise the
    // native exposure maximum, which this profile then leaves to the SDK range
    // check at the exposure write. In every other order the fresh exposure
    // maximum is already final and is enforced before any write.
    const bool rate_first = rate.value() < n.frame_rate_hz.value;
    if (auto ok = float_request(rate.value(), n.frame_rate_hz, frame_rate_feature, "Hz", true); !ok) return ok.error();
    if (auto ok = float_request(exposure.value(), n.exposure_us, n.exposure_feature, "us", !rate_first); !ok)
        return ok.error();

    const NativeWrite rate_write{std::string(frame_rate_feature), true, 0, rate.value()};
    const NativeWrite exposure_write{n.exposure_feature, true, 0, exposure.value()};
    // Floats are always written (no exact-equality shortcut on native floats).
    if (rate_first) {
        timing = {rate_write, exposure_write};
    } else {
        timing = {exposure_write, rate_write};
    }
    std::vector<NativeWrite> plan;
    plan.reserve(roi_x.size() + roi_y.size() + timing.size());
    for (auto* group : {&roi_x, &roi_y, &timing})
        for (auto& write : *group) plan.push_back(std::move(write));
    return plan;
}

} // namespace gige_mel::backend::aravis_profile
