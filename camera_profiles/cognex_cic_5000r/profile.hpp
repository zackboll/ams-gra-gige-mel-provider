#pragma once

#include "profiles/mono_free_running_profile.hpp"

namespace gige_mel::backend::profiles::cognex {
inline constexpr std::string_view profile_id = "cognex_cic_5000r_v1";

/// Matched-build model policy for the characterized CIC-5000R firmware.
class CognexCIC5000RProfile final : public MonoFreeRunningProfile {
public:
    std::string_view id() const noexcept override { return profile_id; }
    /// Check exact identity before discovering configuration, then build the
    /// Cognex-specific state rather than the default monochrome state.
    Result<std::shared_ptr<const ICameraProfileState>> open(device_features::IDeviceFeatures&) const override;
};
} // namespace gige_mel::backend::profiles::cognex