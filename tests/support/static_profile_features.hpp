#pragma once

#include "device_features.hpp"

#include <map>
#include <string>

// TEST ONLY in-memory feature service sufficient for the shared monochrome
// admission policy. Fixed values, no SDK, no device, no simulation of writes
// beyond storing the requested value. Not a model of any physical camera.
namespace profile_navigation_fixture {
namespace b = gige_mel::backend;
namespace f = b::device_features;

class StaticFeatures final : public f::IDeviceFeatures {
public:
    b::Result<bool> implemented(std::string_view name) override {
        return integers_.contains(name) || floats_.contains(name) || strings_.contains(name);
    }
    b::Result<std::int64_t> read_integer(std::string_view name) override {
        const auto found = integers_.find(name);
        if (found == integers_.end()) return missing(name);
        return found->second.value;
    }
    b::Result<f::IntegerFeature> read_integer_feature(std::string_view name, f::ReadAccess) override {
        const auto found = integers_.find(name);
        if (found == integers_.end()) return missing(name);
        return found->second;
    }
    b::Result<f::FloatFeature> read_float_feature(std::string_view name, f::ReadAccess) override {
        const auto found = floats_.find(name);
        if (found == floats_.end()) return missing(name);
        return found->second;
    }
    b::Result<std::string> read_string_or_enum(std::string_view name) override {
        const auto found = strings_.find(name);
        if (found == strings_.end()) return missing(name);
        return found->second;
    }
    b::Result<bool> read_boolean(std::string_view name) override { return missing(name); }
    b::Result<bool> enum_entry_available(std::string_view name, std::string_view entry) override {
        const auto found = strings_.find(name);
        if (found == strings_.end()) return missing(name);
        return found->second == entry;
    }
    b::Status write_integer(std::string_view name, std::int64_t value) override {
        const auto found = integers_.find(name);
        if (found == integers_.end()) return missing(name);
        found->second.value = value;
        return {};
    }
    b::Status write_float(std::string_view name, double value) override {
        const auto found = floats_.find(name);
        if (found == floats_.end()) return missing(name);
        found->second.value = value;
        return {};
    }
private:
    static b::Error missing(std::string_view name) {
        return {b::ErrorCode::unsupported, "static test feature not implemented: " + std::string(name)};
    }
    std::map<std::string, f::IntegerFeature, std::less<>> integers_{
        {"SensorWidth", {640, 640, 640, 1}}, {"SensorHeight", {480, 480, 480, 1}},
        {"Width", {64, 1, 640, 1}}, {"Height", {48, 1, 480, 1}},
        {"OffsetX", {0, 0, 639, 1}}, {"OffsetY", {0, 0, 479, 1}}};
    std::map<std::string, f::FloatFeature, std::less<>> floats_{
        {"AcquisitionFrameRate", {20.0, 1.0, 20.0, std::nullopt, std::string("Hz")}},
        {"ExposureTime", {1000.0, 10.0, 40000.0, std::nullopt, std::string("us")}}};
    std::map<std::string, std::string, std::less<>> strings_{
        {"PixelFormat", "Mono8"}, {"TriggerSelector", "FrameStart"}, {"TriggerMode", "Off"},
        {"AcquisitionMode", "Continuous"}};
};
} // namespace profile_navigation_fixture
