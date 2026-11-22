#include "navigation_bridge.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace gige_mel::backend::profiles {
namespace {
Error refused(ErrorCode code, std::string text) { return {code, "profile navigation: " + std::move(text)}; }

// Representation-preserving report comparison. The handler must return the
// complete report unchanged, so every binary64 field is compared by its IEEE 754
// bit pattern: unlike the numeric operator==, this distinguishes +0.0 from -0.0
// (and NaN payloads, although MEL receipt admits finite values only). Fields are
// compared one by one, never with memcmp, because padding bytes carry no value.
// Non-floating fields use their ordinary equality.
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::radix == 2 &&
              std::numeric_limits<double>::digits == 53 && std::numeric_limits<double>::max_exponent == 1024 &&
              sizeof(double) == sizeof(std::uint64_t), "navigation comparison requires IEEE 754 binary64");

bool identical(const double& a, const double& b) noexcept {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// Completeness guards: each aggregate below is fully compared. The size checks
// for all-double aggregates and the structured bindings for mixed aggregates
// fail to compile if a member is added or removed.
bool identical(const NavigationEuler& a, const NavigationEuler& b) noexcept {
    static_assert(sizeof(NavigationEuler) == 3 * sizeof(double), "compare every NavigationEuler member");
    return identical(a.roll, b.roll) && identical(a.pitch, b.pitch) && identical(a.yaw, b.yaw);
}

bool identical(const NavigationNorthEastDown& a, const NavigationNorthEastDown& b) noexcept {
    static_assert(sizeof(NavigationNorthEastDown) == 3 * sizeof(double), "compare every NavigationNorthEastDown member");
    return identical(a.north, b.north) && identical(a.east, b.east) && identical(a.down, b.down);
}

bool identical(const NavigationAttitudeRate& a, const NavigationAttitudeRate& b) noexcept {
    [[maybe_unused]] const auto& [m0, m1] = a; // exactly two members
    return identical(a.attitude_rate, b.attitude_rate) && a.attitude_rate_time == b.attitude_rate_time;
}

bool identical(const PositionVelocityCovariance& a, const PositionVelocityCovariance& b) noexcept {
    using C = PositionVelocityCovariance;
    static constexpr double C::*entries[]{
        &C::position_position_pn_pn, &C::position_position_pn_pe, &C::position_position_pn_pd,
        &C::position_position_pe_pe, &C::position_position_pe_pd, &C::position_position_pd_pd,
        &C::position_velocity_pn_vn, &C::position_velocity_pn_ve, &C::position_velocity_pn_vd,
        &C::position_velocity_pe_ve, &C::position_velocity_pe_vd, &C::position_velocity_pd_vd,
        &C::velocity_velocity_vn_vn, &C::velocity_velocity_vn_ve, &C::velocity_velocity_vn_vd,
        &C::velocity_velocity_ve_ve, &C::velocity_velocity_ve_vd, &C::velocity_velocity_vd_vd};
    static_assert(std::size(entries) == 18 && sizeof(C) == 18 * sizeof(double), "compare all 18 covariance entries");
    return std::all_of(std::begin(entries), std::end(entries),
                       [&](double C::*entry) { return identical(a.*entry, b.*entry); });
}

bool identical(const NavigationReport& a, const NavigationReport& b) noexcept {
    // Exactly 13 members: time, state, 3 position, attitude, rate, speed,
    // acceleration, 3 scalars, covariance.
    [[maybe_unused]] const auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12] = a;
    return a.system_time == b.system_time && a.state == b.state &&
           identical(a.latitude, b.latitude) && identical(a.longitude, b.longitude) &&
           identical(a.altitude, b.altitude) && identical(a.attitude, b.attitude) &&
           identical(a.attitude_rate, b.attitude_rate) && identical(a.speed, b.speed) &&
           identical(a.acceleration, b.acceleration) && identical(a.wander_angle, b.wander_angle) &&
           identical(a.magnetic_heading, b.magnetic_heading) && identical(a.altitude_msl, b.altitude_msl) &&
           identical(a.position_velocity_covariance_uncertainty, b.position_velocity_covariance_uncertainty);
}
}

Result<std::unique_ptr<ProfileNavigationBridge>> ProfileNavigationBridge::create(
    std::shared_ptr<const ICameraProfileState> state, std::unique_ptr<INavigationOperationPort> port) {
    if (!state || !port) return refused(ErrorCode::invalid_argument, "bridge requires a profile state and a port");
    const auto* handler = navigation_handler(*state);
    if (!handler) return refused(ErrorCode::unsupported, "selected camera profile provides no navigation handler");
    return std::unique_ptr<ProfileNavigationBridge>(
        new ProfileNavigationBridge(std::move(state), *handler, std::move(port)));
}

ProfileNavigationBridge::ProfileNavigationBridge(std::shared_ptr<const ICameraProfileState> state,
                                                 const IProfileNavigationHandler& handler,
                                                 std::unique_ptr<INavigationOperationPort> port)
    : state_(std::move(state)), handler_(&handler), port_(std::move(port)) {}

ProfileNavigationBridge::~ProfileNavigationBridge() {
    // Destroying the bridge from its own handler or port would free code and
    // data that are still executing; refuse loudly rather than continue.
    if (!shutdown()) std::terminate();
}

bool ProfileNavigationBridge::retired() const {
    std::lock_guard lock(mutex_);
    return phase_ == Phase::retired;
}

Status ProfileNavigationBridge::submit(const NavigationReport& report) {
    const IProfileNavigationHandler* handler = nullptr;
    INavigationOperationPort* port = nullptr;
    {
        std::lock_guard lock(mutex_);
        const auto self = std::this_thread::get_id();
        if (active_ == self || retiring_ == self)
            return refused(ErrorCode::invalid_state, "reentrant submission from the handler or port is refused");
        if (phase_ != Phase::open) return refused(ErrorCode::disconnected, "bridge is closing or retired");
        if (lost_) return refused(ErrorCode::disconnected, "control was lost; no further submission");
        if (unresolved_)
            return refused(ErrorCode::invalid_state, "an earlier uncertain submission is unresolved; no retry");
        if (active_ != std::thread::id{})
            return refused(ErrorCode::busy, "another navigation submission is active (no queue)");
        active_ = self;
        handler = handler_;
        port = port_.get();
    }
    // state_ and port_ stay owned until shutdown() observes active_ cleared.
    std::optional<Status> status;
    try {
        status.emplace(run(*handler, *port, report));
    } catch (...) {
        // Only an allocation failure while building the diagnostic can reach
        // here. The port may have been entered, so never claim a definite outcome.
        std::lock_guard lock(mutex_);
        unresolved_ = true;
        active_ = {};
        changed_.notify_all();
        throw;
    }
    {
        std::lock_guard lock(mutex_);
        active_ = {};
    }
    changed_.notify_all();
    return std::move(*status);
}

Status ProfileNavigationBridge::run(const IProfileNavigationHandler& handler, INavigationOperationPort& port,
                                    const NavigationReport& report) {
    // Stage 1: profile preparation. Every failure here is definite: the port
    // has not been invoked, so nothing can have been transmitted.
    std::optional<NavigationOperation> operation;
    try {
        auto prepared = handler.prepare_navigation(report);
        if (!prepared) return prepared.error();
        operation.emplace(std::move(prepared).value());
    } catch (const std::bad_alloc&) {
        return refused(ErrorCode::resource_exhausted, "handler allocation failed before submission");
    } catch (const std::exception& error) {
        return refused(ErrorCode::invalid_state, std::string("handler threw before submission: ") + error.what());
    } catch (...) {
        return refused(ErrorCode::invalid_state, "handler threw an unknown exception before submission");
    }
    if (operation->format.empty())
        return refused(ErrorCode::invalid_state, "handler returned an operation without a format; not submitted");
    if (!identical(operation->report, report)) // bit-for-bit, not numeric equality
        return refused(ErrorCode::invalid_state, "handler altered the report copy; not submitted");

    // Stage 2: backend-owned port. After entry, absence of proof is uncertainty.
    NavigationExecution evidence;
    try {
        evidence = port.submit(std::move(*operation));
    } catch (...) {
        evidence = {NavigationCompletion::uncertain, ErrorCode::io_error, "port threw after entry", false};
    }
    std::lock_guard lock(mutex_);
    if (evidence.control_lost) lost_ = true;
    switch (evidence.completion) {
    case NavigationCompletion::not_submitted:
        if (lost_) return refused(ErrorCode::disconnected, "control lost before submission: " + evidence.diagnostic);
        return refused(evidence.code, "port did not submit: " + evidence.diagnostic);
    case NavigationCompletion::accepted:
        if (!lost_) return {};
        break; // Acceptance reported with or after control loss is not trusted.
    case NavigationCompletion::uncertain:
        break;
    }
    unresolved_ = true;
    return refused(ErrorCode::timeout, std::string("backend acceptance is uncertain") +
        (lost_ ? " after control loss" : "") + (evidence.diagnostic.empty() ? "" : ": " + evidence.diagnostic));
}

bool ProfileNavigationBridge::shutdown() {
    std::shared_ptr<const ICameraProfileState> state;
    std::unique_ptr<INavigationOperationPort> port;
    {
        std::unique_lock lock(mutex_);
        const auto self = std::this_thread::get_id();
        if (active_ == self || retiring_ == self) return false;
        if (phase_ == Phase::retired) return true;
        if (phase_ == Phase::closing) {
            changed_.wait(lock, [&] { return phase_ == Phase::retired; });
            return true;
        }
        phase_ = Phase::closing; // Admission closes before the drain.
        retiring_ = self;
        changed_.wait(lock, [&] { return active_ == std::thread::id{}; });
        handler_ = nullptr;
        port = std::move(port_);
        state = std::move(state_);
    }
    // Retire outside the lock: the port first (it may use backend resources),
    // then this bridge's reference to the profile state (possibly module code).
    port.reset();
    state.reset();
    {
        std::lock_guard lock(mutex_);
        retiring_ = {};
        phase_ = Phase::retired;
    }
    changed_.notify_all();
    return true;
}

} // namespace gige_mel::backend::profiles
