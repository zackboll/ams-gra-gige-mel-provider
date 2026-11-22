#include "navigation_translation.hpp"
#include <cmath>
#include <numbers>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

std::optional<std::string> navigation_error(const mel::NavigationReport& report) {
    if (report.getSystemTime() <= 0ns) return "systemTime must be positive nanoseconds";
    switch (report.getState()) {
    case mel::PositionSolutionState::Aligning:
    case mel::PositionSolutionState::FreeInertial:
    case mel::PositionSolutionState::GPS:
    case mel::PositionSolutionState::Blended: break;
    default: return "state must be Aligning, FreeInertial, GPS or Blended";
    }
    if (!std::isfinite(report.getLatitude()) || report.getLatitude() < -std::numbers::pi / 2 ||
        report.getLatitude() > std::numbers::pi / 2) return "latitude must be finite and within [-pi/2, pi/2] radians";
    if (!std::isfinite(report.getLongitude()) || report.getLongitude() < -std::numbers::pi ||
        report.getLongitude() > std::numbers::pi) return "longitude must be finite and within [-pi, pi] radians";
    const auto rate = report.getAttitudeRate();
    if (rate.getAttitudeRateTime() < 0ns) return "attitudeRate.attitudeRateTime must be nonnegative nanoseconds";
    const auto covariance = report.getPositionVelocityCovarianceUncertainty();
    struct Field { const char* name; double value; bool variance = false; };
    const Field fields[]{
        {"altitude", report.getAltitude()}, {"altitudeMSL", report.getAltitudeMSL()},
        {"attitude.roll", report.getAttitude().getRoll()},
        {"attitude.pitch", report.getAttitude().getPitch()},
        {"attitude.yaw", report.getAttitude().getYaw()},
        {"attitudeRate.roll", rate.getAttitudeRate().getRoll()},
        {"attitudeRate.pitch", rate.getAttitudeRate().getPitch()},
        {"attitudeRate.yaw", rate.getAttitudeRate().getYaw()},
        {"speed.north", report.getSpeed().getNorth()}, {"speed.east", report.getSpeed().getEast()},
        {"speed.down", report.getSpeed().getDown()},
        {"acceleration.north", report.getAcceleration().getNorth()},
        {"acceleration.east", report.getAcceleration().getEast()},
        {"acceleration.down", report.getAcceleration().getDown()},
        {"wanderAngle", report.getWanderAngle()}, {"magneticHeading", report.getMagneticHeading()},
        {"covariance.positionPositionPnPn", covariance.getPositionPositionPnPn(), true},
        {"covariance.positionPositionPnPe", covariance.getPositionPositionPnPe()},
        {"covariance.positionPositionPnPd", covariance.getPositionPositionPnPd()},
        {"covariance.positionPositionPePe", covariance.getPositionPositionPePe(), true},
        {"covariance.positionPositionPePd", covariance.getPositionPositionPePd()},
        {"covariance.positionPositionPdPd", covariance.getPositionPositionPdPd(), true},
        {"covariance.positionVelocityPnVn", covariance.getPositionVelocityPnVn()},
        {"covariance.positionVelocityPnVe", covariance.getPositionVelocityPnVe()},
        {"covariance.positionVelocityPnVd", covariance.getPositionVelocityPnVd()},
        {"covariance.positionVelocityPeVe", covariance.getPositionVelocityPeVe()},
        {"covariance.positionVelocityPeVd", covariance.getPositionVelocityPeVd()},
        {"covariance.positionVelocityPdVd", covariance.getPositionVelocityPdVd()},
        {"covariance.velocityVelocityVnVn", covariance.getVelocityVelocityVnVn(), true},
        {"covariance.velocityVelocityVnVe", covariance.getVelocityVelocityVnVe()},
        {"covariance.velocityVelocityVnVd", covariance.getVelocityVelocityVnVd()},
        {"covariance.velocityVelocityVeVe", covariance.getVelocityVelocityVeVe(), true},
        {"covariance.velocityVelocityVeVd", covariance.getVelocityVelocityVeVd()},
        {"covariance.velocityVelocityVdVd", covariance.getVelocityVelocityVdVd(), true},
    };
    for (const auto& field : fields) {
        if (!std::isfinite(field.value)) return std::string(field.name) + " must be finite";
        if (field.variance && field.value < 0) return std::string(field.name) + " must be nonnegative";
    }
    return std::nullopt;
}

backend::NavigationReport backend_navigation(const mel::NavigationReport& report) noexcept {
    backend::NavigationReport value;
    switch (report.getState()) {
    case mel::PositionSolutionState::Aligning: value.state = backend::NavigationSolutionState::aligning; break;
    case mel::PositionSolutionState::FreeInertial: value.state = backend::NavigationSolutionState::free_inertial; break;
    case mel::PositionSolutionState::GPS: value.state = backend::NavigationSolutionState::gps; break;
    case mel::PositionSolutionState::Blended: value.state = backend::NavigationSolutionState::blended; break;
    default: break; // Rejected before conversion.
    }
    value.system_time = report.getSystemTime();
    value.latitude = report.getLatitude();
    value.longitude = report.getLongitude();
    value.altitude = report.getAltitude();
    const auto& attitude = report.getAttitude();
    value.attitude = {attitude.getRoll(), attitude.getPitch(), attitude.getYaw()};
    const auto rate = report.getAttitudeRate();
    const auto& angles = rate.getAttitudeRate();
    value.attitude_rate = {{angles.getRoll(), angles.getPitch(), angles.getYaw()}, rate.getAttitudeRateTime()};
    const auto& speed = report.getSpeed();
    value.speed = {speed.getNorth(), speed.getEast(), speed.getDown()};
    const auto& acceleration = report.getAcceleration();
    value.acceleration = {acceleration.getNorth(), acceleration.getEast(), acceleration.getDown()};
    value.wander_angle = report.getWanderAngle();
    value.magnetic_heading = report.getMagneticHeading();
    value.altitude_msl = report.getAltitudeMSL();
    const auto covariance = report.getPositionVelocityCovarianceUncertainty();
    value.position_velocity_covariance_uncertainty = {
        covariance.getPositionPositionPnPn(), covariance.getPositionPositionPnPe(), covariance.getPositionPositionPnPd(),
        covariance.getPositionPositionPePe(), covariance.getPositionPositionPePd(), covariance.getPositionPositionPdPd(),
        covariance.getPositionVelocityPnVn(), covariance.getPositionVelocityPnVe(), covariance.getPositionVelocityPnVd(),
        covariance.getPositionVelocityPeVe(), covariance.getPositionVelocityPeVd(), covariance.getPositionVelocityPdVd(),
        covariance.getVelocityVelocityVnVn(), covariance.getVelocityVelocityVnVe(), covariance.getVelocityVelocityVnVd(),
        covariance.getVelocityVelocityVeVe(), covariance.getVelocityVelocityVeVd(), covariance.getVelocityVelocityVdVd()};
    return value;
}

} // namespace provider_detail
} // namespace gige_mel
