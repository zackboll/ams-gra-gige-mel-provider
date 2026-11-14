#include "camera_profile.hpp"
#include "mono_free_running_device.hpp"
#include <cstdio>

namespace gige_mel::backend::profiles {
namespace {
namespace mono = mono_free_running;
std::string integer_text(const device_features::IntegerFeature& f) {
    return std::to_string(f.value) + " [" + std::to_string(f.minimum) + ", " + std::to_string(f.maximum) + "] inc " +
           std::to_string(f.increment);
}

std::string float_text(const device_features::FloatFeature& f) {
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, "%.17g [%.17g, %.17g]%s%s", f.value, f.minimum, f.maximum,
                  f.unit ? " unit " : "", f.unit ? f.unit->c_str() : "");
    return buffer;
}

class MonoState final : public ICameraProfileState {
public:
    MonoState(mono::NativeSnapshot envelope, CameraCapabilities capabilities)
        : envelope_(std::move(envelope)), capabilities_(std::move(capabilities)) {}
    CameraCapabilities capabilities() const override { return capabilities_; }
    std::vector<std::pair<std::string, std::string>> native_diagnostics() const override {
        const auto& n = envelope_;
        std::vector<std::pair<std::string, std::string>> values;
        values.emplace_back("SensorWidth", std::to_string(n.sensor_width));
        values.emplace_back("SensorHeight", std::to_string(n.sensor_height));
        values.emplace_back("Width", integer_text(n.width));
        values.emplace_back("Height", integer_text(n.height));
        values.emplace_back("OffsetX", integer_text(n.offset_x));
        values.emplace_back("OffsetY", integer_text(n.offset_y));
        values.emplace_back(n.frame_rate_feature, float_text(n.frame_rate_hz));
        values.emplace_back(n.exposure_feature, float_text(n.exposure_us));
        values.emplace_back("PixelFormat", n.pixel_format + (n.mono8_available ? " (Mono8 available)" : " (Mono8 unavailable)"));
        values.emplace_back("TriggerSelector", n.trigger_selector.value_or("<absent>"));
        values.emplace_back("TriggerMode", n.trigger_mode.value_or("<absent>"));
        values.emplace_back("ExposureAuto", n.exposure_auto.value_or("<absent>"));
        values.emplace_back("AcquisitionFrameRateEnable",
                                   n.frame_rate_enable ? (*n.frame_rate_enable ? "true" : "false") : "<absent>");
        return values;
    }
    Result<CameraConfiguration> configuration(device_features::IDeviceFeatures& features,
                                              const ConnectionCheck& check) const override {
        return mono::read_configuration(features, envelope_, check);
    }
    Result<CameraConfiguration> apply(device_features::IDeviceFeatures& features, const CameraConfiguration& request,
                                      const ConnectionCheck& check, const BeforeWrite& before) const override {
        return mono::apply_configuration(features, request, envelope_, check, before);
    }
    Result<CameraConfiguration> preflight(device_features::IDeviceFeatures& features) const override {
        auto snapshot = mono::read_snapshot(features);
        if (!snapshot) return snapshot.error();
        auto frozen = mono::acquisition_preflight(snapshot.value(), envelope_);
        if (!frozen) return frozen.error();
        auto mode = features.read_string_or_enum("AcquisitionMode");
        if (!mode) return mode.error();
        if (mode.value() != mono::acquisition_mode)
            return Error{ErrorCode::unsupported, "AcquisitionMode is '" + mode.value() +
                "'; the profile requires Continuous, which the adapter does not set"};
        return frozen;
    }
    FrameDecision validate_frame(const NativeFrameFacts& facts, const CameraConfiguration& frozen,
                                 PixelFormat format) const override {
        return mono::validate_frame(facts, frozen, format);
    }
private:
    const mono::NativeSnapshot envelope_;
    const CameraCapabilities capabilities_;
};

class MonoProfile final : public ICameraProfile {
public:
    std::string_view id() const noexcept override { return mono_free_running_v1; }
    Result<std::shared_ptr<const ICameraProfileState>> open(device_features::IDeviceFeatures& features) const override {
        auto snapshot = mono::read_snapshot(features);
        if (!snapshot) return snapshot.error();
        auto caps = mono::capabilities_from(snapshot.value());
        if (!caps) return Error{caps.error().code, "unsupported camera profile: " + caps.error().message};
        return std::shared_ptr<const ICameraProfileState>(
            std::make_shared<MonoState>(std::move(snapshot).value(), std::move(caps).value()));
    }
};
}

std::shared_ptr<const ICameraProfile> make_mono_free_running_profile() { return std::make_shared<MonoProfile>(); }
} // namespace gige_mel::backend::profiles