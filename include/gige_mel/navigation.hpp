#pragma once

#include <chrono>

namespace gige_mel::backend {

/// Consumer-supplied solution labels, not measured accuracy or quality grades.
enum class NavigationSolutionState { aligning, free_inertial, gps, blended };

/// Roll, pitch and yaw in source order. Forwarding does not establish an
/// axis convention, rotation order, or conversion to the project LOS frame.
struct NavigationEuler {
    double roll{}, pitch{}, yaw{};
    bool operator==(const NavigationEuler&) const = default;
};

/// North/east/down components: m/s for speed, m/s² for acceleration.
struct NavigationNorthEastDown {
    double north{}, east{}, down{};
    bool operator==(const NavigationNorthEastDown&) const = default;
};

/// Source attitude-rate components and their separate time sample.
struct NavigationAttitudeRate {
    NavigationEuler attitude_rate;
    /// Nonnegative nanoseconds; no epoch or alignment with system_time assumed.
    std::chrono::nanoseconds attitude_rate_time{};
    bool operator==(const NavigationAttitudeRate&) const = default;
};

/// @brief The 18 named MEL covariance entries, not a reconstructed matrix.
/// `p`/`v` denote position/velocity and `n`/`e`/`d` denote NED axes. The
/// position-position and velocity-velocity groups contain six terms each;
/// the position-velocity group contains six distinct cross terms. All must
/// be finite. Only PnPn, PePe, PdPd, VnVn, VeVe, VdVd are nonnegative
/// variances; cross terms may be signed. No rescaling or positive-semidefinite
/// or physical-accuracy check is performed.
struct PositionVelocityCovariance {
    double position_position_pn_pn{}, position_position_pn_pe{}, position_position_pn_pd{};
    double position_position_pe_pe{}, position_position_pe_pd{}, position_position_pd_pd{};
    double position_velocity_pn_vn{}, position_velocity_pn_ve{}, position_velocity_pn_vd{};
    double position_velocity_pe_ve{}, position_velocity_pe_vd{}, position_velocity_pd_vd{};
    double velocity_velocity_vn_vn{}, velocity_velocity_vn_ve{}, velocity_velocity_vn_vd{};
    double velocity_velocity_ve_ve{}, velocity_velocity_ve_vd{}, velocity_velocity_vd_vd{};
    bool operator==(const PositionVelocityCovariance&) const = default;
};

/// @brief Owned navigation values passed to an optional backend sink.
/// MEL receipt validates finite values, supported state and field ranges,
/// and requires strictly increasing system time per Image attachment after
/// acceptance. All fields are copied unchanged, without normalization, clock
/// mapping or coordinate conversion. Receipt does not establish pointing,
/// sensor processing, navigation quality, or association with imagery.
/// @see INavigationSink, @ref md_docs_2navigation-reception "Navigation reception"
struct NavigationReport {
    /// Positive nanoseconds on the source's unspecified epoch, not Image time.
    std::chrono::nanoseconds system_time{};
    NavigationSolutionState state{NavigationSolutionState::aligning};
    /// Latitude [-pi/2, pi/2] and longitude [-pi, pi] in radians; altitude in metres.
    double latitude{}, longitude{}, altitude{};
    NavigationEuler attitude;
    NavigationAttitudeRate attitude_rate;
    NavigationNorthEastDown speed, acceleration;
    /// Wander angle in radians; magnetic heading and barometric altitude MSL
    /// copied as supplied, without additional unit or datum assumptions.
    double wander_angle{}, magnetic_heading{}, altitude_msl{};
    PositionVelocityCovariance position_velocity_covariance_uncertainty;
    bool operator==(const NavigationReport&) const = default;
};

} // namespace gige_mel::backend
