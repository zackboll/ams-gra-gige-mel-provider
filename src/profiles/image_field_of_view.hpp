#pragma once

#include "camera_profile.hpp"
#include <gige_mel/image_field_of_view.hpp>

namespace gige_mel::backend::profiles {

/// @brief Optional, internal profile-state companion for Image optics.
/// Implement only when the state owns an immutable model supplied for the
/// selected deployment. Absence is different from explicit support failing
/// for a configuration. Base interfaces and the module descriptor are unchanged.
/// @note Uninstalled; use matching sources and module build identity.
class IProfileImageFieldOfView {
public:
    /// Destroy the companion with its profile state.
    virtual ~IProfileImageFieldOfView() = default;

    /// @brief Compute both angular extents for the actual ROI, in radians.
    /// @param configuration Borrowed complete configuration, including pixel
    /// dimensions and sensor-relative pixel offsets. Do not retain it.
    /// @return Owned angles independent of the profile and its module, or an
    /// owned error. A success is deterministic for this configuration and model.
    /// @warning Bounded synchronous computation only: no native feature access,
    /// transport, waits, acquisition, or provider/consumer callbacks or reentry.
    /// Retain no borrowed configuration or service references. A device may
    /// forward this during Image enable; the provider freezes the result for
    /// the run and does not fall back if explicit support fails.
    virtual Result<ImageFieldOfView> image_field_of_view(const CameraConfiguration& configuration) const = 0;
};

/// @brief Borrow the companion from a live state, or return null if absent.
inline const IProfileImageFieldOfView* image_field_of_view_handler(const ICameraProfileState& state) noexcept {
    return dynamic_cast<const IProfileImageFieldOfView*>(&state);
}

/// @brief Retain a selected state's optional FOV operation for device forwarding.
/// @param state Actual selected state (possibly a host wrapper pinning a module).
/// @return An alias owning that same state and its module lifetime, or an empty
/// pointer if absent. Call through this alias while it is owned; the result has
/// no dependency on the alias. No feature service, worker or transaction is added.
inline std::shared_ptr<const IProfileImageFieldOfView> retain_image_field_of_view(
    const std::shared_ptr<const ICameraProfileState>& state) noexcept {
    if (state) {
        if (const auto* handler = image_field_of_view_handler(*state)) return {state, handler};
    }
    return {};
}

} // namespace gige_mel::backend::profiles