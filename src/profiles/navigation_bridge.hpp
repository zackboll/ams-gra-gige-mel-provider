#pragma once

// Internal, uninstalled host-side composition (GIGE-MEL-047). Connects an
// optional profile navigation handler to an explicitly provided backend-owned
// submission port. Not part of the profile-module boundary, not a transport and
// not a production control grant. No MEL, SDK, socket or register access.
#include "navigation_handler.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace gige_mel::backend::profiles {

/// Stage reached by a port for one owned operation. Only `accepted` is success.
enum class NavigationCompletion {
    not_submitted, ///< Definitely no external effect was started (requires a diagnostic code).
    accepted,      ///< The backend-owned boundary accepted the work; no transport/detector claim.
    uncertain      ///< Effects may have occurred but completion is unknown.
};

/// @brief Port evidence for one operation, interpreted conservatively by the bridge.
struct NavigationExecution {
    NavigationCompletion completion{NavigationCompletion::uncertain};
    /// Definite reason when not_submitted (e.g. busy, disconnected, unsupported).
    ErrorCode code{ErrorCode::io_error};
    std::string diagnostic;
    /// Qualified control-loss evidence. Sticky for the bridge; after loss even a
    /// reported acceptance is treated as uncertain.
    bool control_lost{false};
};

/// @brief Backend-owned recording/submission boundary for prepared operations.
/// The backend constructs it with whatever device resources it owns; the bridge
/// owns it from then on and destroys it only after admitted work has drained.
class INavigationOperationPort {
public:
    virtual ~INavigationOperationPort() = default;
    /// @brief Synchronously take ownership of one prepared operation.
    /// @return Evidence of what happened. Never retry an uncertain write or
    /// commit internally. Bounded work only: no provider or consumer calls, no
    /// reentry into the bridge or device, no detached workers or unbounded queue.
    /// Exceptions are treated as possible effects (uncertain).
    virtual NavigationExecution submit(NavigationOperation operation) = 0;
};

/// @brief Reusable bridge from INavigationSink submission to a profile handler.
/// Owns a reference to the selected profile state (so the handler, and any
/// module code behind it, outlives every admitted call) and the port. Admits
/// one submission at a time without queueing. Failures map to backend::Status:
///
/// | Situation | Status | Port invoked |
/// | --- | --- | --- |
/// | Port `accepted` | success (backend acceptance only) | yes |
/// | Handler `unsupported` / `invalid_argument` / other Error | that Error | no |
/// | Handler exception | `invalid_state` (`resource_exhausted` for allocation) | no |
/// | Malformed operation (empty format, altered report) | `invalid_state` | no |
/// | Port `not_submitted` | port code (`disconnected` after control loss) | yes |
/// | Port `uncertain`, exception, or acceptance after control loss | `timeout` (uncertain) | yes |
/// | Earlier unresolved/lost outcome; concurrent; reentrant; retired | `invalid_state` / `disconnected` / `busy` | no |
///
/// "Altered" means not representation-identical: every floating-point field,
/// including all 18 covariance entries, is compared by its IEEE 754 bit
/// pattern, so a change from +0.0 to -0.0 is refused although the values
/// compare equal numerically. Other fields use ordinary equality.
///
/// `timeout` is the existing INavigationSink encoding for uncertain acceptance.
/// Uncertainty and control loss are sticky: later submissions are refused
/// without invoking the handler or port. Nothing is retried automatically.
class ProfileNavigationBridge final {
public:
    /// @brief Build a bridge only when `state` provides the optional capability.
    /// @return `unsupported` when the profile has no navigation handler (the
    /// device should then expose no INavigationSink at all), `invalid_argument`
    /// for null inputs. No handler or port call is made here.
    static Result<std::unique_ptr<ProfileNavigationBridge>> create(
        std::shared_ptr<const ICameraProfileState> state, std::unique_ptr<INavigationOperationPort> port);

    ProfileNavigationBridge(const ProfileNavigationBridge&) = delete;
    ProfileNavigationBridge& operator=(const ProfileNavigationBridge&) = delete;
    /// Equivalent to shutdown(); must not be invoked from inside a submission.
    ~ProfileNavigationBridge();

    /// @brief Prepare the complete report with the profile handler, then submit it.
    /// Synchronous on the caller's thread; suitable for INavigationSink. The
    /// report is borrowed for the call only. See the class mapping table.
    Status submit(const NavigationReport& report);

    /// @brief Close admission, drain any active submission, then retire.
    /// Releases the port and profile state outside the internal lock after the
    /// drain. Idempotent; concurrent callers wait for retirement. Returns false,
    /// without changing state, for same-thread reentry from handler or port.
    bool shutdown();

    /// True once shutdown has completed.
    bool retired() const;

private:
    ProfileNavigationBridge(std::shared_ptr<const ICameraProfileState>, const IProfileNavigationHandler&,
                            std::unique_ptr<INavigationOperationPort>);
    Status run(const IProfileNavigationHandler&, INavigationOperationPort&, const NavigationReport&);

    enum class Phase { open, closing, retired };
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Phase phase_{Phase::open};
    bool unresolved_{false}, lost_{false};
    std::thread::id active_, retiring_;
    std::shared_ptr<const ICameraProfileState> state_;  // owner of handler_
    const IProfileNavigationHandler* handler_;           // borrowed from *state_
    std::unique_ptr<INavigationOperationPort> port_;
};

} // namespace gige_mel::backend::profiles
