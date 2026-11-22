#pragma once

#include "mel_types.hpp"
#include <irmel/library/irmel-types/CommandStatus.h>
#include <irmel/library/irmel-types/ScanParam.h>
#include <future>
#include <optional>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/// Settle a request before return with a shared payload moved from @p value.
/// Allocation or future construction may throw; no worker is launched.
template <class T> mel::RequestFor<T> ready(T value) {
    std::promise<mel::ErrorOr<std::shared_ptr<T>>> promise;
    auto result = promise.get_future();
    promise.set_value(mel::ErrorOr<std::shared_ptr<T>>(std::make_shared<T>(std::move(value))));
    return result;
}

/// Settle a request with an error value, not a future exception or a payload.
/// Constructing the error/future can itself throw.
template <class T> mel::RequestFor<T> failed(mel::ErrorCode code, std::string message) {
    std::promise<mel::ErrorOr<std::shared_ptr<T>>> promise;
    auto result = promise.get_future();
    promise.set_value(mel::ErrorOr<std::shared_ptr<T>>(mel::Error(code, std::move(message))));
    return result;
}
/// Translate backend failure categories to MEL future errors; default InvalidState.
mel::ErrorCode map_error(backend::ErrorCode code);
/// Translate backend failure categories to correlated C2 rejection reasons; default Other.
ir::CannotComply command_reason(backend::ErrorCode code);

/** @brief Per-request outcome, independent of the runtime's historical diagnostic.
 * Separates error-only futures from ordinary negative responses, so that C2
 * can form a terminal CommandStatus for either. A rejection does not imply rollback.
 */
struct ConfigurationOutcome {
    bool success{false};
    // invalid_argument/unsupported from apply are ordinary negative responses;
    // schema errors and transport/readback failures are error-only futures.
    std::optional<backend::Error> error;
    ir::CannotComply reason{ir::CannotComply::Other};
    std::string description;

    /// Retain failure description and mapped reason; copy the backend error
    /// only when the caller needs an error-only future.
    static ConfigurationOutcome failure(const backend::Error& failure, bool error_only = true) {
        return {false, error_only ? std::optional{failure} : std::nullopt,
                command_reason(failure.code), failure.message};
    }
    /// Form terminal status correlated by command ID; accepted status has no reason.
    ir::CommandStatus status(std::uint32_t id) const {
        ir::CommandStatus result;
        result.setCommandID(id);
        result.setState(success ? ir::CommandState::Accepted : ir::CommandState::Rejected);
        result.setReasonID(success ? ir::CannotComply::NotSet : reason);
        result.setReasonDescription(success ? "" : description);
        return result;
    }
};

/// Require the pinned defaults for every scan field, not valid scan geometry.
bool unset_scan(const ir::ScanParam& scan);

} // namespace provider_detail
} // namespace gige_mel
