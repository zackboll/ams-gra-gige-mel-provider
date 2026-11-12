#include "mono_free_running_device.hpp"

#include <utility>

namespace gige_mel::backend::profiles::mono_free_running {
namespace {

using device_features::IDeviceFeatures;
using device_features::ReadAccess;

Error fail(ErrorCode code, std::string message) { return Error{code, std::move(message)}; }

Result<std::optional<std::string>> read_optional_string(IDeviceFeatures& features, std::string_view name) {
    const auto exists = features.implemented(name);
    if (!exists) return exists.error(); // a failed query is not absence
    if (!exists.value()) return std::optional<std::string>{};
    auto value = features.read_string_or_enum(name);
    if (!value) return value.error();
    return std::optional<std::string>{std::move(value).value()};
}

Result<std::optional<bool>> read_optional_boolean(IDeviceFeatures& features, std::string_view name) {
    const auto exists = features.implemented(name);
    if (!exists) return exists.error();
    if (!exists.value()) return std::optional<bool>{};
    const auto value = features.read_boolean(name);
    if (!value) return value.error();
    return std::optional<bool>{value.value()};
}

// The first IMPLEMENTED name wins. An unusable selected feature or a failed
// implementation query never falls through to the next alias.
Result<std::string> exposure_feature_name(IDeviceFeatures& features) {
    for (const auto name : exposure_features) {
        const auto exists = features.implemented(name);
        if (!exists) return exists.error();
        if (exists.value()) return std::string(name);
    }
    return fail(ErrorCode::unsupported, "neither ExposureTime nor ExposureTimeAbs is implemented");
}

Result<std::string> frame_rate_feature_name(IDeviceFeatures& features) {
    for (const auto name : frame_rate_features) {
        const auto exists = features.implemented(name);
        if (!exists) return exists.error();
        if (exists.value()) return std::string(name);
    }
    return fail(ErrorCode::unsupported, "neither AcquisitionFrameRate nor AcquisitionFrameRateAbs is implemented");
}

Status write_feature(IDeviceFeatures& features, const NativeWrite& write) {
    return write.is_float ? features.write_float(write.feature, write.real)
                          : features.write_integer(write.feature, write.integer);
}

std::string describe(const NativeWrite& write) {
    return write.feature + "=" + (write.is_float ? std::to_string(write.real) : std::to_string(write.integer));
}

std::string text(const CameraConfiguration& c) {
    return std::to_string(c.width) + "x" + std::to_string(c.height) + "@(" + std::to_string(c.offset_x) + "," +
           std::to_string(c.offset_y) + ") " + std::to_string(c.frame_rate_hz) + " Hz " +
           std::to_string(c.exposure.count()) + " ns";
}

} // namespace

Result<NativeSnapshot> read_snapshot(device_features::IDeviceFeatures& features) {
    NativeSnapshot n;
    const auto sw = features.read_integer(sensor_width_feature);
    if (!sw) return sw.error();
    const auto sh = features.read_integer(sensor_height_feature);
    if (!sh) return sh.error();
    n.sensor_width = sw.value();
    n.sensor_height = sh.value();
    struct { std::string_view name; IntegerFeature* target; } integers[] = {
        {width_feature, &n.width}, {height_feature, &n.height},
        {offset_x_feature, &n.offset_x}, {offset_y_feature, &n.offset_y}};
    for (const auto& [name, target] : integers) {
        auto value = features.read_integer_feature(name, ReadAccess::read_write);
        if (!value) return value.error();
        *target = value.value();
    }
    auto exposure_name = exposure_feature_name(features);
    if (!exposure_name) return exposure_name.error();
    n.exposure_feature = std::move(exposure_name).value();
    auto rate_name = frame_rate_feature_name(features);
    if (!rate_name) return rate_name.error();
    n.frame_rate_feature = std::move(rate_name).value();
    auto rate = features.read_float_feature(n.frame_rate_feature, ReadAccess::read_write);
    if (!rate) return rate.error();
    n.frame_rate_hz = std::move(rate).value();
    auto exposure = features.read_float_feature(n.exposure_feature, ReadAccess::read_write);
    if (!exposure) return exposure.error();
    n.exposure_us = std::move(exposure).value();

    auto pixel = features.read_string_or_enum("PixelFormat");
    if (!pixel) return pixel.error();
    n.pixel_format = std::move(pixel).value();
    const auto available = features.enum_entry_available("PixelFormat", n.pixel_format);
    if (!available) return available.error();
    n.mono8_available = n.pixel_format == "Mono8" && available.value();
    n.mono12_available = n.pixel_format == "Mono12" && available.value();

    struct { std::string_view name; std::optional<std::string>* target; } optional_strings[] = {
        {"TriggerSelector", &n.trigger_selector}, {"TriggerMode", &n.trigger_mode}, {"ExposureAuto", &n.exposure_auto},
        {"DeviceStreamChannelEndianness", &n.pixel_endianness}};
    for (const auto& [name, target] : optional_strings) {
        auto value = read_optional_string(features, name);
        if (!value) return value.error();
        *target = std::move(value).value();
    }
    auto enable = read_optional_boolean(features, "AcquisitionFrameRateEnable");
    if (!enable) return enable.error();
    n.frame_rate_enable = enable.value();
    return n;
}

Result<CameraConfiguration> read_configuration(device_features::IDeviceFeatures& features,
                                               const NativeSnapshot& envelope,
                                               const ConnectionCheck& check_connection) {
    if (auto connected = check_connection(); !connected) return connected.error();
    auto native = read_snapshot(features);
    if (auto connected = check_connection(); !connected) return connected.error();
    if (!native) return native.error();
    if (auto ok = check_preconditions(native.value()); !ok) return ok.error();
    if (native.value().pixel_format != envelope.pixel_format ||
        native.value().pixel_endianness != envelope.pixel_endianness)
        return fail(ErrorCode::unsupported, "PixelFormat changed since open; close and reopen explicitly");
    return configuration_from(native.value());
}

Result<CameraConfiguration> apply_configuration(device_features::IDeviceFeatures& features,
                                                const CameraConfiguration& request,
                                                const NativeSnapshot& envelope,
                                                const ConnectionCheck& check_connection,
                                                const BeforeWrite& before_write) {
    auto current = read_snapshot(features);
    if (!current) return current.error();
    // Requires the stopped/manual profile; nothing is normalized.
    auto plan = plan_writes(request, current.value(), envelope);
    if (!plan) return plan.error();
    std::string written;
    for (const auto& write : plan.value()) {
        if (auto ready = before_write(write.feature); !ready) return ready.error();
        if (auto ok = write_feature(features, write); !ok) {
            // Preserve the original failure; report earlier writes and the
            // actual native state. No rollback is attempted or claimed.
            auto message = ok.error().message + "; earlier successful writes: " +
                           (written.empty() ? std::string("none") : written) + "; no rollback was attempted";
            const auto actual = read_configuration(features, envelope, check_connection);
            message += actual ? "; actual native configuration: " + text(actual.value())
                              : "; actual native configuration unreadable or unrepresentable: " + actual.error().message;
            return fail(ok.error().code, std::move(message));
        }
        written += (written.empty() ? "" : ", ") + describe(write);
    }
    auto actual = read_configuration(features, envelope, check_connection);
    if (!actual)
        return fail(actual.error().code, actual.error().message + " (after writes: " + written + ")");
    return actual; // may differ from the request through native quantization
}

} // namespace gige_mel::backend::profiles::mono_free_running
