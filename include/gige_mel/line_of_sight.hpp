#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace gige_mel::backend {

/// Project-local convention, not a coordinate convention established by MEL.
/// Right-handed sensor axes are forward/right/down; orientation uses intrinsic
/// Z-Y-X (roll X, pitch Y, yaw Z). Unknown is not supported.
enum class LosFrame { unknown, sensor_frd_intrinsic_zyx };
/// Only source-relative time is supported, not UTC or host reception time.
enum class LosTimeDomain { unknown, source_relative };
/// Time origin named by the source in LosSupport::epoch_meaning.
enum class LosEpoch { unknown, declared_source_origin };

/// @brief Immutable device-lifetime claims for independent Euler and Report outputs.
/// To advertise either, supply the supported frame/domain/epoch and nonempty
/// source identity, provenance and origin description. These establish the
/// expected source semantics, not proof of physical pointing or alignment.
/// @see ILineOfSightSource, @ref md_docs_2image-los-metadata "Source-backed Image LOS metadata"
struct LosSupport {
    bool euler{false}, report{false}; ///< Availability of each representation.
    LosFrame frame{LosFrame::unknown};
    LosTimeDomain time_domain{LosTimeDomain::unknown};
    LosEpoch epoch{LosEpoch::unknown};
    /// Stable source identity, evidence/origin of the pointing data, and meaning
    /// of the source-relative time origin, respectively.
    std::string source_id, provenance, epoch_meaning;
};

/// X/Y/Z Euler components in the declared LOS frame.
struct LosEulerAngles { double roll, pitch, yaw; };
/// Sensor-relative azimuth positive toward right; elevation positive upward.
struct LosAzEl { double azimuth, elevation; };
/// Aperture attitude in radians and Euler derivatives in rad/s (not body p/q/r).
struct LosEuler {
    LosEulerAngles attitude, rates;
};
/// @brief Sensor-relative Report payload; no inferred conversion from Euler.
/// Admission requires finite angles (radians), rates (rad/s) and image
/// rotation (radians, right-hand positive about aperture forward), all present.
struct LosPointing {
    std::optional<LosAzEl> angles, rates;
    std::optional<double> image_rotation;
    /// Only zero is accepted. No pinned masks are established; zero means
    /// absence of optional claims, not evidence of false status or zero attitude.
    std::uint32_t validity_flags{0};
    /// Unsupported; both must be absent, even if the proposed value is false.
    std::optional<bool> at_speed, in_tolerance;
    /// Unsupported; must be absent. Nonzero flags or supplied optional status
    /// or attitude reject the Report rather than stripping the claims.
    std::optional<LosEulerAngles> platform_attitude;
};
/// @brief Owned source observation, with optional independent Euler and Report data.
/// Identity and clock/frame labels must match the support declaration and the
/// open subscription. Positive time increases strictly per accepted output;
/// malformed or absent Euler does not by itself prevent a valid Report, or vice
/// versa. No clock/coordinate conversion, image association, or synthesis from
/// a NavigationReport is performed.
struct LosObservation {
    /// Positive nanoseconds relative to the declared origin; required for delivery.
    std::optional<std::chrono::nanoseconds> time;
    LosTimeDomain time_domain{LosTimeDomain::unknown};
    LosEpoch epoch{LosEpoch::unknown};
    LosFrame frame{LosFrame::unknown};
    /// Fixed nonzero clock generation and caller-issued subscription token.
    std::uint64_t clock_generation{0}, subscription_id{0};
    std::string source_id;
    std::optional<LosEuler> euler;
    std::optional<LosPointing> report;
};

} // namespace gige_mel::backend