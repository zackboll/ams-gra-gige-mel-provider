#pragma once

// SDK/MEL-free test-generated recording source. Decorates only the camera
// transport; LOS values are independently injected, never computed from pixels.
#include <gige_mel/backend.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace recording_los {
namespace b = gige_mel::backend;
using namespace std::chrono_literals;

inline b::LosSupport support(bool euler = true, bool report = true) {
    return {euler, report, b::LosFrame::sensor_frd_intrinsic_zyx,
        b::LosTimeDomain::source_relative, b::LosEpoch::declared_source_origin,
        "recording-los-043", "TEST-GENERATED; not a physical measurement",
        "nanoseconds since virtual recording origin 043"};
}

inline b::LosObservation observation(std::int64_t time = 123456789012345LL) {
    b::LosObservation v;
    v.time = std::chrono::nanoseconds{time};
    v.time_domain = b::LosTimeDomain::source_relative;
    v.epoch = b::LosEpoch::declared_source_origin;
    v.frame = b::LosFrame::sensor_frd_intrinsic_zyx;
    v.clock_generation = 7;
    v.source_id = "recording-los-043";
    v.euler = b::LosEuler{{0.123456789012345, -0.234567890123456, 0.345678901234567},
                         {-0.456789012345678, 0.567890123456789, -0.678901234567890}};
    b::LosPointing p;
    p.angles = b::LosAzEl{0.789012345678901, -0.890123456789012};
    p.rates = b::LosAzEl{-0.901234567890123, 1.012345678901234};
    p.image_rotation = -1.123456789012345;
    v.report = p;
    return v;
}

struct Subscription {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<b::LosObservation> pending;
    bool stopped{false}, fail{false};
    bool gate_delivery{false}, delivering{false};
    std::uint64_t id, generation;
    unsigned receives{0}, destroyed{0}, coalesced{0};

    void wait_receives(unsigned n) {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 3s, [&] { return receives >= n; }))
            throw std::runtime_error("LOS receive watchdog");
    }
    // Holding a consumer gate permits controlled coalescing of rapid updates.
    unsigned push(b::LosObservation v, bool stamp_subscription = true) {
        std::lock_guard lock(mutex);
        if (stamp_subscription) v.subscription_id = id;
        if (pending) ++coalesced;
        pending = std::move(v);
        changed.notify_all();
        return receives;
    }
    void deliver(b::LosObservation v, bool stamp_subscription = true) {
        // The next receive entry proves preceding callback/validation finished.
        // Timeouts may increment receives, so wait for pending consumption AND
        // entry into another receive, not merely an increment observed at push.
        std::unique_lock lock(mutex);
        if (stamp_subscription) v.subscription_id = id;
        if (pending) ++coalesced;
        pending = std::move(v);
        changed.notify_all();
        if (!changed.wait_for(lock, 3s, [&] { return !pending; }))
            throw std::runtime_error("LOS consumption watchdog");
        const auto after = receives;
        if (!changed.wait_for(lock, 3s, [&] { return receives > after || stopped; }))
            throw std::runtime_error("LOS dispatch watchdog");
    }
    void fault() { std::lock_guard lock(mutex); fail = true; changed.notify_all(); }
    void wait_delivering() {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 3s, [&] { return delivering; }))
            throw std::runtime_error("LOS delivery admission watchdog");
    }
    void wait_destroyed() {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 3s, [&] { return destroyed != 0; }))
            throw std::runtime_error("LOS destruction watchdog");
    }
};

class Stream final : public b::ILineOfSightStream {
    std::shared_ptr<Subscription> state_;
public:
    explicit Stream(std::shared_ptr<Subscription> state) : state_(std::move(state)) {}
    ~Stream() override {
        request_stop();
        std::lock_guard lock(state_->mutex);
        ++state_->destroyed;
        state_->changed.notify_all();
    }
    std::uint64_t clock_generation() const noexcept override { return state_->generation; }
    b::Result<b::LosObservation> receive(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(state_->mutex);
        ++state_->receives;
        state_->changed.notify_all();
        if (!state_->changed.wait_for(lock, timeout, [&] {
                return state_->stopped || state_->fail || state_->pending.has_value(); }))
            return b::Error{b::ErrorCode::timeout, "recording wait"};
        if (state_->stopped) return b::Error{b::ErrorCode::stopped, "recording cancelled"};
        if (state_->fail) return b::Error{b::ErrorCode::io_error, "deliberate recording fault"};
        auto v = std::move(*state_->pending);
        state_->pending.reset();
        state_->delivering = true;
        state_->changed.notify_all();
        // A committed owned value may race cancellation. Stop must wake this
        // gate and provider admission must discard the late return.
        if (!state_->changed.wait_for(lock, timeout, [&] { return !state_->gate_delivery || state_->stopped; }))
            return b::Error{b::ErrorCode::timeout, "recording delivery gate"};
        return v;
    }
    void request_stop() noexcept override {
        std::lock_guard lock(state_->mutex);
        state_->stopped = true;
        state_->pending.reset();
        state_->changed.notify_all();
    }
};

struct Recording {
    b::LosSupport declarations{support()}; // configure before Control init
    std::uint64_t generation{7}; // change only while disabled
    bool fail_open{false};
    std::shared_ptr<Subscription> latest;
    unsigned opens{0};
    std::atomic<unsigned> devices_destroyed{0};
};

class Device final : public b::ICameraDevice, public b::ILineOfSightSource {
    std::shared_ptr<b::ICameraDevice> camera_;
    std::shared_ptr<Recording> recording_;
public:
    Device(std::shared_ptr<b::ICameraDevice> camera, std::shared_ptr<Recording> recording)
        : camera_(std::move(camera)), recording_(std::move(recording)) {}
    ~Device() override { ++recording_->devices_destroyed; }
    b::CameraCapabilities capabilities() const override { return camera_->capabilities(); }
    b::Result<b::CameraConfiguration> configuration() const override { return camera_->configuration(); }
    b::Result<b::CameraConfiguration> apply(const b::CameraConfiguration& c) override { return camera_->apply(c); }
    b::Result<std::unique_ptr<b::IFrameStream>> create_stream() override { return camera_->create_stream(); }
    b::Status close() override { return camera_->close(); }
    b::LosSupport line_of_sight_support() const override { return recording_->declarations; }
    b::Result<std::unique_ptr<b::ILineOfSightStream>> open_line_of_sight(std::uint64_t id) override {
        if (recording_->fail_open) return b::Error{b::ErrorCode::io_error, "recording open fault"};
        auto s = std::make_shared<Subscription>();
        s->id = id;
        s->generation = recording_->generation;
        recording_->latest = s;
        ++recording_->opens;
        return std::unique_ptr<b::ILineOfSightStream>{std::make_unique<Stream>(s)};
    }
};

class Backend final : public b::ICameraBackend {
    std::shared_ptr<b::ICameraBackend> camera_{b::make_synthetic_backend()};
    std::shared_ptr<Recording> recording_;
public:
    explicit Backend(std::shared_ptr<Recording> recording) : recording_(std::move(recording)) {}
    std::string_view id() const noexcept override { return "test-generated-los-recording"; }
    b::Result<std::vector<b::DeviceInfo>> enumerate() override { return camera_->enumerate(); }
    b::Result<std::shared_ptr<b::ICameraDevice>> open(std::string_view id) override {
        auto opened = camera_->open(id);
        if (!opened) return opened.error();
        return std::shared_ptr<b::ICameraDevice>{std::make_shared<Device>(std::move(opened).value(), recording_)};
    }
};
} // namespace recording_los