#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace gige_mel::backend {

// Project-local source contract, NOT a convention established by MEL.
// See docs/image-los-metadata.md. No coordinate or clock transformation.
enum class LosFrame { unknown, sensor_frd_intrinsic_zyx };
enum class LosTimeDomain { unknown, source_relative };
enum class LosEpoch { unknown, declared_source_origin };

struct LosSupport {
    bool euler{false}, report{false};
    LosFrame frame{LosFrame::unknown};
    LosTimeDomain time_domain{LosTimeDomain::unknown};
    LosEpoch epoch{LosEpoch::unknown};
    std::string source_id, provenance, epoch_meaning;
};

struct LosEulerAngles { double roll, pitch, yaw; };
struct LosAzEl { double azimuth, elevation; };
struct LosEuler {
    LosEulerAngles attitude, rates; // radians and Euler radians/second
};
struct LosPointing {
    std::optional<LosAzEl> angles, rates;
    std::optional<double> image_rotation;
    // No pinned masks are known. Only zero with all optional claims absent is
    // accepted. Nonzero flags are rejected, never silently stripped.
    std::uint32_t validity_flags{0};
    std::optional<bool> at_speed, in_tolerance;
    std::optional<LosEulerAngles> platform_attitude;
};
struct LosObservation {
    std::optional<std::chrono::nanoseconds> time;
    LosTimeDomain time_domain{LosTimeDomain::unknown};
    LosEpoch epoch{LosEpoch::unknown};
    LosFrame frame{LosFrame::unknown};
    std::uint64_t clock_generation{0}, subscription_id{0};
    std::string source_id;
    std::optional<LosEuler> euler;
    std::optional<LosPointing> report;
};

} // namespace gige_mel::backend