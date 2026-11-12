#pragma once

#include <chrono>

namespace gige_mel::backend {

// Project-owned values; no MEL or backend implementation dependencies.
enum class NavigationSolutionState { aligning, free_inertial, gps, blended };

struct NavigationEuler {
    double roll{}, pitch{}, yaw{};
    bool operator==(const NavigationEuler&) const = default;
};

struct NavigationNorthEastDown {
    double north{}, east{}, down{};
    bool operator==(const NavigationNorthEastDown&) const = default;
};

struct NavigationAttitudeRate {
    NavigationEuler attitude_rate;
    std::chrono::nanoseconds attitude_rate_time{};
    bool operator==(const NavigationAttitudeRate&) const = default;
};

struct PositionVelocityCovariance {
    double position_position_pn_pn{}, position_position_pn_pe{}, position_position_pn_pd{};
    double position_position_pe_pe{}, position_position_pe_pd{}, position_position_pd_pd{};
    double position_velocity_pn_vn{}, position_velocity_pn_ve{}, position_velocity_pn_vd{};
    double position_velocity_pe_ve{}, position_velocity_pe_vd{}, position_velocity_pd_vd{};
    double velocity_velocity_vn_vn{}, velocity_velocity_vn_ve{}, velocity_velocity_vn_vd{};
    double velocity_velocity_ve_ve{}, velocity_velocity_ve_vd{}, velocity_velocity_vd_vd{};
    bool operator==(const PositionVelocityCovariance&) const = default;
};

// Values and units are preserved from the navigation source, without clock or
// coordinate conversion. Timestamps retain their source's unspecified epoch.
struct NavigationReport {
    std::chrono::nanoseconds system_time{};
    NavigationSolutionState state{NavigationSolutionState::aligning};
    double latitude{}, longitude{}, altitude{};
    NavigationEuler attitude;
    NavigationAttitudeRate attitude_rate;
    NavigationNorthEastDown speed, acceleration;
    double wander_angle{}, magnetic_heading{}, altitude_msl{};
    PositionVelocityCovariance position_velocity_covariance_uncertainty;
    bool operator==(const NavigationReport&) const = default;
};

} // namespace gige_mel::backend
