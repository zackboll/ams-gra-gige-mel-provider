#pragma once

// Internal C++20 extension seam, not a stable binary ABI. Matched-toolchain
// module use requires profile_module.hpp validation and independent state pins.
#include "device_features.hpp"
#include "frame_policy.hpp"
#include <functional>

namespace gige_mel::backend::profiles {

/// Stable binding ID of the built-in monochrome free-running policy.
inline constexpr std::string_view mono_free_running_v1 = "mono_free_running_v1";

/// Synchronous adapter guard that reports current connection/control failure.
/// Borrowed for the invoking method only, under the device operation lock.
/// Neither the guard nor code it calls may reenter provider/device operations
/// or wait for work that needs the same lock.
using ConnectionCheck = std::function<Status()>;

/// Synchronous adapter guard invoked before each planned native setter with its
/// feature name. A failure stops further writes, not effects of earlier writes.
/// The guard and feature name are borrowed; the ConnectionCheck non-reentrancy
/// rules also apply here.
using BeforeWrite = std::function<Status(std::string_view)>;

/// @brief Per-device policy owning the immutable facts established at admission.
/// Own all method and destructor dependencies independently of the parent
/// profile. Streams can retain state independently. Do not retain the borrowed
/// feature service, native handles or SDK node references. The backend owns
/// connection, acquisition and buffer lifetimes. It serializes native state
/// operations; frame validation does not rely on that serialization.
class ICameraProfileState {
public:
    virtual ~ICameraProfileState() = default;

    /// Return the validated open-time capability snapshot without native I/O.
    /// Bounds are an envelope, not admission of every value or combination.
    virtual CameraCapabilities capabilities() const = 0;

    /// Return optional owned name/value inspection text (empty by default).
    /// No native I/O, live telemetry guarantee, or machine-readable schema.
    virtual std::vector<std::pair<std::string, std::string>> native_diagnostics() const { return {}; }

    /// @brief Read a complete, freshly obtained native configuration.
    /// Use the borrowed feature service under adapter serialization. The shared
    /// monochrome implementation calls the connection guard before and after
    /// reading its native snapshot.
    /// @return Actual representable values, not the last request or a clamp to
    /// the supported envelope; otherwise a diagnostic for failed access,
    /// incompatible state or unrepresentable readback.
    virtual Result<CameraConfiguration> configuration(device_features::IDeviceFeatures&,
                                                      const ConnectionCheck&) const = 0;

    /// @brief Validate a complete request and apply a dependency-ordered write plan.
    /// The request is a complete desired configuration, not a patch. Use the
    /// borrowed service under adapter serialization and the supplied guards
    /// for readback and before each setter, respectively.
    /// @pre Adapter has admitted configuration (e.g. no active acquisition).
    /// @return Fresh actual readback on success, which may differ because of
    /// native quantization; otherwise a diagnostic, not a promise of no effect.
    /// @note Validate policy before writing, then enforce fresh native constraints
    /// at each setter, including limits changed by preceding writes. The shared
    /// implementation preserves a setter error and reports earlier writes plus
    /// best-effort readback; a failed before-write guard is returned directly.
    /// @warning After a later write, guard or readback failure, earlier writes
    /// can remain in effect. No automatic rollback or unrelated mode changes.
    virtual Result<CameraConfiguration> apply(device_features::IDeviceFeatures&, const CameraConfiguration&,
                                              const ConnectionCheck&, const BeforeWrite&) const = 0;

    /// @brief Check effective native configuration before allocating or starting a run.
    /// Use the borrowed feature service under adapter serialization.
    /// @return Configuration to freeze for receive-time checks, or the native or
    /// policy error. Read only: do not repair incompatible mode or format.
    /// @see plan_pool for the separate host resource check.
    virtual Result<CameraConfiguration> preflight(device_features::IDeviceFeatures&) const = 0;

    /// @brief Admit or reject a native frame against the frozen run configuration.
    /// Compare the adapter's borrowed buffer facts to the configuration admitted
    /// by preflight and the pixel representation selected for the run.
    /// @return Validated source stride and span for an accepted frame, within
    /// received bytes and capacity; incomplete/reject must explain why.
    /// @note May run concurrently during receive. Use immutable facts or
    /// synchronized instrumentation; no native I/O, borrowing of SDK state, or
    /// assumption that the device operation lock is held.
    virtual FrameDecision validate_frame(const NativeFrameFacts&, const CameraConfiguration&, PixelFormat) const = 0;
};

/// @brief Internal camera-model policy, constructed without opening a device.
/// Catalog registration and selection do not perform device admission. The
/// adapter opens the exact selected device and invokes the profile on that
/// device; the profile checks the identity/features required by its policy.
class ICameraProfile {
public:
    virtual ~ICameraProfile() = default;

    /// Stable, exact profile ID matching its catalog key and module descriptor
    /// where applicable. The view is borrowed from the live profile.
    virtual std::string_view id() const noexcept = 0;

    /// @brief Admit an explicitly opened device and build its independent policy state.
    /// Borrowed native feature service for this invocation
    /// only, provided while the adapter owns exclusive open-time access or holds
    /// the device operation lock. Do not retain it or any SDK references.
    /// @return Non-null owned state on success, or a diagnostic on unsupported
    /// identity, features or capabilities, or failed native access. No fallback
    /// or implicit device configuration. Use Result for expected failures.
    virtual Result<std::shared_ptr<const ICameraProfileState>> open(device_features::IDeviceFeatures&) const = 0;
};

/// Construct the built-in policy without device I/O.
std::shared_ptr<const ICameraProfile> make_mono_free_running_profile();

} // namespace gige_mel::backend::profiles