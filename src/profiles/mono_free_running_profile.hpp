#pragma once

#include "camera_profile.hpp"
#include "mono_free_running.hpp"

namespace gige_mel::backend::profiles {

/// @brief Reusable per-device policy for free-running monochrome cameras.
/// Uses manual exposure and integer-Hz rate control with SFNC feature aliases,
/// untriggered Continuous acquisition, and a single Mono8 or explicitly
/// little-endian unpacked Mono12 image. It does not normalize native modes or
/// implement optional capability interfaces. Override individual operations
/// to specialize these assumptions, or implement ICameraProfileState directly
/// when the native representation or sequencing differs substantially.
/// @note Internal and uninstalled; use matching sources and build identity.
class MonoFreeRunningProfileState : public ICameraProfileState {
public:
    /// @brief Own the validated native admission facts and advertised capabilities by value.
    /// A derived profile may narrow the advertised policy bounds independently
    /// of the native snapshot. Preserve that snapshot for truthful readback,
    /// native compatibility checks and explicit recovery outside policy bounds.
    /// State needs neither its parent profile nor an open feature service to
    /// survive. Feature services and guards are borrowed only for a method call.
    MonoFreeRunningProfileState(mono_free_running::NativeSnapshot envelope, CameraCapabilities capabilities);

    /// Return the immutable advertised capabilities without native reads.
    CameraCapabilities capabilities() const override;
    /// Describe the original native admission facts, not live settings.
    std::vector<std::pair<std::string, std::string>> native_diagnostics() const override;
    /// Read actual representable configuration from fresh native facts under
    /// the supplied connection guard. Does not consult virtual capabilities().
    Result<CameraConfiguration> configuration(device_features::IDeviceFeatures& features,
        const ConnectionCheck& check) const override;
    /// Read fresh native constraints, plan ordered writes and return actual
    /// readback. Apply additional request policy before delegating here; this
    /// operation does not enforce narrowed advertised policy bounds.
    Result<CameraConfiguration> apply(device_features::IDeviceFeatures& features,
        const CameraConfiguration& request, const ConnectionCheck& check, const BeforeWrite& before) const override;
    /// Freeze the representable current configuration, then require Continuous
    /// acquisition. Override if model-specific native checks must run in a
    /// different order; the shared admission algorithm also accepts a snapshot.
    Result<CameraConfiguration> preflight(device_features::IDeviceFeatures& features) const override;
    /// Validate native status, monochrome representation, geometry and span
    /// against the frozen run settings without native I/O.
    FrameDecision validate_frame(const NativeFrameFacts& facts, const CameraConfiguration& frozen,
        PixelFormat format) const override;

protected:
    /// Borrow the immutable admission snapshot for the lifetime of this state.
    /// This is not a current device reading or the advertised policy envelope.
    const mono_free_running::NativeSnapshot& native_admission() const noexcept { return envelope_; }

private:
    const mono_free_running::NativeSnapshot envelope_;
    const CameraCapabilities capabilities_;
};

/// @brief Default monochrome free-running profile, constructible without I/O.
/// Internal and SDK-/MEL-free. A derived model profile can reuse the state
/// implementation but must override open() to construct its own derived state.
class MonoFreeRunningProfile : public ICameraProfile {
public:
    /// Construct the stateless policy without opening a device.
    MonoFreeRunningProfile() = default;

    /// Return the canonical selection ID mono_free_running_v1.
    std::string_view id() const noexcept override;
    /// Read and admit the native snapshot without writes and construct a
    /// MonoFreeRunningProfileState independent of this profile. The feature
    /// service is borrowed only for this invocation.
    Result<std::shared_ptr<const ICameraProfileState>> open(device_features::IDeviceFeatures& features) const override;
};

} // namespace gige_mel::backend::profiles