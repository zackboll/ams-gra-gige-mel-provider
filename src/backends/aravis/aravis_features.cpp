#include "aravis_features.hpp"
#include "aravis_native.hpp"

namespace gige_mel::backend::aravis::detail {
namespace {

ErrorCode translate_code(const GError& error) {
    if (error.domain == arv_device_error_quark()) {
        switch (error.code) {
        case ARV_DEVICE_ERROR_TIMEOUT: return ErrorCode::timeout;
        case ARV_DEVICE_ERROR_NOT_CONNECTED: return ErrorCode::disconnected;
        case ARV_DEVICE_ERROR_NOT_FOUND: return ErrorCode::not_found;
        case ARV_DEVICE_ERROR_NOT_CONTROLLER: return ErrorCode::busy;
        case ARV_DEVICE_ERROR_FEATURE_NOT_FOUND:
        case ARV_DEVICE_ERROR_WRONG_FEATURE: return ErrorCode::unsupported;
        default: return ErrorCode::io_error;
        }
    }
    if (error.domain == arv_gc_error_quark()) {
        switch (error.code) {
        case ARV_GC_ERROR_OUT_OF_RANGE:
        case ARV_GC_ERROR_ENUM_ENTRY_NOT_FOUND: return ErrorCode::invalid_argument;
        case ARV_GC_ERROR_NODE_NOT_FOUND:
        case ARV_GC_ERROR_READ_ONLY:
        case ARV_GC_ERROR_PROPERTY_NOT_DEFINED:
        case ARV_GC_ERROR_PVALUE_NOT_DEFINED: return ErrorCode::unsupported;
        default: return ErrorCode::io_error;
        }
    }
    return ErrorCode::io_error;
}

// ---------------------------------------------------------------------------
// Native feature access. Callers hold the device operation lock. Node pointers
// are borrowed from the device's GenICam tree and never escape a call.
Result<ArvGcFeatureNode*> feature_node(ArvDevice* device, std::string_view name, bool writable) {
    const std::string feature(name);
    ArvGcNode* node = arv_device_get_feature(device, feature.c_str());
    if (!node || !ARV_IS_GC_FEATURE_NODE(node))
        return fail(ErrorCode::unsupported, "required feature '" + feature + "' is not present");
    auto* feature_node = ARV_GC_FEATURE_NODE(node);
    ErrorSlot error;
    const bool implemented = arv_gc_feature_node_is_implemented(feature_node, error.out());
    if (error.failed()) return translate("query implementation of", feature, *error.take());
    const bool available = implemented && arv_gc_feature_node_is_available(feature_node, error.out());
    if (error.failed()) return translate("query availability of", feature, *error.take());
    if (!implemented || !available)
        return fail(ErrorCode::unsupported, "required feature '" + feature + "' is not implemented/available");
    const auto mode = arv_gc_feature_node_get_actual_access_mode(feature_node);
    const bool readable = mode == ARV_GC_ACCESS_MODE_RO || mode == ARV_GC_ACCESS_MODE_RW;
    if (!readable || (writable && mode != ARV_GC_ACCESS_MODE_RW))
        return fail(ErrorCode::unsupported, "feature '" + feature + "' has access mode " + std::to_string(static_cast<int>(mode)) +
                                                (writable ? "; read/write access is required" : "; read access is required"));
    return feature_node;
}

} // namespace

Error translate(std::string_view operation, std::string_view feature, const GError& error) {
    std::string message = "Aravis " + std::string(operation);
    if (!feature.empty()) message += " '" + std::string(feature) + "'";
    message += ": " + copy(error.message) + " [" + copy(g_quark_to_string(error.domain)) + " " +
               std::to_string(error.code) + "]";
    return fail(translate_code(error), std::move(message));
}

AravisDeviceFeatures::AravisDeviceFeatures(ArvDevice* device) : device_(device) {}

// False is confirmed absence/unimplemented. Availability, access, wrong
// node type and failed queries must never become optional absence.
Result<bool> AravisDeviceFeatures::implemented(std::string_view name) {
    const std::string feature(name);
    ArvGcNode* node = arv_device_get_feature(device_, feature.c_str());
    if (!node) return false;
    if (!ARV_IS_GC_FEATURE_NODE(node))
        return fail(ErrorCode::unsupported, "optional feature '" + feature + "' exists but is not a GenICam feature node");
    ErrorSlot error;
    const bool implemented = arv_gc_feature_node_is_implemented(ARV_GC_FEATURE_NODE(node), error.out());
    if (error.failed()) return translate("query implementation of optional feature", feature, *error.take());
    return implemented;
}

Result<std::int64_t> AravisDeviceFeatures::read_integer(std::string_view name) {
    const auto node = feature_node(device_, name, false);
    if (!node) return node.error();
    if (!ARV_IS_GC_INTEGER(node.value()))
        return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not an integer feature");
    ErrorSlot error;
    const gint64 value = arv_gc_integer_get_value(ARV_GC_INTEGER(node.value()), error.out());
    if (error.failed()) return translate("read", name, *error.take());
    return static_cast<std::int64_t>(value);
}

Result<device_features::IntegerFeature> AravisDeviceFeatures::read_integer_feature(std::string_view name,
    device_features::ReadAccess access) {
    const auto node = feature_node(device_, name, access == device_features::ReadAccess::read_write);
    if (!node) return node.error();
    if (!ARV_IS_GC_INTEGER(node.value()))
        return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not an integer feature");
    auto* integer = ARV_GC_INTEGER(node.value());
    device_features::IntegerFeature f;
    ErrorSlot error;
    f.value = arv_gc_integer_get_value(integer, error.out());
    if (!error.failed()) f.minimum = arv_gc_integer_get_min(integer, error.out());
    if (!error.failed()) f.maximum = arv_gc_integer_get_max(integer, error.out());
    if (!error.failed()) f.increment = arv_gc_integer_get_inc(integer, error.out());
    if (error.failed()) return translate("read value/bounds/increment of", name, *error.take());
    return f;
}

Result<device_features::FloatFeature> AravisDeviceFeatures::read_float_feature(std::string_view name,
    device_features::ReadAccess access) {
    const auto node = feature_node(device_, name, access == device_features::ReadAccess::read_write);
    if (!node) return node.error();
    if (!ARV_IS_GC_FLOAT(node.value()))
        return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not a float feature");
    auto* real = ARV_GC_FLOAT(node.value());
    device_features::FloatFeature f;
    ErrorSlot error;
    f.value = arv_gc_float_get_value(real, error.out());
    if (!error.failed()) f.minimum = arv_gc_float_get_min(real, error.out());
    if (!error.failed()) f.maximum = arv_gc_float_get_max(real, error.out());
    double increment = 0;
    if (!error.failed()) increment = arv_gc_float_get_inc(real, error.out());
    if (error.failed()) return translate("read value/bounds of", name, *error.take());
    // Aravis 0.8.36 (arv_gc_float_get_inc, arvgcfloat.c) documents exactly
    // G_MINDOUBLE (== DBL_MIN) as "not defined"; a failure is the GError
    // handled above. Only that value is absence. Every other successfully
    // returned value - including zero, negative and nonfinite ones from an
    // explicit <Inc> - is kept unchanged so check_limits() rejects it with
    // feature/value context instead of it silently becoming "no increment".
    if (increment != G_MINDOUBLE) f.increment = increment;
    if (const char* unit = arv_gc_float_get_unit(real); unit && *unit) f.unit = unit;
    return f;
}

Result<std::string> AravisDeviceFeatures::read_string_or_enum(std::string_view name) {
    const auto node = feature_node(device_, name, false);
    if (!node) return node.error();
    ErrorSlot error;
    const char* value = nullptr;
    if (ARV_IS_GC_ENUMERATION(node.value()))
        value = arv_gc_enumeration_get_string_value(ARV_GC_ENUMERATION(node.value()), error.out());
    else if (ARV_IS_GC_STRING(node.value()))
        value = arv_gc_string_get_value(ARV_GC_STRING(node.value()), error.out());
    else
        return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not an enumeration or string");
    if (error.failed()) return translate("read", name, *error.take());
    return copy(value); // copied before any later SDK call can invalidate it
}

Result<bool> AravisDeviceFeatures::read_boolean(std::string_view name) {
    const auto node = feature_node(device_, name, false);
    if (!node) return node.error();
    if (!ARV_IS_GC_BOOLEAN(node.value()))
        return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not a Boolean feature");
    ErrorSlot error;
    const bool value = arv_gc_boolean_get_value(ARV_GC_BOOLEAN(node.value()), error.out());
    if (error.failed()) return translate("read", name, *error.take());
    return value;
}

Result<bool> AravisDeviceFeatures::enum_entry_available(std::string_view name, std::string_view entry) {
    const std::string feature(name), selected(entry);
    ErrorSlot error;
    const bool available = arv_device_is_enumeration_entry_available(device_, feature.c_str(), selected.c_str(), error.out());
    if (error.failed()) return translate("query selected entry of", name, *error.take());
    return available;
}

// Exactly one native feature write through the GenICam node (no ArvCamera
// convenience setter, so no implicit trigger/auto/format changes). The
// per-device range/access policy rechecks constraints at each SDK write.
Status AravisDeviceFeatures::write_integer(std::string_view name, std::int64_t value) {
    const auto node = feature_node(device_, name, true);
    if (!node) return node.error();
    if (!ARV_IS_GC_INTEGER(node.value()))
        return fail(ErrorCode::unsupported, std::string(name) + " is not an integer feature");
    ErrorSlot error;
    arv_gc_integer_set_value(ARV_GC_INTEGER(node.value()), value, error.out());
    if (error.failed()) return translate("write", name, *error.take());
    return {};
}

Status AravisDeviceFeatures::write_float(std::string_view name, double value) {
    const auto node = feature_node(device_, name, true);
    if (!node) return node.error();
    if (!ARV_IS_GC_FLOAT(node.value()))
        return fail(ErrorCode::unsupported, std::string(name) + " is not a float feature");
    ErrorSlot error;
    arv_gc_float_set_value(ARV_GC_FLOAT(node.value()), value, error.out());
    if (error.failed()) return translate("write", name, *error.take());
    return {};
}

namespace {

// Diagnostic identity remains adapter-specific: some cameras expose integer
// identities and the native node type is part of the reported provenance.
Result<std::optional<std::string>> read_optional_string(ArvDevice* device, std::string_view name) {
    AravisDeviceFeatures features(device);
    const auto exists = features.implemented(name);
    if (!exists) return exists.error(); // a failed query is not absence
    if (!exists.value()) return std::optional<std::string>{};
    auto value = features.read_string_or_enum(name);
    if (!value) return value.error();
    return std::optional<std::string>{std::move(value).value()};
}

} // namespace

// GIGE-MEL-018: read-only identity text. String and enumeration nodes give
// their value; an Integer node (e.g. the Cognex CIC-5000R declares
// DeviceSerialNumber as an Integer SwissKnife) gives its exact decimal
// readback, labelled with its node type so it is never mistaken for a string
// identity. Absence and query failures follow read_optional_string().
Result<std::optional<std::string>> read_optional_identity(ArvDevice* device, std::string_view name) {
    AravisDeviceFeatures features(device);
    const auto exists = features.implemented(name);
    if (!exists) return exists.error();
    if (!exists.value()) return std::optional<std::string>{};
    ArvGcNode* node = arv_device_get_feature(device, std::string(name).c_str());
    if (node && ARV_IS_GC_INTEGER(node) && !ARV_IS_GC_ENUMERATION(node)) {
        const auto value = features.read_integer(name);
        if (!value) return value.error();
        return std::optional<std::string>{std::to_string(value.value()) + " (GenICam Integer node)"};
    }
    return read_optional_string(device, name);
}

} // namespace gige_mel::backend::aravis::detail
