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

// Representation-error tolerance: 4 ULP relative to the magnitude (at least 1).
// It cannot absorb a genuine fraction (30.000300003 Hz differs by ~1e-5 relative).
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

Result<std::uint32_t> increment(const IntegerFeature& feature, std::string_view name) {
    if (feature.increment <= 0 || feature.increment > std::numeric_limits<std::uint32_t>::max())
        return fail(ErrorCode::unsupported, std::string(name) + " advertises increment " +
                                                std::to_string(feature.increment) + "; a positive 32-bit increment is required");
    if (feature.minimum % feature.increment != 0)
        return fail(ErrorCode::unsupported, std::string(name) + " minimum " + std::to_string(feature.minimum) +
                                                " is not a multiple of its increment " + std::to_string(feature.increment) +
                                                "; the capability contract cannot represent this grid");
    return static_cast<std::uint32_t>(feature.increment);
}

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

Status float_request(double value, const FloatFeature& feature, std::string_view name, std::string_view unit) {
    if (!(value >= feature.minimum && value <= feature.maximum))
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

Result<CameraCapabilities> capabilities_from(const NativeSnapshot& n) {
    if (auto ok = check_preconditions(n); !ok) return ok.error();
    CameraCapabilities caps;
    const auto sw = dimension(n.sensor_width, sensor_width_feature);
    const auto sh = dimension(n.sensor_height, sensor_height_feature);
    const auto wi = increment(n.width, width_feature);
    const auto hi = increment(n.height, height_feature);
    const auto xi = increment(n.offset_x, offset_x_feature);
    const auto yi = increment(n.offset_y, offset_y_feature);
    for (const auto* r : {&sw, &sh, &wi, &hi, &xi, &yi})
        if (!*r) return r->error();
    caps.sensor_width = sw.value();
    caps.sensor_height = sh.value();
    caps.width_increment = wi.value();
    caps.height_increment = hi.value();
    caps.offset_x_increment = xi.value();
    caps.offset_y_increment = yi.value();

    const auto& rate = n.frame_rate_hz;
    if (!std::isfinite(rate.minimum) || !std::isfinite(rate.maximum) || rate.minimum < 0 || rate.maximum < rate.minimum)
        return fail(ErrorCode::unsupported, std::string(frame_rate_feature) + " advertises invalid bounds [" +
                                                text(rate.minimum) + ", " + text(rate.maximum) + "] Hz");
    // Integer-Hz envelope inside the native bounds; not a claim that every
    // integer in it reads back exactly (see quantization policy).
    const double low = std::max(1.0, std::ceil(rate.minimum));
    const double high = std::min(std::floor(rate.maximum), static_cast<double>(std::numeric_limits<std::uint32_t>::max()));
    if (high < low)
        return fail(ErrorCode::unsupported, std::string(frame_rate_feature) + " bounds [" + text(rate.minimum) + ", " +
                                                text(rate.maximum) + "] Hz contain no integer rate");
    caps.minimum_frame_rate_hz = static_cast<std::uint32_t>(low);
    caps.maximum_frame_rate_hz = static_cast<std::uint32_t>(high);

    const auto& exposure = n.exposure_us;
    if (!std::isfinite(exposure.minimum) || !std::isfinite(exposure.maximum) || exposure.minimum < 0 ||
        exposure.maximum < exposure.minimum || exposure.maximum * 1000.0 >= two_pow_63)
        return fail(ErrorCode::unsupported, n.exposure_feature + " advertises invalid or unrepresentable bounds [" +
                                                text(exposure.minimum) + ", " + text(exposure.maximum) + "] us");
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

// One ROI axis. Static limits: the native minimum and increment grid, the
// sensor extent, and the coupled sum offset + size <= sensor. The features'
// current maxima may depend on the other feature, so they are revalidated by
// the SDK at each write (range checking is enabled on the adapter's device).
Status plan_axis(std::uint32_t size, std::uint32_t off, const IntegerFeature& size_feature,
                 const IntegerFeature& off_feature, std::int64_t sensor, std::string_view size_name,
                 std::string_view off_name, std::vector<NativeWrite>& writes) {
    const auto requested_size = static_cast<std::int64_t>(size);
    const auto requested_offset = static_cast<std::int64_t>(off);
    if (auto ok = integer_request(requested_size, size_feature, sensor, size_name); !ok) return ok;
    if (auto ok = integer_request(requested_offset, off_feature, sensor - requested_size, off_name); !ok) return ok;
    const NativeWrite size_write{std::string(size_name), false, requested_size, 0};
    const NativeWrite offset_write{std::string(off_name), false, requested_offset, 0};
    const bool size_changes = requested_size != size_feature.value;
    const bool offset_changes = requested_offset != off_feature.value;
    // Offset first is safe when the new offset fits with the CURRENT size;
    // otherwise the size necessarily shrinks and is written first. Either
    // order keeps every intermediate ROI inside the sensor.
    if (requested_offset + size_feature.value <= sensor) {
        if (offset_changes) writes.push_back(offset_write);
        if (size_changes) writes.push_back(size_write);
    } else {
        if (size_changes) writes.push_back(size_write);
        if (offset_changes) writes.push_back(offset_write);
    }
    return {};
}

} // namespace

Result<std::vector<NativeWrite>> plan_writes(const CameraConfiguration& c, const NativeSnapshot& n,
                                             const NativeSnapshot& envelope) {
    if (auto ok = check_preconditions(n); !ok) return ok.error();
    // Static grid facts come from the validated open-time envelope; present
    // values come from the current readback.
    auto width = envelope.width, height = envelope.height, off_x = envelope.offset_x, off_y = envelope.offset_y;
    width.value = n.width.value;
    height.value = n.height.value;
    off_x.value = n.offset_x.value;
    off_y.value = n.offset_y.value;
    std::vector<NativeWrite> roi_x, roi_y, timing;
    if (auto ok = plan_axis(c.width, c.offset_x, width, off_x, envelope.sensor_width, width_feature, offset_x_feature, roi_x); !ok)
        return ok.error();
    if (auto ok = plan_axis(c.height, c.offset_y, height, off_y, envelope.sensor_height, height_feature, offset_y_feature, roi_y); !ok)
        return ok.error();
    const auto rate = frame_rate_to_native(c.frame_rate_hz);
    if (!rate) return rate.error();
    const auto exposure = exposure_to_native(c.exposure);
    if (!exposure) return exposure.error();
    if (auto ok = float_request(rate.value(), envelope.frame_rate_hz, frame_rate_feature, "Hz"); !ok) return ok.error();
    if (auto ok = float_request(exposure.value(), envelope.exposure_us, n.exposure_feature, "us"); !ok) return ok.error();

    const NativeWrite rate_write{std::string(frame_rate_feature), true, 0, rate.value()};
    const NativeWrite exposure_write{n.exposure_feature, true, 0, exposure.value()};
    // Floats are always written (no exact-equality shortcut on native floats).
    if (rate.value() < n.frame_rate_hz.value) { // period lengthens: rate first
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
