#include "profile.hpp"
#include "profiles/mono_free_running_device.hpp"

#include <algorithm>
#include <array>

namespace gige_mel::backend::profiles::cognex {
namespace {
namespace mono = mono_free_running;
using device_features::IDeviceFeatures;
constexpr std::array<std::uint32_t, 6> rates{1, 2, 4, 5, 8, 10};

Status admit_rate(std::uint32_t rate) {
    if (std::find(rates.begin(), rates.end(), rate) != rates.end()) return {};
    return Error{ErrorCode::unsupported, "cognex_cic_5000r_v1: AcquisitionFrameRateAbs admits only 1, 2, 4, 5, 8, 10 Hz; no rounding or clamping"};
}

Status identity(IDeviceFeatures& features) {
    const std::pair<std::string_view, std::string_view> required[]{
        {"DeviceVendorName", "COGNEX"}, {"DeviceModelName", "CIC-5000R"},
        {"DeviceFirmwareVersion", "106349-08;U;CIC-5000R;V3.7-1;0"}};
    for (const auto& [name, expected] : required) {
        auto actual = features.read_string_or_enum(name);
        if (!actual) return Error{actual.error().code, std::string(name) + ": " + actual.error().message};
        if (actual.value() != expected)
            return Error{ErrorCode::unsupported, std::string(name) + " is '" + actual.value() +
                "'; cognex_cic_5000r_v1 requires exactly '" + std::string(expected) + "'"};
    }
    return {};
}

// These are required on the documented firmware, not optional generic features.
Status native_contract(IDeviceFeatures& features, const mono::NativeSnapshot& n) {
    if (n.frame_rate_feature != "AcquisitionFrameRateAbs")
        return Error{ErrorCode::unsupported, "cognex_cic_5000r_v1 requires AcquisitionFrameRateAbs (Hz)"};
    if (n.exposure_feature != "ExposureTimeAbs")
        return Error{ErrorCode::unsupported, "cognex_cic_5000r_v1 requires ExposureTimeAbs (microseconds)"};
    if (n.pixel_format != "Mono8")
        return Error{ErrorCode::unsupported, "cognex_cic_5000r_v1 requires actual PixelFormat Mono8; Mono12 byte order is unqualified"};
    if (!n.frame_rate_enable || !*n.frame_rate_enable)
        return Error{ErrorCode::unsupported, "AcquisitionFrameRateEnable must be present and true; the profile does not enable it"};
    if (!n.trigger_mode || *n.trigger_mode != "Off")
        return Error{ErrorCode::unsupported, "TriggerMode must be present and Off; the profile does not change it"};
    auto mode = features.read_string_or_enum("AcquisitionMode");
    if (!mode) return mode.error();
    if (mode.value() != "Continuous")
        return Error{ErrorCode::unsupported, "AcquisitionMode must be Continuous; the profile does not change it"};
    return {};
}

class CognexCIC5000RState final : public MonoFreeRunningProfileState {
public:
    using MonoFreeRunningProfileState::MonoFreeRunningProfileState;
    std::vector<std::pair<std::string, std::string>> native_diagnostics() const override {
        return {{"DeviceVendorName", "COGNEX"}, {"DeviceModelName", "CIC-5000R"},
            {"DeviceFirmwareVersion", "106349-08;U;CIC-5000R;V3.7-1;0"},
            {"ProfileRatePolicyHz", "1,2,4,5,8,10"}};
    }
    Result<CameraConfiguration> apply(IDeviceFeatures& features, const CameraConfiguration& request,
        const ConnectionCheck& check, const BeforeWrite& before) const override {
        if (auto ok = check(); !ok) return ok.error();
        if (auto ok = admit_rate(request.frame_rate_hz); !ok) return ok.error();
        auto current = mono::read_snapshot(features);
        if (!current) return current.error();
        if (auto ok = native_contract(features, current.value()); !ok) return ok.error();
        return MonoFreeRunningProfileState::apply(features, request, check, before);
    }
    Result<CameraConfiguration> preflight(IDeviceFeatures& features) const override {
        auto current = mono::read_snapshot(features);
        if (!current) return current.error();
        if (auto ok = native_contract(features, current.value()); !ok) return ok.error();
        // Reuse admission on this snapshot: the generic preflight would read
        // again and check AcquisitionMode after representation admission.
        auto frozen = mono::acquisition_preflight(current.value(), native_admission());
        if (!frozen) return frozen.error();
        if (auto ok = admit_rate(frozen.value().frame_rate_hz); !ok) return ok.error();
        return frozen;
    }
    FrameDecision validate_frame(const NativeFrameFacts& facts, const CameraConfiguration& frozen,
        PixelFormat format) const override {
        if (format != PixelFormat::mono8)
            return {FrameVerdict::reject, ErrorCode::unsupported, 0, 0, "cognex_cic_5000r_v1 admits Mono8 only"};
        return MonoFreeRunningProfileState::validate_frame(facts, frozen, format);
    }
};
} // namespace

Result<std::shared_ptr<const ICameraProfileState>> CognexCIC5000RProfile::open(IDeviceFeatures& features) const {
    if (auto ok = identity(features); !ok) return ok.error(); // before configuration discovery, never writes
    auto snapshot = mono::read_snapshot(features);
    if (!snapshot) return snapshot.error();
    if (auto ok = native_contract(features, snapshot.value()); !ok) return ok.error();
    auto caps = mono::capabilities_from(snapshot.value());
    if (!caps) return caps.error();
    const auto admitted = [&](auto rate) {
        return rate >= caps.value().minimum_frame_rate_hz && rate <= caps.value().maximum_frame_rate_hz;
    };
    const auto low = std::find_if(rates.begin(), rates.end(), admitted);
    const auto high = std::find_if(rates.rbegin(), rates.rend(), admitted);
    if (low == rates.end())
        return Error{ErrorCode::unsupported, "AcquisitionFrameRateAbs native bounds contain no Cognex-admitted rate"};
    caps.value().minimum_frame_rate_hz = *low;
    caps.value().maximum_frame_rate_hz = *high;
    return std::shared_ptr<const ICameraProfileState>(
        std::make_shared<CognexCIC5000RState>(std::move(snapshot).value(), std::move(caps).value()));
}
} // namespace gige_mel::backend::profiles::cognex