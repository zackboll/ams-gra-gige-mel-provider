#include "channels.hpp"
#include "channel_core.hpp"
#include "unsupported_callbacks.hpp"
#include "health_snapshot.hpp"
#include "software_identity.hpp"
#include <irmel/library/health-status/HealthStatusChannel.h>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

class HealthChannelImpl final : public ChannelCore, public ir::HealthStatusChannel {
    struct Publisher {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<bool> running{true};
        bool prompted{true};
    };
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::send;
    using ChannelCore::registerMetadataCallback;
    ~HealthChannelImpl() override { stop_auxiliary_locked(); }

    ir::Return enable() override {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        if (publisher_ && publisher_->running) return ir::Return::Success;
        try {
            auto state = std::make_shared<Publisher>();
            { std::lock_guard lock(callback_mutex_); publisher_ = state; }
            worker_ = std::thread([state, weak = weak_from_this(), runtime = runtime_] {
                run(state, weak, runtime);
            });
            return ir::Return::Success;
        } catch (const std::exception& error) {
            stop_auxiliary_locked();
            runtime_->report(error.what());
            return ir::Return::Fail;
        }
    }
    ir::Return registerMetadataCallback(Callback<mel::MFA_Status> callback) override {
        const auto result = register_callback(status_callback_, std::move(callback));
        if (result == ir::Return::Success) prompt();
        return result;
    }
    ir::Return registerMetadataCallback(Callback<mel::MFA_StatusDetailed> callback) override {
        const auto result = register_callback(detailed_callback_, std::move(callback));
        if (result == ir::Return::Success) prompt();
        return result;
    }
    GIGE_UNSUPPORTED_CALLBACK(ir::LFStatus)
    ir::Return registerMetadataCallback(Callback<mel::BIT_Status> callback) override {
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        const auto self = shared_from_this();
        const auto result = register_callback(bit_callback_, std::move(callback));
        if (result == ir::Return::Success) prompt();
        return result;
    }
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::SubsystemStatusResp* const)> callback) override {
        // Same registration contract and lifetime pins as BIT_Status; dispatch
        // only on the existing Health worker, never inline on this thread.
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        const auto self = shared_from_this();
        const auto result = register_callback(subsystem_callback_, std::move(callback));
        if (result == ir::Return::Success) prompt();
        return result;
    }
    GIGE_UNSUPPORTED_CALLBACK(mel::DiscreteStatus)
    GIGE_UNSUPPORTED_CALLBACK(mel::MFA_SecurityAuditRecord)
    GIGE_UNSUPPORTED_CALLBACK(ir::NUC_TempData)

private:
    void prompt() {
        std::shared_ptr<Publisher> state;
        { std::lock_guard lock(callback_mutex_); state = publisher_; }
        if (!state) return;
        { std::lock_guard lock(state->mutex); state->prompted = true; }
        state->changed.notify_all();
    }
    void stop_auxiliary_locked() override {
        std::shared_ptr<Publisher> state;
        { std::lock_guard lock(callback_mutex_); state = publisher_; }
        if (state) {
            { std::lock_guard lock(state->mutex); state->running = false; }
            state->changed.notify_all();
        }
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) worker_.detach();
            else worker_.join();
        }
    }
    void publish(const Publisher& publisher) {
        Callback<mel::MFA_Status> status_callback;
        Callback<mel::MFA_StatusDetailed> detailed_callback;
        Callback<mel::BIT_Status> bit_callback;
        Callback<ir::SubsystemStatusResp> subsystem_callback;
        {
            std::lock_guard lock(callback_mutex_);
            status_callback = status_callback_;
            detailed_callback = detailed_callback_;
            bit_callback = bit_callback_;
            subsystem_callback = subsystem_callback_;
        }
        // Subsystem-only subscribers still need the canonical BIT commit: its
        // revision and current-fault observations feed BIT_report.
        const bool want_subsystem = static_cast<bool>(subsystem_callback);
        const auto s = health_snapshot(*runtime_, false, static_cast<bool>(bit_callback) || want_subsystem,
                                       want_subsystem);
        const auto state = !s.initialized ? mel::MFA_State::Unknown :
            !s.configuration_valid || (s.active && s.failed) ? mel::MFA_State::Degraded :
            s.active || s.tasking ? mel::MFA_State::Operate : mel::MFA_State::Standby;
        const auto description = !s.initialized ? "Provider not initialized" :
            !s.configuration_valid ? "Actual camera configuration is not known valid" :
            s.active && s.failed ? "Owned Image/Stack acquisition has observed a terminal failure; explicitly disable before recovery" :
            s.tasking ? "Virtual task-admission session owned; no physical sensor operation claimed" :
            s.active ? "Image/Stack acquisition is logically owned without an observed terminal failure" :
            "Idle with known valid configuration; explicit Image/Stack enable may be attempted (not a physical connection probe)";
        mel::MFA_Status status(state, description,
            "Experimental image/virtual-task profile; no physical task scheduling, scanning, hardware BIT or calibration",
            mel::StateTransitionStatus::NotTransitioning,
            mel::About(std::string(ProviderSoftware::name) + " (software provider)", "", ProviderSoftware::version_text, "", ""), {});
        mel::MFA_StatusDetailed detailed;
        const auto add = [&](const char* name, std::string value) {
            detailed.addStatus(mel::NameValuePair(std::string("gige_mel.") + name, std::move(value)));
        };
        add("health_schema", "1");
        add("backend_id", runtime_->backend_id);
        add("device_id", runtime_->settings.device_id);
        add("configuration_valid", s.configuration_valid ? "true" : "false");
        add("configuration_generation", std::to_string(s.generation));
        add("acquisition_state", !s.active ? "idle" : s.failed ? "faulted" : "running");
        add("delivered_frames", std::to_string(s.statistics.delivered_frames));
        add("dropped_no_buffer", std::to_string(s.statistics.dropped_no_buffer));
        add("rejected_frames", std::to_string(s.statistics.rejected_frames));
        add("callback_failures", std::to_string(s.statistics.callback_failures));
        add("stream_failures", std::to_string(s.statistics.stream_failures));
        add("last_error_history", s.statistics.last_error);
        invoke(status_callback, status, &publisher.running);
        invoke(detailed_callback, detailed, &publisher.running);
        // A failed canonical BIT preparation suppresses both BIT-derived
        // reports for this sample; Status/Detailed above remain published.
        if (s.bit) invoke(bit_callback, *s.bit, &publisher.running);
        else if (s.bit_error) {
            try { std::rethrow_exception(s.bit_error); }
            catch (const std::exception& error) { runtime_->report(std::string("Health BIT publication preparation failed: ") + error.what()); }
            catch (...) { runtime_->report("Health BIT publication preparation failed"); }
        }
        // Appended last; dispatches the revision paired with this capture.
        if (s.subsystem) invoke(subsystem_callback, *s.subsystem, &publisher.running);
        else if (s.subsystem_error) {
            try { std::rethrow_exception(s.subsystem_error); }
            catch (const std::exception& error) { runtime_->report(std::string("Health subsystem publication preparation failed: ") + error.what()); }
            catch (...) { runtime_->report("Health subsystem publication preparation failed"); }
        }
    }
    static void run(const std::shared_ptr<Publisher>& state, const std::weak_ptr<ChannelCore>& weak,
                    const std::shared_ptr<Runtime>& runtime) noexcept {
        while (state->running) {
            {
                std::lock_guard lock(state->mutex);
                state->prompted = false;
            }
            try {
                // Pins are scoped to preparation/dispatch, NEVER the wait. Their
                // destruction may itself tear down Control on this worker.
                const auto control_pin = runtime->control.load().lock();
                const auto channel = std::dynamic_pointer_cast<HealthChannelImpl>(weak.lock());
                if (!control_pin || !channel || channel->is_detached()) break;
                channel->publish(*state);
            } catch (...) {
                try { runtime->report("Health publication preparation failed"); } catch (...) {}
            }
            std::unique_lock lock(state->mutex);
            state->changed.wait_for(lock, 100ms, [&] { return !state->running || state->prompted; });
        }
    }
    Callback<mel::MFA_Status> status_callback_;
    Callback<mel::MFA_StatusDetailed> detailed_callback_;
    Callback<mel::BIT_Status> bit_callback_;
    Callback<ir::SubsystemStatusResp> subsystem_callback_;
    std::shared_ptr<Publisher> publisher_; // writes under callback_mutex AND operations
    std::thread worker_; // operations; defensive destructor on its own worker
};

std::shared_ptr<ChannelCore> make_health_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config) {
    return std::make_shared<HealthChannelImpl>(runtime, config);
}

} // namespace provider_detail
} // namespace gige_mel
