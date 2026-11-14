#pragma once

#include "profiles/camera_profile.hpp"

namespace gige_mel::backend::profiles::cognex {
inline constexpr std::string_view profile_id = "cognex_cic_5000r_v1";

class CognexCIC5000RProfile final : public ICameraProfile {
public:
    std::string_view id() const noexcept override { return profile_id; }
    Result<std::shared_ptr<const ICameraProfileState>> open(device_features::IDeviceFeatures&) const override;
};
} // namespace gige_mel::backend::profiles::cognex