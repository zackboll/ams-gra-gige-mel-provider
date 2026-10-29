#include "config_parser.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <limits>

namespace gige_mel {
namespace {
using Json = nlohmann::json;

class ConfigSax final : public nlohmann::json_sax<Json> {
public:
    backend::CameraConfiguration configuration;
    std::string error;
    bool supported{true};
    bool complete{false};

    bool null() override { return reject("null is not a configuration value"); }
    bool boolean(bool) override { return reject("boolean is not a configuration value"); }
    bool number_float(number_float_t, const string_t&) override {
        return reject("configuration numbers must be bounded JSON integer tokens");
    }
    bool number_integer(number_integer_t value) override {
        if (key_ == 1) return version(value == 1);
        if (value < 0) return reject("camera settings must be nonnegative");
        return number_unsigned(static_cast<number_unsigned_t>(value));
    }
    bool number_unsigned(number_unsigned_t value) override {
        if (key_ == 1) return version(value == 1);
        if (key_ < 2 || key_ > 7) return reject("expected schema string or member name");
        const bool offset = key_ == 4 || key_ == 5;
        if (!offset && value == 0) return reject("dimensions, rate and exposure must be positive");
        if (key_ == 7) {
            using Rep = std::chrono::nanoseconds::rep;
            if (value > static_cast<number_unsigned_t>(std::numeric_limits<Rep>::max()))
                return reject("exposure_ns exceeds nanoseconds representation");
            configuration.exposure = std::chrono::nanoseconds{static_cast<Rep>(value)};
        } else {
            if (value > std::numeric_limits<std::uint32_t>::max()) return reject("camera setting exceeds uint32_t");
            const auto narrowed = static_cast<std::uint32_t>(value);
            switch (key_) {
            case 2: configuration.width = narrowed; break;
            case 3: configuration.height = narrowed; break;
            case 4: configuration.offset_x = narrowed; break;
            case 5: configuration.offset_y = narrowed; break;
            case 6: configuration.frame_rate_hz = narrowed; break;
            default: return reject("invalid numeric member");
            }
        }
        key_ = -1;
        return true;
    }
    bool string(string_t& value) override {
        if (key_ != 0) return reject("only schema may contain a string");
        supported = supported && value == "gige-mel.camera-config";
        key_ = -1;
        return true;
    }
    bool binary(binary_t&) override { return reject("binary value is not supported"); }
    bool start_object(std::size_t) override {
        if (started_) return reject("nested objects are not supported");
        started_ = true;
        return true;
    }
    bool key(string_t& name) override {
        static constexpr std::array<std::string_view, 8> names{
            "schema", "version", "width", "height", "offset_x", "offset_y", "frame_rate_hz", "exposure_ns"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (name != names[i]) continue;
            if (seen_[i]) return reject("duplicate decoded configuration member: " + name);
            seen_[i] = true;
            key_ = static_cast<int>(i);
            return true;
        }
        return reject("unknown configuration member: " + name);
    }
    bool end_object() override {
        for (bool seen : seen_) if (!seen) return reject("all eight configuration members are required");
        complete = true;
        return true;
    }
    bool start_array(std::size_t) override { return reject("arrays are not supported"); }
    bool end_array() override { return reject("arrays are not supported"); }
    bool parse_error(std::size_t, const std::string&, const Json::exception& exception) override {
        return reject(exception.what());
    }

private:
    bool reject(std::string message) { error = std::move(message); return false; }
    bool version(bool is_one) { supported = supported && is_one; key_ = -1; return true; }
    std::array<bool, 8> seen_{};
    int key_{-1};
    bool started_{false};
};
}

backend::Result<backend::CameraConfiguration> parse_camera_config(std::string_view input) {
    using B = backend::ErrorCode;
    if (input.size() > 4096) return backend::Error{B::invalid_argument, "configuration exceeds 4096 bytes"};
    if (input.find('\0') != std::string_view::npos)
        return backend::Error{B::invalid_argument, "configuration contains an embedded NUL"};
    ConfigSax sax;
    try {
        // Strict parsing consumes the entire input. SAX false aborts parsing,
        // unlike the DOM callback's discard semantics. Comments stay disabled.
        if (!Json::sax_parse(input.begin(), input.end(), &sax, Json::input_format_t::json, true) || !sax.complete)
            return backend::Error{B::invalid_argument, sax.error.empty() ? "expected one flat configuration object" : sax.error};
    } catch (const Json::exception& exception) {
        return backend::Error{B::invalid_argument, exception.what()};
    }
    if (!sax.supported) return backend::Error{B::unsupported, "unsupported camera configuration schema or version"};
    return sax.configuration;
}
}