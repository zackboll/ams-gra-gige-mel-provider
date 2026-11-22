#pragma once

#include "mel_types.hpp"
#include <irmel/library/irmel-types/CommandStatus.h>
#include <irmel/library/irmel-types/ScanParam.h>
#include <future>
#include <optional>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

template <class T> mel::RequestFor<T> ready(T value) {
    std::promise<mel::ErrorOr<std::shared_ptr<T>>> promise;
    auto result = promise.get_future();
    promise.set_value(mel::ErrorOr<std::shared_ptr<T>>(std::make_shared<T>(std::move(value))));
    return result;
}

template <class T> mel::RequestFor<T> failed(mel::ErrorCode code, std::string message) {
    std::promise<mel::ErrorOr<std::shared_ptr<T>>> promise;
    auto result = promise.get_future();
    promise.set_value(mel::ErrorOr<std::shared_ptr<T>>(mel::Error(code, std::move(message))));
    return result;
}
mel::ErrorCode map_error(backend::ErrorCode code);
ir::CannotComply command_reason(backend::ErrorCode code);

struct ConfigurationOutcome {
    bool success{false};
    // invalid_argument/unsupported from apply are ordinary negative responses;
    // schema errors and transport/readback failures are error-only futures.
    std::optional<backend::Error> error;
    ir::CannotComply reason{ir::CannotComply::Other};
    std::string description;

    static ConfigurationOutcome failure(const backend::Error& failure, bool error_only = true) {
        return {false, error_only ? std::optional{failure} : std::nullopt,
                command_reason(failure.code), failure.message};
    }
    ir::CommandStatus status(std::uint32_t id) const {
        ir::CommandStatus result;
        result.setCommandID(id);
        result.setState(success ? ir::CommandState::Accepted : ir::CommandState::Rejected);
        result.setReasonID(success ? ir::CannotComply::NotSet : reason);
        result.setReasonDescription(success ? "" : description);
        return result;
    }
};

bool unset_scan(const ir::ScanParam& scan);

} // namespace provider_detail
} // namespace gige_mel
