#include "channels.hpp"
#include "channel_core.hpp"
#include "unsupported_callbacks.hpp"
#include "navigation_translation.hpp"
#include <irmel/library/image/ImageChannel.h>
#include <cmath>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/** @brief Delivers copied monochrome frames and attachment-local Image metadata.
 * Camera responses, navigation receipts and communications are available without
 * image enable. Known-empty bad pixels and source-backed LOS are optional. No
 * distortion map, quaternion, candidate/preprocessed objects, NUC temperature
 * or LOS3D kinematics are supplied. LOS is not derived from navigation or
 * associated with a particular image, and implies no physical alignment.
 */
class ImageChannelImpl final : public ChannelCore, public ir::ImageChannel {
    /// Subscription-owned source and Runtime; per-family time history is worker-local.
    struct LosPublisher {
        std::shared_ptr<Runtime> runtime;
        std::unique_ptr<backend::ILineOfSightStream> stream;
        std::atomic<bool> running{true};
        std::uint64_t subscription, generation;
        std::optional<std::chrono::nanoseconds> last_euler, last_report;
    };
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::send;
    using ChannelCore::registerMetadataCallback;
    /// Retire LOS before base acquisition cleanup, including self-thread teardown.
    ~ImageChannelImpl() override { stop_auxiliary_locked(); }
    /// Submit a complete camera snapshot using the shared configuration transaction.
    /// @see ChannelCore::camera_command
    mel::RequestFor<ir::CameraCommandResp> send(ir::CameraCommand command) override { return camera_command(command); }
    /** @brief Accept one structurally valid navigation report for this attachment.
     * @param report Copied into bounded latest-report state on acceptance;
     * systemTime (nanoseconds) must strictly increase within this attachment.
     * @return Ready receipt with exact input time and zero correlation IDs, or
     * an error future. Rejections leave the local latest report unchanged.
     * @note Under operations and with Control/channel pinned, forward once to
     * an optional sink. Without a sink, success means local receipt only; with
     * a sink it also means backend acceptance as defined by that sink, not
     * physical application, time alignment or frame association. Timeout or
     * sink exception can leave external acceptance uncertain (no rollback).
     * A reserved receipt callback runs inline after unlocking and is drained
     * by detach. Disable does not reset the accepted timestamp. Error future
     * construction can itself throw on resource failure.
     */
    mel::RequestFor<ir::NavigationReportResp> send(mel::NavigationReport report) override {
        if (in_provider_callback())
            return failed<ir::NavigationReportResp>(mel::ErrorCode::InvalidState, "submit navigation outside provider callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::NavigationReportResp>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        Callback<ir::NavigationReportResp> callback;
        ir::NavigationReportResp reply;
        mel::RequestFor<ir::NavigationReportResp> result;
        try {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<ir::NavigationReportResp>(mel::ErrorCode::InvalidState, "channel is detached");
            if (const auto error = navigation_error(report))
                return failed<ir::NavigationReportResp>(mel::ErrorCode::InvalidParameters, *error);
            if (latest_navigation_ && report.getSystemTime() <= latest_navigation_->getSystemTime())
                return failed<ir::NavigationReportResp>(mel::ErrorCode::InvalidState, "systemTime must strictly increase within this Image attachment");
            // All potentially allocating preparation precedes the no-throw commit.
            { std::lock_guard lock(callback_mutex_); callback = navigation_callback_; }
            reply.setCommandID(0); // Unassigned: NavigationReport has no correlation IDs.
            reply.setReqId(0);
            reply.setSystemTime(report.getSystemTime()); // Exact echo, not an acknowledgement clock.
            result = ready(reply);
            if (auto* sink = dynamic_cast<backend::INavigationSink*>(runtime_->device.get())) {
                try {
                    NavigationSubmissionGuard submission;
                    const auto submitted = sink->submit_navigation(backend_navigation(report));
                    if (!submitted) {
                        if (submitted.error().code == backend::ErrorCode::timeout)
                            return failed<ir::NavigationReportResp>(mel::ErrorCode::InvalidState,
                                "backend navigation acceptance is uncertain after timeout: " + submitted.error().message);
                        return failed<ir::NavigationReportResp>(map_error(submitted.error().code), submitted.error().message);
                    }
                } catch (const std::bad_alloc&) {
                    throw;
                } catch (...) {
                    return failed<ir::NavigationReportResp>(mel::ErrorCode::InsufficientRemoteResources, "backend navigation submission threw; acceptance is uncertain");
                }
            }
            static_assert(std::is_nothrow_copy_constructible_v<mel::NavigationReport>);
            admit_callback(callback);
            latest_navigation_.emplace(report);
        } catch (const std::bad_alloc&) {
            return failed<ir::NavigationReportResp>(mel::ErrorCode::InsufficientLocalResources, "navigation preparation or submission allocation failed; backend acceptance is not confirmed");
        }
        invoke(callback, reply, nullptr, true);
        return result;
    }
    /// With explicit known-empty defect-map support, install the slot and
    /// reserve one empty snapshot for inline dispatch after unlocking. No
    /// calibration or recurring defect detection; otherwise NotSupported.
    ir::Return registerMetadataCallback(Callback<ir::BadPixelList> callback) override {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        const auto self = shared_from_this();
        const ir::BadPixelList snapshot; // Pinned defaults: zero bytes/count, empty vector.
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return ir::Return::Fail;
            if (!runtime_->camera_capabilities.known_empty_defect_map) return ir::Return::NotSupported;
            const auto result = register_callback(bad_pixel_callback_, std::move(callback));
            if (result != ir::Return::Success) return result;
            // The slot is immutable after registration. Admit before releasing
            // operations so concurrent detach drains even not-yet-entered dispatch.
            admit_callback(bad_pixel_callback_);
        }
        invoke(bad_pixel_callback_, snapshot, nullptr, true);
        return ir::Return::Success;
    }
    /// Install the source-qualified azimuth/elevation report slot; no initial
    /// callback or replay. Dispatch is on the enabled LOS worker only.
    ir::Return registerMetadataCallback(Callback<ir::LineOfSightReport> callback) override {
        return register_los(report_callback_, std::move(callback), runtime_->los_support.report);
    }
    /// Install the independently qualified Euler attitude/rates slot for enabled
    /// worker publication; NotSupported without this LOS declaration.
    ir::Return registerMetadataCallback(Callback<ir::LineOfSightEuler> callback) override {
        return register_los(euler_callback_, std::move(callback), runtime_->los_support.euler);
    }
    GIGE_UNSUPPORTED_CALLBACK(ir::OpticalDistortionMap)
    GIGE_UNSUPPORTED_CALLBACK(ir::LineOfSightQuaternion)
    /// Serialize installation of the single receipt slot with submission and
    /// detach; no replay of previously accepted navigation.
    ir::Return registerMetadataCallback(Callback<ir::NavigationReportResp> callback) override {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        const auto self = shared_from_this();
        std::lock_guard operation(runtime_->operations);
        return register_callback(navigation_callback_, std::move(callback));
    }
    GIGE_UNSUPPORTED_CALLBACK(ir::CandidateObjectMessage)
    GIGE_UNSUPPORTED_CALLBACK(ir::CandidateObjectPreProcMessage)
    GIGE_UNSUPPORTED_CALLBACK(ir::NUC_TempData)
    /// Reject LOS3D kinematics registration without retaining the callable.
    ir::Return registerMetadataCallback(const std::function<void(ir::Channel&, const ir::LOS3D_KinematicsType* const)>&) override {
        return ir::Return::NotSupported;
    }
    /// Install the slot for typed Image camera responses, including C2-originated
    /// responses. No initial dispatch or callback for error-only outcomes.
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::CameraCommandResp* const)> callback) override {
        return register_callback(camera_callback_, std::move(callback));
    }

private:
    /// Serialize capability-gated LOS slot installation with lifecycle via operations.
    template<class T> ir::Return register_los(Callback<T>& slot, Callback<T> callback, bool supported) {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        if (!supported) return ir::Return::NotSupported;
        return register_callback(slot, std::move(callback));
    }
    /// If either LOS family is advertised, open a fresh subscription with a
    /// nonzero clock generation. Failure prevents Image enable even without a subscriber.
    bool start_auxiliary_locked() override {
        if (!runtime_->los_support.euler && !runtime_->los_support.report) return true;
        if (runtime_->los_subscription == std::numeric_limits<std::uint64_t>::max()) return false;
        auto* source = dynamic_cast<backend::ILineOfSightSource*>(runtime_->device.get());
        if (!source) return false;
        const auto subscription = ++runtime_->los_subscription;
        auto opened = source->open_line_of_sight(subscription);
        if (!opened) { runtime_->report(opened.error().message); return false; }
        auto state = std::make_shared<LosPublisher>();
        state->runtime = runtime_;
        state->stream = std::move(opened).value();
        if (!state->stream || state->stream->clock_generation() == 0) return false;
        state->subscription = subscription;
        state->generation = state->stream->clock_generation();
        los_publisher_ = state;
        los_worker_ = std::thread([state, weak = weak_from_this()] { run_los(state, weak); });
        return true;
    }
    /// Close LOS worker admission, wake receive and join (or detach self).
    void stop_auxiliary_locked() override {
        if (los_publisher_) {
            los_publisher_->running = false;
            los_publisher_->stream->request_stop();
        }
        if (los_worker_.joinable()) {
            if (los_worker_.get_id() == std::this_thread::get_id()) los_worker_.detach();
            else los_worker_.join();
        }
        los_publisher_.reset();
    }
    /// Check every Euler component before copying source values to MEL.
    static bool finite(const backend::LosEulerAngles& v) noexcept {
        return std::isfinite(v.roll) && std::isfinite(v.pitch) && std::isfinite(v.yaw);
    }
    /// Reject nonfinite azimuth or elevation, for angles or rates.
    static bool finite(const backend::LosAzEl& v) noexcept {
        return std::isfinite(v.azimuth) && std::isfinite(v.elevation);
    }
    /// Accept matching source/frame/epoch/subscription observations at strictly
    /// increasing positive source-relative nanoseconds per family. Copy finite
    /// values without transforms; report needs angles, rates and rotation, with
    /// no platform attitude, validity bits or at-speed/in-tolerance claims.
    void publish_los(LosPublisher& state, const backend::LosObservation& v) {
        const auto& support = runtime_->los_support;
        if (!v.time || v.time->count() <= 0 || v.time_domain != support.time_domain ||
            v.epoch != support.epoch || v.frame != support.frame || v.source_id != support.source_id ||
            v.subscription_id != state.subscription || v.clock_generation != state.generation) return;
        if (support.euler && v.euler && finite(v.euler->attitude) && finite(v.euler->rates) &&
            (!state.last_euler || *v.time > *state.last_euler)) {
            ir::LineOfSightEuler message;
            message.setSystemTime(*v.time);
            mel::Euler attitude, rates;
            attitude.setRoll(v.euler->attitude.roll);
            attitude.setPitch(v.euler->attitude.pitch);
            attitude.setYaw(v.euler->attitude.yaw);
            rates.setRoll(v.euler->rates.roll);
            rates.setPitch(v.euler->rates.pitch);
            rates.setYaw(v.euler->rates.yaw);
            message.setAttitude(attitude);
            message.setAttitudeRates(rates);
            Callback<ir::LineOfSightEuler> callback;
            { std::lock_guard lock(callback_mutex_); callback = euler_callback_; }
            state.last_euler = v.time;
            invoke(callback, message, &state.running);
        }
        if (support.report && v.report && v.report->angles && v.report->rates && v.report->image_rotation &&
            finite(*v.report->angles) && finite(*v.report->rates) && std::isfinite(*v.report->image_rotation) &&
            v.report->validity_flags == 0 && !v.report->at_speed && !v.report->in_tolerance &&
            !v.report->platform_attitude && (!state.last_report || *v.time > *state.last_report)) {
            ir::LineOfSightReport message;
            message.setSystemTime(*v.time);
            message.setPointingAngle({v.report->angles->azimuth, v.report->angles->elevation});
            message.setPointingAngleRates({v.report->rates->azimuth, v.report->rates->elevation});
            message.setImageRotation(*v.report->image_rotation);
            Callback<ir::LineOfSightReport> callback;
            { std::lock_guard lock(callback_mutex_); callback = report_callback_; }
            state.last_report = v.time;
            invoke(callback, message, &state.running);
        }
    }
    /// Receive on a separate worker, pin Control/channel only for dispatch.
    /// Terminal source/preparation failure retires LOS publication, not Image.
    static void run_los(const std::shared_ptr<LosPublisher>& state,
                        const std::weak_ptr<ChannelCore>& weak) noexcept {
        try {
            while (state->running) {
                auto result = state->stream->receive(50ms);
                if (!state->running) break;
                if (!result) {
                    if (result.error().code == backend::ErrorCode::timeout) continue;
                    state->runtime->report("LOS source fault: " + result.error().message);
                    break;
                }
                // Never pin owners across the source wait. Their last release
                // can destroy Control here; stop handles its own worker safely.
                const auto control_pin = state->runtime->control.load().lock();
                const auto channel = std::dynamic_pointer_cast<ImageChannelImpl>(weak.lock());
                if (!control_pin || !channel || channel->is_detached()) break;
                channel->publish_los(*state, result.value());
            }
        } catch (...) {
            try { state->runtime->report("LOS publication/source threw; subscription retired"); } catch (...) {}
        }
        state->running = false;
    }
    Callback<ir::LineOfSightEuler> euler_callback_;
    Callback<ir::LineOfSightReport> report_callback_;
    std::shared_ptr<LosPublisher> los_publisher_; // operations only
    std::thread los_worker_;
    /// Snapshot the Image response slot, then attempt inline admission and delivery.
    void on_camera_reply(const ir::CameraCommandResp& reply) override {
        Callback<ir::CameraCommandResp> callback;
        { std::lock_guard lock(callback_mutex_); callback = camera_callback_; }
        invoke(callback, reply);
    }
    Callback<ir::CameraCommandResp> camera_callback_;
    Callback<ir::NavigationReportResp> navigation_callback_;
    Callback<ir::BadPixelList> bad_pixel_callback_;
    // One private, attachment-local report; never read by frame publication.
    std::optional<mel::NavigationReport> latest_navigation_; // Runtime::operations
};

std::shared_ptr<ChannelCore> make_image_channel(const std::shared_ptr<Runtime>& runtime, const ir::Config& config) {
    return std::make_shared<ImageChannelImpl>(runtime, config);
}

} // namespace provider_detail
} // namespace gige_mel
