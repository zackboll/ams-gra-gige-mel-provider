#include "channels.hpp"
#include "channel_core.hpp"
#include "unsupported_callbacks.hpp"
#include "health_snapshot.hpp"
#include "tasking.hpp"
#include "test_hooks.hpp"
#include <algorithm>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

class C2ChannelImpl final : public ChannelCore, public ir::C2Channel {
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::send;
    using ChannelCore::registerMetadataCallback;
    mel::RequestFor<ir::CameraCommandResp> send(ir::CameraCommand command) override { return camera_command(command); }
    mel::RequestFor<ir::Return> send(ir::BIT_Command command) override {
        if (in_provider_callback())
            return failed<ir::Return>(mel::ErrorCode::InvalidState, "schedule BIT outside provider callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::Return>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        ConfigurationOutcome outcome;
        std::optional<mel::BIT_Status> payload;
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<ir::Return>(mel::ErrorCode::InvalidState, "channel is detached");
            const bool initiate = !command.getInitiateBIT_ID().empty();
            const auto& selected = initiate ? command.getInitiateBIT_ID() : command.getCancelBIT_ID();
            const auto reject = [&](backend::ErrorCode code, const char* message) {
                outcome = ConfigurationOutcome::failure({code, message});
            };
            if (!selected.empty()) {
                if (selected.size() != 1)
                    reject(backend::ErrorCode::invalid_argument, "selected BIT operation requires exactly one test ID");
                else if (selected.front() != 1)
                    reject(backend::ErrorCode::not_found, "unknown BIT test ID; only numeric ID 1 is recognized");
                else if (!initiate)
                    reject(backend::ErrorCode::invalid_state, "no active test: software BIT is synchronous and has no queued work");
                else {
                    try {
                        auto snapshot = health_snapshot(*runtime_, true);
                        if (snapshot.bit_error) std::rethrow_exception(snapshot.bit_error);
                        payload = std::move(snapshot.bit);
                        outcome.success = true;
                    } catch (const std::exception& error) {
                        outcome = ConfigurationOutcome::failure({backend::ErrorCode::resource_exhausted,
                            std::string("BIT report preparation failed: ") + error.what()});
                        runtime_->report(outcome.description);
                    } catch (...) {
                        reject(backend::ErrorCode::resource_exhausted, "BIT report preparation failed: unknown internal failure");
                        runtime_->report(outcome.description);
                    }
                }
            } else if (!command.getClearFaultCode().empty())
                reject(backend::ErrorCode::unsupported, "clearFaultCode is unsupported; real recovery retires current software faults");
            else reject(backend::ErrorCode::invalid_argument, "BIT command has no selected operation");
        }
        auto result = outcome.error ? failed<ir::Return>(map_error(outcome.error->code), outcome.description)
                                    : ready(ir::Return::Success);
        if (payload) {
            Callback<mel::BIT_Status> callback;
            { std::lock_guard lock(callback_mutex_); callback = bit_callback_; }
            invoke(callback, *payload);
        }
        on_command_status(outcome.status(command.getCommandID()));
        return result;
    }
    // GIGE-MEL-042: unsupported, unadvertised families. Each returns its typed
    // Unsupported error and, once admitted, one correlated terminal rejection.
    mel::RequestFor<mel::CalibrationConfiguration> send(ir::CalibrationConfigurationCmd command) override {
        return reject_unsupported<mel::CalibrationConfiguration>(command,
            "CalibrationConfigurationCmd is unsupported: calibration configuration is not available in the experimental image-stream profile");
    }
    mel::RequestFor<mel::CalibrationStatus> send(ir::CalibrationStatusCmd command) override {
        return reject_unsupported<mel::CalibrationStatus>(command,
            "CalibrationStatusCmd is unsupported: calibration status is not available in the experimental image-stream profile");
    }
    mel::RequestFor<ir::CameraProtectCmdResp> send(ir::CameraProtectCmd command) override {
        return reject_unsupported<ir::CameraProtectCmdResp>(command,
            "CameraProtectCmd is unsupported: camera protection is not available in the experimental image-stream profile");
    }
    mel::RequestFor<ir::EraseCommandType> send(ir::EraseCommand command) override {
        return reject_unsupported<ir::EraseCommandType>(command,
            "EraseCommand is unsupported: erase and zeroize are not available in the experimental image-stream profile");
    }
    // The upstream response type is CommandStatus; the future still carries the
    // Unsupported error, never a successful (rejection-valued) payload.
    mel::RequestFor<ir::CommandStatus> send(ir::SystemTrackDataResponse command) override {
        return reject_unsupported<ir::CommandStatus>(command,
            "SystemTrackDataResponse is unsupported: system track data input is not available in the experimental image-stream profile");
    }
    mel::RequestFor<ir::MFA_Mode> send(ir::ModeCmd command) override {
        // Same admission precedence as configuration/BIT: no recursive status
        // dispatch or operation-lock wait from any provider callback thread.
        if (in_provider_callback())
            return failed<ir::MFA_Mode>(mel::ErrorCode::InvalidState, "submit ModeCmd outside provider callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::MFA_Mode>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        ConfigurationOutcome outcome;
        Callback<ir::CommandStatus> callback;
        std::optional<ir::CommandStatus> status;
        mel::RequestFor<ir::MFA_Mode> result;
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<ir::MFA_Mode>(mel::ErrorCode::InvalidState, "channel is detached");
            const auto state = command.getState();
            const auto mode = command.getMode();
            const bool operational = state == mel::MFA_State::Operate || state == mel::MFA_State::OperateRxOnly ||
                                     state == mel::MFA_State::OperateTxOnly;
            const bool compatible_mode = operational ? mode != ir::MFA_Mode::Unused : mode == ir::MFA_Mode::Unused;
            const auto reject = [&](backend::ErrorCode code, const char* message) {
                outcome = ConfigurationOutcome::failure({code, message});
            };
            if (state <= mel::MFA_State::NotSet || state >= mel::MFA_State::MaxExclusive ||
                mode > ir::MFA_Mode::ScanBarSched || !compatible_mode)
                reject(backend::ErrorCode::invalid_argument, "invalid ModeCmd state/mode combination");
            else if (state == mel::MFA_State::Standby && !unset_scan(command.getScanParameters()))
                reject(backend::ErrorCode::invalid_argument, "Standby requires every ScanParam field at its upstream default");
            else if (state == mel::MFA_State::Operate && mode == ir::MFA_Mode::TaskSched &&
                     backend::valid_task_support(runtime_->task_support)) {
                bool valid, active;
                { std::lock_guard lock(runtime_->state_mutex);
                  valid = runtime_->initialized && runtime_->configuration_valid;
                  active = runtime_->image_active; }
                if (!unset_scan(command.getScanParameters()))
                    reject(backend::ErrorCode::invalid_argument, "TaskSched does not accept scan parameters");
                else if (!valid || active)
                    reject(backend::ErrorCode::invalid_state, "TaskSched requires valid idle Image/Stack ownership");
                else if (runtime_->task_session) outcome.success = true;
                else {
                    auto* source = dynamic_cast<backend::ITaskExecutionSource*>(runtime_->device.get());
                    auto opened = task_call([&]() -> backend::Result<std::unique_ptr<backend::ITaskExecutionSession>> {
                        if (!source || source->task_execution_support() != runtime_->task_support)
                            return backend::Error{backend::ErrorCode::unsupported, "task source declaration changed"};
                        return source->open_task_session(task_deadline());
                    });
                    if (!opened) outcome = ConfigurationOutcome::failure(opened.error());
                    else if (!opened.value()) reject(backend::ErrorCode::io_error, "null task session");
                    else {
                        auto observed = task_call([&] { return opened.value()->observe(task_deadline()); });
                        if (!observed || observed.value().phase != backend::TaskPhase::idle || observed.value().task) {
                            opened.value()->retire();
                            reject(backend::ErrorCode::io_error, "new task session did not establish idle admission state");
                        } else {
                            runtime_->task_session = std::move(opened).value();
                            runtime_->task_admissions = 0;
                            { std::lock_guard lock(runtime_->state_mutex); runtime_->tasking_active = true; }
                            outcome.success = true;
                        }
                    }
                }
            }
            else if (state != mel::MFA_State::Standby)
                reject(backend::ErrorCode::unsupported, "only Standby/Unused and source-qualified Operate/TaskSched are implemented");
            else {
                bool valid, active;
                {
                    // Do not prepare a Health snapshot: it may commit BIT
                    // observations. Readiness and ownership are stable against
                    // lifecycle/configuration changes under operations.
                    std::lock_guard lock(runtime_->state_mutex);
                    valid = runtime_->initialized && runtime_->configuration_valid;
                    active = runtime_->image_active;
                }
                if (!valid)
                    reject(backend::ErrorCode::invalid_state, "Standby requires initialized provider and known-valid configuration");
                else if (runtime_->task_session) {
                    auto observed = task_call([&] { return runtime_->task_session->observe(task_deadline()); });
                    if (!observed) outcome = ConfigurationOutcome::failure(observed.error());
                    else {
                        const auto phase = observed.value().phase;
                        if (phase == backend::TaskPhase::admitted || phase == backend::TaskPhase::executing) {
                            auto cancelled = task_call([&] { return runtime_->task_session->cancel_current(task_deadline()); });
                            if (!cancelled) outcome = ConfigurationOutcome::failure(cancelled.error());
                            else outcome.success = true;
                        } else outcome.success = true;
                        if (outcome.success) retire_tasking(*runtime_);
                    }
                }
                else if (active) {
                    const auto image_pin = runtime_->acquisition_channel.lock();
                    if (!image_pin || image_pin->is_detached())
                        reject(backend::ErrorCode::invalid_state, "owned acquisition is unavailable for shutdown");
                    else {
                        // Reuse disable's Image implementation, not disable()
                        // itself: operations is already held. No buffer release,
                        // unbinding, detach or diagnostic-history reset.
                        image_pin->stop_auxiliary_locked();
                        image_pin->stop_locked();
                        outcome.success = true;
                    }
                } else outcome.success = true; // Idempotent, no backend I/O.
            }
            result = outcome.error ? failed<ir::MFA_Mode>(map_error(outcome.error->code), outcome.description)
                                   : ready(command.getMode());
            status.emplace(outcome.status(command.getCommandID()));
            { std::lock_guard lock(callback_mutex_); callback = status_callback_; }
            admit_callback(callback);
        }
        invoke(callback, *status, nullptr, true);
        return result;
    }
    mel::RequestFor<ir::Return> send(ir::ConfigSetCommand command) override { return config_set(command); }
    ir::Return registerMetadataCallback(Callback<mel::BIT_Configuration> callback) override {
        return register_initial(catalogue_callback_, std::move(callback), [&] { return bit_catalogue(); });
    }
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::CommandStatus* const)> callback) override {
        return register_callback(status_callback_, std::move(callback));
    }
    GIGE_UNSUPPORTED_CALLBACK(mel::CalibrationConfiguration)
    GIGE_UNSUPPORTED_CALLBACK(ir::CameraProtectCmdResp)
    GIGE_UNSUPPORTED_CALLBACK(mel::CalibrationStatus)
    ir::Return registerMetadataCallback(Callback<mel::BIT_Status> callback) override {
        return register_initial(bit_callback_, std::move(callback), [&] {
            auto snapshot = health_snapshot(*runtime_);
            if (snapshot.bit_error) std::rethrow_exception(snapshot.bit_error);
            return std::move(*snapshot.bit);
        });
    }
private:
    // Established C2 admission precedence (as ConfigSet/BIT/ModeCmd): reject any
    // provider-callback context before locking or status dispatch, pin Control
    // and this channel, then linearize with detach under operations. Admission
    // touches no backend, configuration, acquisition, BIT, navigation or Health
    // state.
    //
    // GIGE-MEL-042-R1: the registered CommandStatus callback is snapshotted and
    // reserved (admit_callback) while this request is still admitted under
    // operations. Detach sets detached_ and drains reservations under the same
    // lock, so either detach precedes admission (InvalidState, no callback) or
    // detach waits until this admitted status has been dispatched. The ready
    // future and status are formed before admission; nothing that can throw
    // runs between the reservation and invoke, which contains callback
    // exceptions and always releases the reservation. No provider lock is held
    // while the consumer callback runs.
    template <class Response, class Request>
    mel::RequestFor<Response> reject_unsupported(const Request& request, const char* description) {
        if (in_provider_callback())
            return failed<Response>(mel::ErrorCode::InvalidState, "submit C2 commands outside provider callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<Response>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        const auto outcome = ConfigurationOutcome::failure({backend::ErrorCode::unsupported, description});
        auto result = failed<Response>(map_error(outcome.error->code), outcome.description);
        const auto status = outcome.status(request.getCommandID());
        GIGE_MEL_UNSUPPORTED_STATUS_SEAM(before_admission);
        Callback<ir::CommandStatus> callback;
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<Response>(mel::ErrorCode::InvalidState, "channel is detached");
            { std::lock_guard lock(callback_mutex_); callback = status_callback_; }
            admit_callback(callback); // Last throwing step; reserves only a registered callback.
        }
        GIGE_MEL_UNSUPPORTED_STATUS_SEAM(after_reservation);
        invoke(callback, status, nullptr, true);
        return result;
    }
    template <class T, class Prepare>
    ir::Return register_initial(Callback<T>& slot, Callback<T> callback, Prepare prepare) {
        if (in_provider_callback() || is_detached()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        const auto self = shared_from_this();
        if (!callback) return is_detached() ? ir::Return::Fail : ir::Return::BadPointer;
        std::optional<T> payload;
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return ir::Return::Fail;
            if (!callback) return ir::Return::BadPointer;
            { std::lock_guard lock(callback_mutex_); if (slot) return ir::Return::Fail; }
            try { payload.emplace(prepare()); }
            catch (const std::exception& error) { runtime_->report(error.what()); return ir::Return::Fail; }
            catch (...) { runtime_->report("BIT initial publication preparation failed"); return ir::Return::Fail; }
            const auto registered = register_callback(slot, callback);
            if (registered != ir::Return::Success) return registered;
        }
        invoke(callback, *payload);
        return ir::Return::Success;
    }
    void on_command_status(const ir::CommandStatus& status) override {
        Callback<ir::CommandStatus> callback;
        { std::lock_guard lock(callback_mutex_); callback = status_callback_; }
        invoke(callback, status);
    }
    Callback<ir::CommandStatus> status_callback_;
    Callback<mel::BIT_Configuration> catalogue_callback_;
    Callback<mel::BIT_Status> bit_callback_;
};

std::shared_ptr<ChannelCore> make_c2_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config) {
    return std::make_shared<C2ChannelImpl>(runtime, config);
}

} // namespace provider_detail
} // namespace gige_mel
