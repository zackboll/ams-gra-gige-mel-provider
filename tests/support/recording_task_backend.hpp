#pragma once
#include <gige_mel/task_execution.hpp>
#include <mutex>
#include <condition_variable>
#include <stdexcept>

namespace recording_task {
namespace b = gige_mel::backend;
using namespace std::chrono_literals;

inline b::TaskExecutionSupport support() {
    return {true, b::TaskCoordinates::virtual_aircraft_origin, b::TaskClock::session_relative_virtual,
        0, 0, 1ns, 10s, 1, 0, 16, true, true,
        "recording-virtual-stare", "SDK-free deterministic task fixture; not physical pointing",
        "session origin zero; system_time zero means immediate at source virtual now", 1};
}
struct Recording {
    std::mutex mutex;
    std::condition_variable changed;
    b::TaskExecutionSupport declarations{support()};
    b::TaskObservation observation;
    unsigned opens{0}, admissions{0}, cancellations{0}, replacements{0}, retirements{0};
    bool owned{false}, fail_open{false}, fail_admit{false}, fail_cancel{false}, fail_observe{false};
    b::TaskObservation snapshot() { std::lock_guard lock(mutex); return observation; }
    void wait_retired() {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 3s, [&] { return !owned; })) throw std::runtime_error("retirement watchdog");
    }
    void executing() {
        std::lock_guard lock(mutex);
        if (!owned || observation.phase != b::TaskPhase::admitted) throw std::runtime_error("not admitted");
        observation.phase = b::TaskPhase::executing;
    }
    void advance(std::chrono::nanoseconds by) {
        std::lock_guard lock(mutex);
        if (!owned || observation.phase != b::TaskPhase::executing || by.count() < 0)
            throw std::runtime_error("not executing");
        observation.virtual_time += by;
        // Fixture executes one duration from the most recent admission.
        remaining -= by;
        if (remaining.count() <= 0) observation.phase = b::TaskPhase::completed;
    }
    void fail() { std::lock_guard lock(mutex); observation.phase = b::TaskPhase::failed; }
    std::chrono::nanoseconds remaining{0};
};
class Session final : public b::ITaskExecutionSession {
    std::shared_ptr<Recording> r_;
public:
    explicit Session(std::shared_ptr<Recording> r) : r_(std::move(r)) {}
    ~Session() override { retire(); }
    b::Status admit(b::VirtualStareRequest request, std::chrono::steady_clock::time_point deadline) override {
        std::lock_guard lock(r_->mutex);
        if (std::chrono::steady_clock::now() >= deadline) return b::Error{b::ErrorCode::timeout, "admission deadline"};
        if (!r_->owned || r_->fail_admit) return b::Error{b::ErrorCode::io_error, "recording admission failure"};
        const auto& s = r_->declarations;
        if (request.duration < s.minimum_duration || request.duration > s.maximum_duration ||
            request.system_time != 0ns || request.azimuth != 0 || request.elevation != 0)
            return b::Error{b::ErrorCode::invalid_argument, "recording parameters"};
        if (r_->observation.phase == b::TaskPhase::admitted || r_->observation.phase == b::TaskPhase::executing) {
            ++r_->replacements; ++r_->cancellations;
        }
        r_->observation.task = request; r_->observation.phase = b::TaskPhase::admitted;
        r_->remaining = request.duration; ++r_->admissions;
        return {};
    }
    b::Result<b::TaskObservation> observe(std::chrono::steady_clock::time_point deadline) override {
        std::lock_guard lock(r_->mutex);
        if (std::chrono::steady_clock::now() >= deadline) return b::Error{b::ErrorCode::timeout, "observation deadline"};
        if (!r_->owned || r_->fail_observe) return b::Error{b::ErrorCode::io_error, "recording observation failure"};
        return r_->observation;
    }
    b::Status cancel_current(std::chrono::steady_clock::time_point deadline) override {
        std::lock_guard lock(r_->mutex);
        if (std::chrono::steady_clock::now() >= deadline) return b::Error{b::ErrorCode::timeout, "cancellation deadline"};
        if (r_->fail_cancel) return b::Error{b::ErrorCode::io_error, "recording cancellation failure"};
        if (!r_->owned || (r_->observation.phase != b::TaskPhase::admitted &&
            r_->observation.phase != b::TaskPhase::executing))
            return b::Error{b::ErrorCode::invalid_state, "no current task"};
        r_->observation.phase = b::TaskPhase::cancelled; ++r_->cancellations;
        return {};
    }
    void retire() noexcept override {
        std::lock_guard lock(r_->mutex);
        if (!r_->owned) return;
        if (r_->observation.phase == b::TaskPhase::admitted || r_->observation.phase == b::TaskPhase::executing) {
            if (r_->fail_cancel) r_->observation.phase = b::TaskPhase::failed;
            else { r_->observation.phase = b::TaskPhase::cancelled; ++r_->cancellations; }
        }
        r_->owned = false; ++r_->retirements;
        r_->changed.notify_all();
    }
};
class Device final : public b::ICameraDevice, public b::ITaskExecutionSource, public b::IStackedImageSource {
    std::shared_ptr<b::ICameraDevice> camera_;
    std::shared_ptr<Recording> r_;
public:
    Device(std::shared_ptr<b::ICameraDevice> c, std::shared_ptr<Recording> r)
        : camera_(std::move(c)), r_(std::move(r)) {}
    b::CameraCapabilities capabilities() const override { return camera_->capabilities(); }
    b::Result<b::CameraConfiguration> configuration() const override { return camera_->configuration(); }
    b::Result<b::CameraConfiguration> apply(const b::CameraConfiguration& c) override { return camera_->apply(c); }
    b::Result<std::unique_ptr<b::IFrameStream>> create_stream() override { return camera_->create_stream(); }
    b::Status close() override { return camera_->close(); }
    b::StackSupport stacked_image_support() const override {
        auto* s = dynamic_cast<b::IStackedImageSource*>(camera_.get());
        return s ? s->stacked_image_support() : b::StackSupport{};
    }
    b::Result<std::unique_ptr<b::IStackedImageStream>> open_stacked_images(std::uint64_t generation) override {
        auto* s = dynamic_cast<b::IStackedImageSource*>(camera_.get());
        if (!s) return b::Error{b::ErrorCode::unsupported, "no stack source"};
        return s->open_stacked_images(generation);
    }
    b::TaskExecutionSupport task_execution_support() const override { return r_->declarations; }
    b::Result<std::unique_ptr<b::ITaskExecutionSession>> open_task_session(
        std::chrono::steady_clock::time_point deadline) override {
        std::lock_guard lock(r_->mutex);
        if (std::chrono::steady_clock::now() >= deadline || r_->fail_open || r_->owned)
            return b::Error{b::ErrorCode::io_error, "recording session open failure"};
        auto session = std::make_unique<Session>(r_);
        r_->observation = {}; r_->owned = true; ++r_->opens;
        return std::unique_ptr<b::ITaskExecutionSession>{std::move(session)};
    }
};
class Backend final : public b::ICameraBackend {
    std::shared_ptr<b::ICameraBackend> camera_{b::make_synthetic_backend()};
    std::shared_ptr<Recording> r_;
public:
    explicit Backend(std::shared_ptr<Recording> r,
                     std::shared_ptr<b::ICameraBackend> camera = b::make_synthetic_backend())
        : camera_(std::move(camera)), r_(std::move(r)) {}
    std::string_view id() const noexcept override { return "recording-task"; }
    b::Result<std::vector<b::DeviceInfo>> enumerate() override { return camera_->enumerate(); }
    b::Result<std::shared_ptr<b::ICameraDevice>> open(std::string_view id) override {
        auto c = camera_->open(id); if (!c) return c.error();
        return std::shared_ptr<b::ICameraDevice>{std::make_shared<Device>(std::move(c).value(), r_)};
    }
};
} // namespace recording_task