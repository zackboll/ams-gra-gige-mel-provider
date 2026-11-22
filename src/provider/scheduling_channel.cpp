#include "channels.hpp"
#include "channel_core.hpp"
#include "unsupported_callbacks.hpp"
#include "tasking.hpp"
#include "test_hooks.hpp"
#include <irmel/library/scheduling/SchedulingChannel.h>
#include <algorithm>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

class SchedulingChannelImpl final : public ChannelCore, public ir::SchedulingChannel {
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::send;
    using ChannelCore::registerMetadataCallback;
    mel::RequestFor<ir::TrackListResponse> send(ir::PrioritizedTrackListCmd) override {
        return failed<ir::TrackListResponse>(mel::ErrorCode::Unsupported, "track lists are unsupported");
    }
    mel::RequestFor<ir::Return> exec(ir::IRSTScanTask) override {
        return failed<ir::Return>(mel::ErrorCode::Unsupported, "scanning is unsupported");
    }
    mel::RequestFor<ir::Return> exec(ir::IRSTTrackTask) override {
        return failed<ir::Return>(mel::ErrorCode::Unsupported, "tracking is unsupported");
    }
    GIGE_UNSUPPORTED_CALLBACK(ir::TaskExecutingRep)
    GIGE_UNSUPPORTED_CALLBACK(ir::SchedCreatedRep)
    GIGE_UNSUPPORTED_CALLBACK(ir::UpdateTrackListResponse)
    GIGE_UNSUPPORTED_CALLBACK(ir::TaskEvents)
    GIGE_UNSUPPORTED_CALLBACK(ir::ScanPerformanceReport)
    ir::Return registerMetadataCallback(Callback<ir::ExecuteTaskAck> callback) override {
        return register_callback(ack_callback_, std::move(callback));
    }
    mel::RequestFor<ir::Return> exec(ir::IRSTStareTask command) override {
        if (in_provider_callback())
            return failed<ir::Return>(mel::ErrorCode::InvalidState, "submit tasks outside provider callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::Return>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        const auto info = command.getTaskInfo();
        const auto p = command.getParams();
        const auto center = p.getCenter();
        const ir::ExecuteTaskAck ack(command.getCommandID(), info.getReqId());
        auto result = ready(ir::Return::Success); // allocate before source mutation
        Callback<ir::ExecuteTaskAck> callback;
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached() || !runtime_->task_session)
                return failed<ir::Return>(mel::ErrorCode::InvalidState, "TaskSched session required");
            const auto& s = runtime_->task_support;
            if (info.getAction() != ir::TaskListAction::Immediate)
                return failed<ir::Return>(mel::ErrorCode::Unsupported, "only Immediate; no queue or embedded abort");
            if (info.getSystemTime() != std::chrono::nanoseconds{0} || info.getPriority() != 0 ||
                info.getPrecedenceWithinPriority() != 0 || center.az != s.azimuth || center.el != s.elevation ||
                p.getCentFrameRefAz() != ir::CoordFrameRef::Aircraft ||
                p.getCentFrameRefEl() != ir::CoordFrameRef::Aircraft ||
                p.getDurationNanoseconds() < s.minimum_duration || p.getDurationNanoseconds() > s.maximum_duration)
                return failed<ir::Return>(mel::ErrorCode::InvalidParameters, "outside qualified virtual stare parameters");
            const auto end = runtime_->task_ids.begin() + static_cast<std::ptrdiff_t>(runtime_->task_admissions);
            if (std::find(runtime_->task_ids.begin(), end, info.getReqId()) != end)
                return failed<ir::Return>(mel::ErrorCode::InvalidId, "duplicate request ID in this session");
            if (runtime_->task_admissions >= s.session_admissions)
                return failed<ir::Return>(mel::ErrorCode::InsufficientLocalResources, "bounded session exhausted; enter Standby");
            // Copy potentially allocating consumer function before source admission.
            { std::lock_guard lock(callback_mutex_); callback = ack_callback_; }
            const auto admitted = task_call([&] { return runtime_->task_session->admit(
                {command.getCommandID(), info.getReqId(), info.getSystemTime(), p.getDurationNanoseconds(),
                 center.az, center.el}, task_deadline()); });
            if (!admitted) {
                runtime_->report(admitted.error().message);
                return failed<ir::Return>(map_error(admitted.error().code), admitted.error().message);
            }
            runtime_->task_ids[runtime_->task_admissions++] = info.getReqId();
            admit_callback(callback); // detach drains reservation before returning
        }
        GIGE_MEL_TASK_SEAM();
        invoke(callback, ack, nullptr, true);
        return result;
    }
    mel::RequestFor<ir::Return> exec(ir::AbortTask) override {
        // Pinned AbortTask has no accessor: never inspect or claim targeted IDs.
        if (in_provider_callback())
            return failed<ir::Return>(mel::ErrorCode::InvalidState, "abort outside provider callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::Return>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        auto result = ready(ir::Return::Success);
        std::lock_guard operation(runtime_->operations);
        if (is_detached() || !runtime_->task_session)
            return failed<ir::Return>(mel::ErrorCode::InvalidState, "TaskSched session required");
        auto cancelled = task_call([&] { return runtime_->task_session->cancel_current(task_deadline()); });
        if (!cancelled) {
            runtime_->report(cancelled.error().message);
            return failed<ir::Return>(map_error(cancelled.error().code), cancelled.error().message);
        }
        return result;
    }
protected:
    void stop_auxiliary_locked() override { retire_tasking(*runtime_); }
private:
    Callback<ir::ExecuteTaskAck> ack_callback_;
};

std::shared_ptr<ChannelCore> make_scheduling_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config) {
    return std::make_shared<SchedulingChannelImpl>(runtime, config);
}

} // namespace provider_detail
} // namespace gige_mel
