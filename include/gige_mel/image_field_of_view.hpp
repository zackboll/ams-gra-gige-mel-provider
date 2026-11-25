#pragma once

#include <gige_mel/backend.hpp>

namespace gige_mel::backend {

/// @brief Owned angular extents of one Image ROI, in radians.
/// Contains only scalars: no borrowed storage, plugin pointer, callable or
/// deleter. It remains usable after the source and any module have retired.
struct ImageFieldOfView {
    double horizontal_radians{}; ///< Horizontal angular extent in radians.
    double vertical_radians{}; ///< Vertical angular extent in radians.
};

/// @brief Optional Image-only companion on an ICameraDevice.
/// The common provider discovers this interface by cross-cast. If absent, it
/// uses frozen width/height times the session's angular pitches instead. If
/// present, errors, exceptions and invalid angles fail Image enable without
/// fallback. The provider requires finite, strictly positive angles, freezes
/// them before acquisition, and publishes them without consulting the source
/// again during that run. This interface makes no calibration, pointing,
/// distortion, navigation or timestamp claim and does not apply to StackedImage.
/// @note SDK- and MEL-independent project-owned C++ source contract. Use
/// compatible C++ builds; this is not a stable cross-toolchain plugin ABI.
class IImageFieldOfViewSource {
public:
    /// Destroy the optional interface with its device.
    virtual ~IImageFieldOfViewSource() = default;

    /// @brief Compute both angular extents from immutable model data.
    /// @param configuration Complete actual configuration borrowed for this
    /// call; interpret ROI dimensions and sensor-relative offsets in pixels.
    /// @return Independent owned angles in radians, or an owned diagnostic.
    /// @warning Called synchronously during serialized Image enable. The
    /// computation must be bounded and deterministic. Do not access native
    /// features, perform transport operations, wait, acquire, reenter the
    /// provider or invoke provider/consumer callbacks. Retain no borrowed
    /// configuration or service references.
    virtual Result<ImageFieldOfView> image_field_of_view(const CameraConfiguration& configuration) const = 0;
};

} // namespace gige_mel::backend