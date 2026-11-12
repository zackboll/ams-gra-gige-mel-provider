// SDK-free tests of the shared profile's device-service orchestration. The
// recording service models only typed feature facts and the live constraints
// used below. It does not simulate an SDK, acquisition or physical hardware.
#include "device_features.hpp"
#include "profiles/mono_free_running_device.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace backend = gige_mel::backend;
namespace features = backend::device_features;
namespace profile = backend::profiles::mono_free_running;
using namespace std::chrono_literals;

namespace {

#define CHECK(expression)                                                                                 \
    do {                                                                                                  \
        if (!(expression))                                                                                \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #expression); \
    } while (false)

bool contains(const std::string& value, std::string_view part) { return value.find(part) != std::string::npos; }

template <class T>
void expect_error(const backend::Result<T>& result, backend::ErrorCode code, std::string_view part) {
    CHECK(!result);
    CHECK(result.error().code == code);
    if (!contains(result.error().message, part))
        throw std::runtime_error("expected '" + std::string(part) + "' in: " + result.error().message);
}

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

Node integer_node(std::int64_t value, std::int64_t minimum, std::int64_t maximum, std::int64_t increment = 1,
                  Access access = Access::read_write) {
    Node node;
    node.access = access;
    node.integer = {value, minimum, maximum, increment};
    return node;
}

Node float_node(double value, double minimum, double maximum, std::string unit) {
    Node node;
    node.kind = Kind::real;
    node.real = {value, minimum, maximum, std::nullopt, std::move(unit)};
    return node;
}

Node text_node(std::string value, Kind kind = Kind::enumeration) {
    Node node;
    node.kind = kind;
    node.text = std::move(value);
    node.entries[node.text] = true;
    return node;
}

Node boolean_node(bool value) {
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

Call query(std::string_view operation, std::string_view feature) {
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

RecordingFeatures baseline() {
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

const backend::CameraConfiguration initial{512, 512, 0, 0, 25, 10ms};
const profile::ConnectionCheck connected = []() -> backend::Status { return {}; };
const profile::BeforeWrite allow_write = [](std::string_view) -> backend::Status { return {}; };

profile::NativeSnapshot envelope(RecordingFeatures& device) {
    auto result = profile::read_snapshot(device);
    if (!result) throw std::runtime_error("fixture snapshot: " + result.error().message);
    device.calls.clear();
    return std::move(result).value();
}

backend::Result<backend::CameraConfiguration> apply(RecordingFeatures& device,
                                                   const backend::CameraConfiguration& request,
                                                   const profile::NativeSnapshot& opened) {
    return profile::apply_configuration(device, request, opened, connected, allow_write);
}

void typed_snapshot_and_metadata() {
    auto device = baseline();
    device.nodes["SensorHeight"].integer.value = 1536;
    device.nodes["Width"].integer = {480, 16, 2000, 16};
    device.nodes["Height"].integer = {320, 8, 1504, 8};
    device.nodes["OffsetX"].integer = {32, 0, 1568, 4};
    device.nodes["OffsetY"].integer = {48, 0, 1216, 8};
    device.nodes["AcquisitionFrameRate"].real = {37, 1, 73, 1, "Hz"};
    device.nodes["ExposureTimeAbs"].real = {1234.5, 0.5, 50000, 0.25, "us"};
    const auto result = profile::read_snapshot(device);
    CHECK(result);
    const auto& value = result.value();
    CHECK(value.sensor_width == 2048 && value.sensor_height == 1536);
    CHECK(value.width.value == 480 && value.width.minimum == 16 && value.width.maximum == 2000 && value.width.increment == 16);
    CHECK(value.height.value == 320 && value.height.minimum == 8 && value.height.maximum == 1504 && value.height.increment == 8);
    CHECK(value.offset_x.value == 32 && value.offset_x.maximum == 1568 && value.offset_x.increment == 4);
    CHECK(value.offset_y.value == 48 && value.offset_y.maximum == 1216 && value.offset_y.increment == 8);
    CHECK(value.frame_rate_hz.value == 37 && value.frame_rate_hz.minimum == 1 && value.frame_rate_hz.maximum == 73);
    CHECK(value.frame_rate_hz.increment == 1 && value.frame_rate_hz.unit == "Hz");
    CHECK(value.exposure_us.value == 1234.5 && value.exposure_us.minimum == 0.5 && value.exposure_us.maximum == 50000);
    CHECK(value.exposure_us.increment == 0.25 && value.exposure_us.unit == "us");
    CHECK(value.frame_rate_feature == "AcquisitionFrameRate" && value.exposure_feature == "ExposureTimeAbs");
    CHECK(device.count("read_integer", "SensorWidth") == 1 && device.count("read_integer", "SensorHeight") == 1);
    for (const auto& call : device.calls)
        if (call.operation == "read_integer_feature" || call.operation == "read_float_feature")
            CHECK(call.access == features::ReadAccess::read_write);
    CHECK(device.writes().empty());

    for (const auto access : {Access::read_only, Access::write_only, Access::none}) {
        auto restricted = baseline();
        restricted.nodes["Width"].access = access;
        expect_error(profile::read_snapshot(restricted), backend::ErrorCode::unsupported, "Width");
        CHECK(restricted.writes().empty());
    }
    auto wrong = baseline();
    wrong.nodes["Width"].kind = Kind::real;
    expect_error(profile::read_snapshot(wrong), backend::ErrorCode::unsupported, "wrong type");
    auto failed = baseline();
    failed.on_call = [](auto&, const Call& call) -> std::optional<backend::Error> {
        if (call.operation == "read_float_feature" && call.feature == "ExposureTimeAbs")
            return backend::Error{backend::ErrorCode::timeout, "ExposureTimeAbs bounds query timed out"};
        return std::nullopt;
    };
    expect_error(profile::read_snapshot(failed), backend::ErrorCode::timeout, "bounds query timed out");
    CHECK(failed.writes().empty());

    for (const auto bad : {0.0, -5.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        auto malformed = baseline();
        const auto opened = envelope(malformed);
        malformed.nodes["ExposureTimeAbs"].real.increment = bad;
        const auto snapshot = profile::read_snapshot(malformed);
        CHECK(snapshot && snapshot.value().exposure_us.increment.has_value());
        CHECK(std::isnan(bad) ? std::isnan(*snapshot.value().exposure_us.increment) : *snapshot.value().exposure_us.increment == bad);
        expect_error(apply(malformed, initial, opened), backend::ErrorCode::unsupported, "invalid increment");
        CHECK(malformed.writes().empty());
    }
}

void optional_absence_and_errors() {
    for (const std::string name : {"TriggerMode", "ExposureAuto", "AcquisitionFrameRateEnable"}) {
        const bool boolean = name == "AcquisitionFrameRateEnable";
        const auto populate = [&](RecordingFeatures& device) {
            device.nodes[name] = boolean ? boolean_node(true) : text_node("Off");
        };
        for (const bool unimplemented : {false, true}) {
            auto device = baseline();
            populate(device);
            if (unimplemented) device.nodes[name].implemented = false;
            else device.nodes.erase(name);
            const auto snapshot = profile::read_snapshot(device);
            CHECK(snapshot);
            if (name == "TriggerMode") CHECK(!snapshot.value().trigger_mode);
            if (name == "ExposureAuto") CHECK(!snapshot.value().exposure_auto);
            if (boolean) CHECK(!snapshot.value().frame_rate_enable);
            CHECK(device.count(boolean ? "read_boolean" : "read_string_or_enum", name) == 0);
        }
        auto failed = baseline();
        populate(failed);
        failed.on_call = [&](auto&, const Call& call) -> std::optional<backend::Error> {
            if (call.operation == "implemented" && call.feature == name)
                return backend::Error{backend::ErrorCode::io_error, name + " implementation query failed"};
            return std::nullopt;
        };
        expect_error(profile::read_snapshot(failed), backend::ErrorCode::io_error, name + " implementation query failed");
        CHECK(failed.count(boolean ? "read_boolean" : "read_string_or_enum", name) == 0);
        CHECK(failed.writes().empty());
        for (const bool wrong_type : {false, true}) {
            auto unavailable = baseline();
            populate(unavailable);
            if (wrong_type) unavailable.nodes[name].kind = Kind::integer;
            else unavailable.nodes[name].available = false;
            expect_error(profile::read_snapshot(unavailable), backend::ErrorCode::unsupported, name);
            CHECK(unavailable.writes().empty());
        }
        auto unreadable = baseline();
        populate(unreadable);
        unreadable.on_call = [&](auto&, const Call& call) -> std::optional<backend::Error> {
            if (call.feature == name && call.operation == (boolean ? "read_boolean" : "read_string_or_enum"))
                return backend::Error{backend::ErrorCode::timeout, name + " value read timed out"};
            return std::nullopt;
        };
        expect_error(profile::read_snapshot(unreadable), backend::ErrorCode::timeout, "value read timed out");
    }
    auto disabled = baseline();
    disabled.nodes["AcquisitionFrameRateEnable"] = boolean_node(false);
    const auto snapshot = profile::read_snapshot(disabled);
    CHECK(snapshot && snapshot.value().frame_rate_enable == false);
    expect_error(profile::capabilities_from(snapshot.value()), backend::ErrorCode::unsupported, "AcquisitionFrameRateEnable is false");
    auto malformed_node = baseline();
    malformed_node.nodes["ExposureAuto"].kind = Kind::non_feature;
    expect_error(profile::read_snapshot(malformed_node), backend::ErrorCode::unsupported, "not a feature");
}

void first_implemented_aliases() {
    struct Alias { const char* preferred; const char* alternate; bool exposure; };
    for (const auto& alias : {Alias{"ExposureTime", "ExposureTimeAbs", true},
                              Alias{"AcquisitionFrameRate", "AcquisitionFrameRateAbs", false}}) {
        const auto arrange = [&] {
            auto device = baseline();
            device.nodes[alias.preferred] = float_node(alias.exposure ? 1000.0 : 25.0, 0.1, 1e7, alias.exposure ? "us" : "Hz");
            device.nodes[alias.alternate] = float_node(alias.exposure ? 2000.0 : 7.0, 0.1, 1e7, alias.exposure ? "us" : "Hz");
            return device;
        };
        auto preferred = arrange();
        const auto first = profile::read_snapshot(preferred);
        CHECK(first);
        CHECK((alias.exposure ? first.value().exposure_feature : first.value().frame_rate_feature) == alias.preferred);
        CHECK(preferred.count("implemented", alias.alternate) == 0);
        CHECK(preferred.count("read_float_feature", alias.alternate) == 0);
        for (const bool absent : {false, true}) {
            auto fallback = arrange();
            if (absent) fallback.nodes.erase(alias.preferred);
            else fallback.nodes[alias.preferred].implemented = false;
            const auto selected = profile::read_snapshot(fallback);
            CHECK(selected);
            CHECK((alias.exposure ? selected.value().exposure_feature : selected.value().frame_rate_feature) == alias.alternate);
            CHECK((alias.exposure ? selected.value().exposure_us.value : selected.value().frame_rate_hz.value) ==
                  (alias.exposure ? 2000.0 : 7.0));
            const auto opened = envelope(fallback);
            auto request = initial;
            request.frame_rate_hz = alias.exposure ? 25u : 3u;
            CHECK(apply(fallback, request, opened));
            CHECK(fallback.count("write_float", alias.alternate) == 1);
            CHECK(fallback.count("write_float", alias.preferred) == 0);
        }
        for (int failure = 0; failure < 4; ++failure) {
            auto bad = arrange();
            if (failure == 0) {
                bad.on_call = [&](auto&, const Call& call) -> std::optional<backend::Error> {
                    if (call.operation == "implemented" && call.feature == alias.preferred)
                        return backend::Error{backend::ErrorCode::io_error, std::string(alias.preferred) + " probe failed"};
                    return std::nullopt;
                };
            } else if (failure == 1) bad.nodes[alias.preferred].available = false;
            else if (failure == 2) bad.nodes[alias.preferred].kind = Kind::integer;
            else bad.nodes[alias.preferred].access = Access::read_only;
            expect_error(profile::read_snapshot(bad), failure == 0 ? backend::ErrorCode::io_error : backend::ErrorCode::unsupported,
                         alias.preferred);
            CHECK(bad.count("implemented", alias.alternate) == 0 && bad.writes().empty());
        }
        auto neither = arrange();
        neither.nodes.erase(alias.preferred);
        neither.nodes[alias.alternate].implemented = false;
        expect_error(profile::read_snapshot(neither), backend::ErrorCode::unsupported, "neither");

        auto changed = arrange();
        const auto opened = envelope(changed);
        changed.nodes[alias.preferred].implemented = false;
        expect_error(apply(changed, initial, opened), backend::ErrorCode::unsupported, "selected");
        CHECK(changed.writes().empty());
    }
}

void enumeration_and_owned_values() {
    auto unavailable = baseline();
    const auto opened = envelope(unavailable);
    unavailable.nodes["PixelFormat"].entries["Mono8"] = false;
    const auto snapshot = profile::read_snapshot(unavailable);
    CHECK(snapshot && !snapshot.value().mono8_available);
    expect_error(profile::read_configuration(unavailable, opened, connected), backend::ErrorCode::unsupported, "Mono8");
    CHECK(unavailable.writes().empty());

    auto failed = baseline();
    failed.on_call = [](auto&, const Call& call) -> std::optional<backend::Error> {
        if (call.operation == "enum_entry_available") {
            CHECK(call.feature == "PixelFormat" && call.entry == "Mono8");
            return backend::Error{backend::ErrorCode::timeout, "selected PixelFormat entry query timed out"};
        }
        return std::nullopt;
    };
    expect_error(profile::read_snapshot(failed), backend::ErrorCode::timeout, "entry query timed out");
    auto text_only = baseline();
    text_only.nodes["PixelFormat"].kind = Kind::string;
    expect_error(profile::read_snapshot(text_only), backend::ErrorCode::unsupported, "wrong type");

    auto mono12 = baseline();
    mono12.nodes["PixelFormat"] = text_node("Mono12");
    mono12.nodes["DeviceStreamChannelEndianness"] = text_node("Little", Kind::string);
    mono12.on_call = [](RecordingFeatures& device, const Call& call) -> std::optional<backend::Error> {
        // A later operation invalidates the fixture's original backing text.
        if (call.operation == "implemented" && call.feature == "TriggerSelector") {
            device.nodes["PixelFormat"].text = "changed after read";
            device.nodes["ExposureTimeAbs"].real.unit = "changed after read";
        }
        return std::nullopt;
    };
    const auto copied = profile::read_snapshot(mono12);
    CHECK(copied && copied.value().pixel_format == "Mono12" && copied.value().mono12_available && !copied.value().mono8_available);
    CHECK(copied.value().exposure_us.unit == "us" && copied.value().pixel_endianness == "Little");
    CHECK(profile::capabilities_from(copied.value()).value().pixel_format == backend::PixelFormat::mono12);
}

void fresh_snapshot_before_any_write() {
    const backend::CameraConfiguration request{640, 480, 16, 8, 25, 1ms};
    for (int change = 0; change < 3; ++change) {
        auto device = baseline();
        const auto opened = envelope(device);
        if (change == 0) device.nodes["ExposureTimeAbs"].real.maximum = 500;
        else if (change == 1) device.nodes["Width"].integer = {512, 8, 2048, 8};
        else device.nodes["Height"].integer.maximum = 256;
        const auto result = apply(device, request, opened);
        expect_error(result, change == 1 ? backend::ErrorCode::unsupported : backend::ErrorCode::invalid_argument,
                     change == 0 ? "ExposureTimeAbs" : change == 1 ? "increment changed" : "Height");
        CHECK(device.writes().empty());
        CHECK(device.nodes["Width"].integer.value == 512 && device.nodes["OffsetX"].integer.value == 0);
    }
    auto device = baseline();
    const auto opened = envelope(device);
    CHECK(profile::read_configuration(device, opened, connected).value() == initial);
    device.nodes["Width"].integer.value = 320;
    const auto refreshed = profile::read_configuration(device, opened, connected);
    CHECK(refreshed && refreshed.value().width == 320);
    CHECK(device.count("read_integer_feature", "Width") == 2 && device.writes().empty());
    // A selector change is read and diagnosed, never silently normalized.
    device.nodes["TriggerSelector"].text = "AcquisitionStart";
    expect_error(apply(device, initial, opened), backend::ErrorCode::unsupported, "TriggerSelector");
    CHECK(device.writes().empty());
}

void ordered_writes_and_live_constraints() {
    auto roi = baseline();
    roi.nodes["Width"].integer.value = 64;
    roi.nodes["OffsetX"].integer.value = 1900;
    roi.coupled_roi = true;
    roi.update_roi_bounds();
    const auto roi_opened = envelope(roi);
    const backend::CameraConfiguration expanded{2048, 512, 0, 0, 25, 1ms};
    CHECK(apply(roi, expanded, roi_opened).value() == expanded);
    CHECK((roi.writes() == std::vector<std::string>{"OffsetX", "Width", "ExposureTimeAbs", "AcquisitionFrameRate"}));
    CHECK(roi.nodes["Width"].integer.value == 2048 && roi.nodes["OffsetX"].integer.value == 0);

    auto timing = baseline();
    const auto timing_opened = envelope(timing);
    timing.nodes["ExposureTimeAbs"].real.maximum = 500;
    timing.after_write = [](auto& device, const Call& call) {
        if (call.feature == "AcquisitionFrameRate" && call.real == 20)
            device.nodes["ExposureTimeAbs"].real.maximum = 5000;
    };
    const backend::CameraConfiguration longer_period{512, 512, 0, 0, 20, 2ms};
    CHECK(apply(timing, longer_period, timing_opened).value() == longer_period);
    CHECK((timing.writes() == std::vector<std::string>{"AcquisitionFrameRate", "ExposureTimeAbs"}));

    auto late = baseline();
    const auto late_opened = envelope(late);
    late.after_write = [](auto& device, const Call& call) {
        if (call.feature == "AcquisitionFrameRate") device.nodes["ExposureTimeAbs"].real.maximum = 500;
    };
    expect_error(apply(late, longer_period, late_opened), backend::ErrorCode::invalid_argument, "live float bounds");
    CHECK((late.writes() == std::vector<std::string>{"AcquisitionFrameRate", "ExposureTimeAbs"}));
    CHECK(late.nodes["AcquisitionFrameRate"].real.value == 20 && late.nodes["ExposureTimeAbs"].real.value == 10000);
}

void partial_failure_and_guard_boundaries() {
    const backend::CameraConfiguration request{320, 240, 32, 16, 20, 2ms};
    const std::vector<std::string> expected{"OffsetX", "Width", "OffsetY", "Height", "AcquisitionFrameRate", "ExposureTimeAbs"};
    for (const std::size_t failure : {std::size_t{0}, std::size_t{3}, std::size_t{5}}) {
        auto device = baseline();
        const auto opened = envelope(device);
        device.on_call = [&](auto&, const Call& call) -> std::optional<backend::Error> {
            if (call.is_write() && call.feature == expected[failure])
                return backend::Error{backend::ErrorCode::invalid_argument, "original setter failure: " + call.feature};
            return std::nullopt;
        };
        const auto result = apply(device, request, opened);
        expect_error(result, backend::ErrorCode::invalid_argument, "original setter failure");
        CHECK(contains(result.error().message, "earlier successful writes:") && contains(result.error().message, "no rollback was attempted"));
        CHECK(contains(result.error().message, "actual native configuration:"));
        CHECK(device.writes() == std::vector<std::string>(expected.begin(), expected.begin() + static_cast<std::ptrdiff_t>(failure + 1)));
        if (failure == 0) CHECK(contains(result.error().message, "earlier successful writes: none"));
        if (failure == 5) {
            CHECK(contains(result.error().message, "OffsetX=32, Width=320, OffsetY=16, Height=240"));
            CHECK(contains(result.error().message, "320x240@(32,16) 20 Hz 10000000 ns"));
            device.on_call = {};
            CHECK(profile::read_configuration(device, opened, connected).value() ==
                  (backend::CameraConfiguration{320, 240, 32, 16, 20, 10ms}));
            CHECK(apply(device, request, opened).value() == request); // only an explicit retry continues
        }
    }
    auto unreadable = baseline();
    const auto unreadable_opened = envelope(unreadable);
    bool setter_failed = false;
    unreadable.on_call = [&](auto&, const Call& call) -> std::optional<backend::Error> {
        if (call.is_write() && call.feature == "ExposureTimeAbs") {
            setter_failed = true;
            return backend::Error{backend::ErrorCode::invalid_argument, "original exposure failure"};
        }
        if (setter_failed && call.operation == "read_integer")
            return backend::Error{backend::ErrorCode::disconnected, "secondary readback failure"};
        return std::nullopt;
    };
    const auto secondary = apply(unreadable, request, unreadable_opened);
    expect_error(secondary, backend::ErrorCode::invalid_argument, "original exposure failure");
    CHECK(contains(secondary.error().message, "secondary readback failure") && unreadable.writes() == expected);

    auto uncertain = baseline();
    const auto uncertain_opened = envelope(uncertain);
    uncertain.on_call = [](auto& device, const Call& call) -> std::optional<backend::Error> {
        if (call.is_write() && call.feature == "ExposureTimeAbs") {
            device.nodes["ExposureTimeAbs"].real.value = call.real; // effect occurred before a failing result
            return backend::Error{backend::ErrorCode::timeout, "write acknowledgement timed out"};
        }
        return std::nullopt;
    };
    const auto timeout = apply(uncertain, request, uncertain_opened);
    expect_error(timeout, backend::ErrorCode::timeout, "write acknowledgement timed out");
    CHECK(contains(timeout.error().message, "320x240@(32,16) 20 Hz 2000000 ns"));
    CHECK(uncertain.writes() == expected);

    auto guarded = baseline();
    const auto guarded_opened = envelope(guarded);
    const profile::BeforeWrite refuse_height = [](std::string_view name) -> backend::Status {
        if (name == "Height") return backend::Error{backend::ErrorCode::busy, "guard refused Height"};
        return {};
    };
    const auto refused = profile::apply_configuration(guarded, request, guarded_opened, connected, refuse_height);
    expect_error(refused, backend::ErrorCode::busy, "guard refused Height");
    CHECK(refused.error().message == "guard refused Height");
    CHECK((guarded.writes() == std::vector<std::string>{"OffsetX", "Width", "OffsetY"}));
    CHECK(guarded.count("read_integer", "SensorWidth") == 1); // no failed-setter readback for guard refusal

    auto connection = baseline();
    const auto connection_opened = envelope(connection);
    const profile::ConnectionCheck disconnected = []() -> backend::Status {
        return backend::Error{backend::ErrorCode::disconnected, "direct connection guard"};
    };
    expect_error(profile::read_configuration(connection, connection_opened, disconnected), backend::ErrorCode::disconnected,
                 "direct connection guard");
    CHECK(connection.calls.empty());
    bool lost = false;
    connection.on_call = [&](auto&, const Call& call) -> std::optional<backend::Error> {
        if (call.operation == "read_integer" && call.feature == "SensorWidth") {
            lost = true;
            return backend::Error{backend::ErrorCode::io_error, "snapshot query failure"};
        }
        return std::nullopt;
    };
    const profile::ConnectionCheck current_connection = [&]() -> backend::Status {
        if (lost) return backend::Error{backend::ErrorCode::disconnected, "connection lost during snapshot"};
        return {};
    };
    const auto priority = profile::read_configuration(connection, connection_opened, current_connection);
    expect_error(priority, backend::ErrorCode::disconnected, "connection lost during snapshot");
    CHECK(!contains(priority.error().message, "snapshot query failure") && connection.writes().empty());
}

void final_readback_is_authoritative() {
    auto quantized = baseline();
    const auto quantized_opened = envelope(quantized);
    quantized.after_write = [](auto& device, const Call& call) {
        if (call.feature == "ExposureTimeAbs") device.nodes["ExposureTimeAbs"].real.value = std::floor(call.real);
    };
    auto request = initial;
    request.exposure = 1'250'500ns;
    const auto actual = apply(quantized, request, quantized_opened);
    CHECK(actual && actual.value().exposure == 1'250'000ns && actual.value() != request);
    CHECK((quantized.writes() == std::vector<std::string>{"ExposureTimeAbs", "AcquisitionFrameRate"}));

    auto fractional = baseline();
    const auto fractional_opened = envelope(fractional);
    fractional.after_write = [](auto& device, const Call& call) {
        if (call.feature == "AcquisitionFrameRate") device.nodes["AcquisitionFrameRate"].real.value = 30.000300003000032;
    };
    request = initial;
    request.frame_rate_hz = 30;
    const auto unrepresentable = apply(fractional, request, fractional_opened);
    expect_error(unrepresentable, backend::ErrorCode::unsupported, "30.000300003");
    CHECK(contains(unrepresentable.error().message, "after writes:") && fractional.writes().size() == 2);
    fractional.after_write = {};
    request.frame_rate_hz = 40;
    CHECK(apply(fractional, request, fractional_opened).value() == request);

    auto failed = baseline();
    const auto failed_opened = envelope(failed);
    bool written = false;
    failed.after_write = [&](auto&, const Call& call) { if (call.feature == "AcquisitionFrameRate") written = true; };
    failed.on_call = [&](auto&, const Call& call) -> std::optional<backend::Error> {
        if (written && call.operation == "read_integer")
            return backend::Error{backend::ErrorCode::io_error, "final readback failed"};
        return std::nullopt;
    };
    const auto unreadable = apply(failed, initial, failed_opened);
    expect_error(unreadable, backend::ErrorCode::io_error, "final readback failed");
    CHECK(contains(unreadable.error().message, "after writes:") && failed.writes().size() == 2);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"typed snapshot, access and native metadata", typed_snapshot_and_metadata},
        {"optional absence is distinct from query, type and access errors", optional_absence_and_errors},
        {"first-implemented aliases and stable open identity", first_implemented_aliases},
        {"selected enumeration availability and owned values", enumeration_and_owned_values},
        {"fresh snapshot rejects before any write", fresh_snapshot_before_any_write},
        {"ordered setters and live dependent bounds", ordered_writes_and_live_constraints},
        {"partial errors, no retry and connection/write guards", partial_failure_and_guard_boundaries},
        {"final native readback is authoritative", final_readback_is_authoritative},
    };
    for (const auto& [name, run] : tests) {
        try { run(); }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
        std::cout << "PASS " << name << '\n';
    }
    std::cout << tests.size() << " SDK-free device-service scenarios passed\n";
    return 0;
}
