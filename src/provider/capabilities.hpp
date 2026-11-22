#pragma once

#include "runtime.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/// Check supported packed output layout, ROI containment in sensor pixels and
/// MEL byte-count representability. Not a validation of all backend controls.
bool valid_layout(const backend::CameraConfiguration& c, const backend::CameraCapabilities& caps);
/** @brief Build a value snapshot from retained state under state_mutex, not device I/O.
 * @param runtime Initialized session; caller must not hold state_mutex.
 * @param type Advertised channel family.
 * @param id Copied channel identity.
 * @param config Optional borrowed attachment configuration for platform/location.
 * @return Geometry (logical bit depth, row pitch and sizes in bytes) where
 * applicable, and only the supported metadata families. Stack uses the declared
 * product. Does not assert actual configuration validity or device readiness.
 * @throws std::overflow_error If an Image/C2 layout cannot be represented.
 */
ir::ChannelCapability capability(const Runtime& runtime, ir::ChannelType type,
                                 const mel::UCI_ID& id, const ir::Config* config = nullptr);

} // namespace provider_detail
} // namespace gige_mel
