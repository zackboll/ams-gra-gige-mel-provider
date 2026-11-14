#pragma once

// Internal source-level C++20 extension seam, never installed or a binary ABI.
#include "device_features.hpp"
#include "frame_policy.hpp"
#include <functional>

namespace gige_mel::backend::profiles {

inline constexpr std::string_view mono_free_running_v1 = "mono_free_running_v1";
using ConnectionCheck = std::function<Status()>;
using BeforeWrite = std::function<Status(std::string_view)>;

// Each implementation owns its distinct, immutable open-time native snapshot.
// No native handles or borrowed feature services may be retained. Native methods
// run under the adapter's existing device lock; validate_frame must also support
// concurrent receive. Callbacks are synchronous, borrowed and must not reenter.
// A state must own everything its destructor and methods need independently of
// its parent profile (which need not outlive a retained native-buffer lease).
class ICameraProfileState {
public:
    virtual ~ICameraProfileState() = default;
    virtual CameraCapabilities capabilities() const = 0;
    // Optional read-only inspection text; no SDK values or native I/O retained.
    virtual std::vector<std::pair<std::string, std::string>> native_diagnostics() const { return {}; }
    virtual Result<CameraConfiguration> configuration(device_features::IDeviceFeatures&,
                                                      const ConnectionCheck&) const = 0;
    // Validate the complete request before writes, check fresh native constraints
    // at ordered writes, return truthful readback; no implicit rollback.
    virtual Result<CameraConfiguration> apply(device_features::IDeviceFeatures&, const CameraConfiguration&,
                                              const ConnectionCheck&, const BeforeWrite&) const = 0;
    virtual Result<CameraConfiguration> preflight(device_features::IDeviceFeatures&) const = 0;
    // accept promises a fully validated layout/span inside received capacity and
    // the selected representation. Incomplete/reject must explain the decision.
    virtual FrameDecision validate_frame(const NativeFrameFacts&, const CameraConfiguration&, PixelFormat) const = 0;
};

class ICameraProfile {
public:
    virtual ~ICameraProfile() = default;
    virtual std::string_view id() const noexcept = 0;
    // Explicit device-open boundary: discover features, build a concrete snapshot,
    // validate device compatibility and capabilities. Registration never calls it.
    virtual Result<std::shared_ptr<const ICameraProfileState>> open(device_features::IDeviceFeatures&) const = 0;
};

std::shared_ptr<const ICameraProfile> make_mono_free_running_profile();

} // namespace gige_mel::backend::profiles