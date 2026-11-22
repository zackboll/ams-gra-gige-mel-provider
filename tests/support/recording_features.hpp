#pragma once

#include "device_features.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// SDK-free typed facts, live constraints and call recording only. No SDK,
// acquisition or physical-device simulation.
namespace recording_features_fixture {
namespace backend = gige_mel::backend;
namespace features = backend::device_features;

enum class Kind { integer, real, string, enumeration, boolean, non_feature };
enum class Access { none, read_only, write_only, read_write };

struct Node {
    Kind kind{Kind::integer};
    Access access{Access::read_write};
    bool implemented{true}, available{true};
    features::IntegerFeature integer;
    features::FloatFeature real;
    std::string text;
    bool boolean{true};
    std::map<std::string, bool, std::less<>> entries;
};

inline Node integer_node(std::int64_t value, std::int64_t minimum, std::int64_t maximum, std::int64_t increment = 1,
                  Access access = Access::read_write) {
    Node node;
    node.access = access;
    node.integer = {value, minimum, maximum, increment};
    return node;
}

inline Node float_node(double value, double minimum, double maximum, std::string unit) {
    Node node;
    node.kind = Kind::real;
    node.real = {value, minimum, maximum, std::nullopt, std::move(unit)};
    return node;
}

inline Node text_node(std::string value, Kind kind = Kind::enumeration) {
    Node node;
    node.kind = kind;
    node.text = std::move(value);
    node.entries[node.text] = true;
    return node;
}

inline Node boolean_node(bool value) {
    Node node;
    node.kind = Kind::boolean;
    node.boolean = value;
    return node;
}

struct Call {
    std::string operation, feature, entry;
    features::ReadAccess access{features::ReadAccess::readable};
    std::int64_t integer{0};
    double real{0};
    bool is_write() const { return operation == "write_integer" || operation == "write_float"; }
};

inline Call query(std::string_view operation, std::string_view feature) {
    Call call;
    call.operation = operation;
    call.feature = feature;
    return call;
}

class RecordingFeatures final : public features::IDeviceFeatures {
public:
    std::map<std::string, Node, std::less<>> nodes;
    std::vector<Call> calls;
    std::function<std::optional<backend::Error>(RecordingFeatures&, const Call&)> on_call;
    std::function<void(RecordingFeatures&, const Call&)> after_write;
    bool coupled_roi{false};

    backend::Result<bool> implemented(std::string_view name) override {
        if (auto error = record(query("implemented", name))) return *error;
        const auto found = nodes.find(name);
        if (found == nodes.end()) return false;
        if (found->second.kind == Kind::non_feature) return rejected(name, "is not a feature");
        return found->second.implemented;
    }

    backend::Result<std::int64_t> read_integer(std::string_view name) override {
        if (auto error = record(query("read_integer", name))) return *error;
        auto node = checked(name, Kind::integer, features::ReadAccess::readable);
        if (!node) return node.error();
        return node.value()->integer.value;
    }

    backend::Result<features::IntegerFeature> read_integer_feature(std::string_view name,
                                                                  features::ReadAccess access) override {
        auto call = query("read_integer_feature", name);
        call.access = access;
        if (auto error = record(call)) return *error;
        auto node = checked(name, Kind::integer, access);
        if (!node) return node.error();
        return node.value()->integer;
    }

    backend::Result<features::FloatFeature> read_float_feature(std::string_view name,
                                                              features::ReadAccess access) override {
        auto call = query("read_float_feature", name);
        call.access = access;
        if (auto error = record(call)) return *error;
        auto node = checked(name, Kind::real, access);
        if (!node) return node.error();
        return node.value()->real;
    }

    backend::Result<std::string> read_string_or_enum(std::string_view name) override {
        if (auto error = record(query("read_string_or_enum", name))) return *error;
        auto node = accessible(name, features::ReadAccess::readable);
        if (!node) return node.error();
        if (node.value()->kind != Kind::string && node.value()->kind != Kind::enumeration)
            return rejected(name, "has wrong string/enumeration type");
        return node.value()->text;
    }

    backend::Result<bool> read_boolean(std::string_view name) override {
        if (auto error = record(query("read_boolean", name))) return *error;
        auto node = checked(name, Kind::boolean, features::ReadAccess::readable);
        if (!node) return node.error();
        return node.value()->boolean;
    }

    backend::Result<bool> enum_entry_available(std::string_view name, std::string_view entry) override {
        auto call = query("enum_entry_available", name);
        call.entry = entry;
        if (auto error = record(call)) return *error;
        auto node = checked(name, Kind::enumeration, features::ReadAccess::readable);
        if (!node) return node.error();
        const auto found = node.value()->entries.find(entry);
        return found != node.value()->entries.end() && found->second;
    }

    backend::Status write_integer(std::string_view name, std::int64_t value) override {
        auto call = query("write_integer", name);
        call.integer = value;
        if (auto error = record(call)) return *error;
        auto node = checked(name, Kind::integer, features::ReadAccess::read_write);
        if (!node) return node.error();
        const auto& bounds = node.value()->integer;
        if (value < bounds.minimum || value > bounds.maximum)
            return backend::Error{backend::ErrorCode::invalid_argument, std::string(name) + " violates live integer bounds"};
        node.value()->integer.value = value;
        if (coupled_roi) update_roi_bounds();
        if (after_write) after_write(*this, call);
        return {};
    }

    backend::Status write_float(std::string_view name, double value) override {
        auto call = query("write_float", name);
        call.real = value;
        if (auto error = record(call)) return *error;
        auto node = checked(name, Kind::real, features::ReadAccess::read_write);
        if (!node) return node.error();
        const auto& bounds = node.value()->real;
        if (value < bounds.minimum || value > bounds.maximum)
            return backend::Error{backend::ErrorCode::invalid_argument, std::string(name) + " violates live float bounds"};
        node.value()->real.value = value;
        if (after_write) after_write(*this, call);
        return {};
    }

    void update_roi_bounds() {
        nodes.at("Width").integer.maximum = nodes.at("SensorWidth").integer.value - nodes.at("OffsetX").integer.value;
        nodes.at("OffsetX").integer.maximum = nodes.at("SensorWidth").integer.value - nodes.at("Width").integer.value;
        nodes.at("Height").integer.maximum = nodes.at("SensorHeight").integer.value - nodes.at("OffsetY").integer.value;
        nodes.at("OffsetY").integer.maximum = nodes.at("SensorHeight").integer.value - nodes.at("Height").integer.value;
    }

    std::size_t count(std::string_view operation, std::string_view feature) const {
        return static_cast<std::size_t>(std::count_if(calls.begin(), calls.end(), [&](const auto& call) {
            return call.operation == operation && call.feature == feature;
        }));
    }

    std::vector<std::string> writes() const {
        std::vector<std::string> result;
        for (const auto& call : calls) if (call.is_write()) result.push_back(call.feature);
        return result;
    }

private:
    static backend::Error rejected(std::string_view name, std::string_view reason) {
        return {backend::ErrorCode::unsupported, "recording feature '" + std::string(name) + "' " + std::string(reason)};
    }

    std::optional<backend::Error> record(Call call) {
        calls.push_back(std::move(call));
        return on_call ? on_call(*this, calls.back()) : std::nullopt;
    }

    backend::Result<Node*> accessible(std::string_view name, features::ReadAccess access) {
        const auto found = nodes.find(name);
        if (found == nodes.end() || !found->second.implemented) return rejected(name, "is not implemented");
        auto& node = found->second;
        if (!node.available) return rejected(name, "is unavailable");
        if (node.access != Access::read_only && node.access != Access::read_write)
            return rejected(name, "is not readable");
        if (access == features::ReadAccess::read_write && node.access != Access::read_write)
            return rejected(name, "requires read/write access");
        return &node;
    }

    backend::Result<Node*> checked(std::string_view name, Kind kind, features::ReadAccess access) {
        auto node = accessible(name, access);
        if (!node) return node.error();
        if (node.value()->kind != kind) return rejected(name, "has wrong type");
        return node;
    }
};

inline RecordingFeatures baseline() {
    RecordingFeatures device;
    device.nodes["SensorWidth"] = integer_node(2048, 1, 2048, 1, Access::read_only);
    device.nodes["SensorHeight"] = integer_node(2048, 1, 2048, 1, Access::read_only);
    device.nodes["Width"] = integer_node(512, 1, 2048);
    device.nodes["Height"] = integer_node(512, 1, 2048);
    device.nodes["OffsetX"] = integer_node(0, 0, 2048);
    device.nodes["OffsetY"] = integer_node(0, 0, 2048);
    device.nodes["AcquisitionFrameRate"] = float_node(25.0, 0.1, 1000.0, "Hz");
    device.nodes["ExposureTimeAbs"] = float_node(10000.0, 10.0, 1e7, "us");
    device.nodes["PixelFormat"] = text_node("Mono8");
    device.nodes["TriggerSelector"] = text_node("FrameStart");
    device.nodes["TriggerMode"] = text_node("Off");
    return device;
}

// Typed fixtures reproduce documented values, not physical qualification.
inline RecordingFeatures cognex_features() {
    auto f = baseline();
    f.nodes["DeviceVendorName"] = text_node("COGNEX", Kind::string);
    f.nodes["DeviceModelName"] = text_node("CIC-5000R", Kind::string);
    f.nodes["DeviceFirmwareVersion"] = text_node("106349-08;U;CIC-5000R;V3.7-1;0", Kind::string);
    for (auto name : {"DeviceVendorName", "DeviceModelName", "DeviceFirmwareVersion"})
        f.nodes[name].access = Access::read_only;
    f.nodes["SensorWidth"] = integer_node(2592, 2592, 2592, 1, Access::read_only);
    f.nodes["SensorHeight"] = integer_node(1944, 1944, 1944, 1, Access::read_only);
    f.nodes["Width"] = integer_node(2592, 64, 2592);
    f.nodes["Height"] = integer_node(1944, 64, 1944);
    f.nodes["OffsetX"] = integer_node(0, 0, 0);
    f.nodes["OffsetY"] = integer_node(0, 0, 0);
    f.nodes.erase("AcquisitionFrameRate");
    f.nodes["AcquisitionFrameRateAbs"] = float_node(10, 0.0149, 1'000'000, "Hz");
    f.nodes["ExposureTimeAbs"] = float_node(35'000, 35, 999'985, "us");
    f.nodes["AcquisitionFrameRateEnable"] = boolean_node(true); // explicit fixture arrangement, not power-on state
    f.nodes["AcquisitionMode"] = text_node("Continuous");
    f.nodes["ExposureAuto"] = text_node("Off");
    return f;
}

} // namespace recording_features_fixture
