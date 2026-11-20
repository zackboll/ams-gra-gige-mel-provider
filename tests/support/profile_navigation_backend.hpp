#pragma once

#include "profiles/navigation_bridge.hpp"
#include "static_profile_features.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// TEST ONLY, SDK-free composition (GIGE-MEL-047). One generic backend for any
// selected camera profile. Imaging is delegated to the synthetic backend; the
// selected profile is admitted on a static feature fixture. The device exposes
// INavigationSink only when the selected profile state provides the optional
// handler. Its port RECORDS prepared operations; it never transmits anything.
namespace profile_navigation_fixture {
namespace p = b::profiles;

/// Behavior of the recording port, set by the test before each submission.
enum class PortMode { accept, refuse, uncertain, lost_after_accept, lost_before_send, throw_after_entry };

/// Bounded test rendezvous: one entry, then wait for release (5 s watchdog).
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool armed{false}, entered{false}, released{false}, expired{false};
    void arm() { std::lock_guard lock(mutex); armed = true; entered = released = expired = false; }
    void pass() {
        std::unique_lock lock(mutex);
        if (!armed) return;
        entered = true;
        changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return released; })) expired = true;
        armed = false;
    }
    bool wait_entered() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }) && !expired;
    }
    void release() {
        { std::lock_guard lock(mutex); released = true; }
        changed.notify_all();
    }
    bool clean() { std::lock_guard lock(mutex); return !expired; }
};

/// Everything a test inspects. Event strings record observable ordering.
struct Recording {
    mutable std::mutex mutex;
    std::vector<p::NavigationOperation> operations; // exactly what the port received
    std::vector<std::string> events;
    PortMode mode{PortMode::accept};
    b::ErrorCode refuse_code{b::ErrorCode::busy};
    Gate port_gate;
    unsigned ports_created{0}, ports_destroyed{0}, devices_opened{0}, sink_devices{0};
    void event(std::string text) { std::lock_guard lock(mutex); events.push_back(std::move(text)); }
    std::vector<std::string> snapshot_events() const { std::lock_guard lock(mutex); return events; }
    std::size_t operation_count() const { std::lock_guard lock(mutex); return operations.size(); }
    void set_mode(PortMode value) { std::lock_guard lock(mutex); mode = value; }
};

class RecordingPort final : public p::INavigationOperationPort {
public:
    explicit RecordingPort(std::shared_ptr<Recording> r) : r_(std::move(r)) {
        std::lock_guard lock(r_->mutex);
        ++r_->ports_created;
    }
    ~RecordingPort() override {
        std::lock_guard lock(r_->mutex);
        r_->events.push_back("port retired");
        ++r_->ports_destroyed;
    }
    p::NavigationExecution submit(p::NavigationOperation operation) override {
        r_->port_gate.pass();
        PortMode mode;
        {
            std::lock_guard lock(r_->mutex);
            mode = r_->mode;
            if (mode == PortMode::refuse || mode == PortMode::lost_before_send) {
                r_->events.push_back("port refused");
                return {p::NavigationCompletion::not_submitted, r_->refuse_code, "recording port refused before entry",
                        mode == PortMode::lost_before_send};
            }
            r_->operations.push_back(std::move(operation));
            r_->events.push_back("port recorded");
        }
        switch (mode) {
        case PortMode::uncertain:
            return {p::NavigationCompletion::uncertain, b::ErrorCode::io_error, "recording port: completion unknown", false};
        case PortMode::lost_after_accept:
            return {p::NavigationCompletion::accepted, b::ErrorCode::io_error, "recording port: control lost", true};
        case PortMode::throw_after_entry: throw std::runtime_error("recording port failure after recording");
        default: return {p::NavigationCompletion::accepted, b::ErrorCode::io_error, "", false};
        }
    }
private:
    std::shared_ptr<Recording> r_;
};

/// Camera behavior shared by both device kinds; owns the profile state.
class DeviceCore : public b::ICameraDevice {
public:
    DeviceCore(std::shared_ptr<b::ICameraDevice> inner, std::shared_ptr<const p::ICameraProfileState> state)
        : inner_(std::move(inner)), state_(std::move(state)) {}
    b::CameraCapabilities capabilities() const override { return inner_->capabilities(); }
    b::Result<b::CameraConfiguration> configuration() const override { return inner_->configuration(); }
    b::Result<b::CameraConfiguration> apply(const b::CameraConfiguration& value) override { return inner_->apply(value); }
    b::Result<std::unique_ptr<b::IFrameStream>> create_stream() override { return inner_->create_stream(); }
    b::Status close() override { return inner_->close(); }
protected:
    std::shared_ptr<b::ICameraDevice> inner_;
    std::shared_ptr<const p::ICameraProfileState> state_;
};

/// Profile A: no handler, therefore no sink (receipt-only in the provider).
class PlainDevice final : public DeviceCore {
public:
    using DeviceCore::DeviceCore;
};

/// Profile B: INavigationSink delegating to the reusable bridge.
class NavigationDevice final : public DeviceCore, public b::INavigationSink {
public:
    NavigationDevice(std::shared_ptr<b::ICameraDevice> inner, std::shared_ptr<const p::ICameraProfileState> state,
                     std::unique_ptr<p::ProfileNavigationBridge> bridge, std::shared_ptr<Recording> r)
        : DeviceCore(std::move(inner), std::move(state)), bridge_(std::move(bridge)), r_(std::move(r)) {}
    ~NavigationDevice() override { (void)bridge_->shutdown(); }
    b::Status submit_navigation(b::NavigationReport report) override {
        r_->event("sink entered");
        return bridge_->submit(report);
    }
    b::Status close() override {
        // Drain and retire navigation resources before the device's own close.
        (void)bridge_->shutdown();
        r_->event("device closed");
        return inner_->close();
    }
private:
    std::unique_ptr<p::ProfileNavigationBridge> bridge_;
    std::shared_ptr<Recording> r_;
};

/// Generic backend: identical for every profile; the profile decides the sink.
class ProfileNavigationBackend final : public b::ICameraBackend {
public:
    ProfileNavigationBackend(std::shared_ptr<const p::ICameraProfile> profile, std::shared_ptr<Recording> r)
        : profile_(std::move(profile)), r_(std::move(r)) {}
    std::string_view id() const noexcept override { return "recording-profile-navigation"; }
    b::Result<std::vector<b::DeviceInfo>> enumerate() override { return synthetic_->enumerate(); }
    b::Result<std::shared_ptr<b::ICameraDevice>> open(std::string_view device_id) override {
        StaticFeatures features;
        auto state = profile_->open(features); // features are borrowed for this call only
        if (!state) return state.error();
        auto inner = synthetic_->open(device_id);
        if (!inner) return inner.error();
        { std::lock_guard lock(r_->mutex); ++r_->devices_opened; }
        auto shared = std::move(state).value();
        if (!p::navigation_handler(*shared))
            return std::shared_ptr<b::ICameraDevice>(std::make_shared<PlainDevice>(std::move(inner).value(), shared));
        auto bridge = p::ProfileNavigationBridge::create(shared, std::make_unique<RecordingPort>(r_));
        if (!bridge) return bridge.error();
        { std::lock_guard lock(r_->mutex); ++r_->sink_devices; }
        return std::shared_ptr<b::ICameraDevice>(std::make_shared<NavigationDevice>(
            std::move(inner).value(), std::move(shared), std::move(bridge).value(), r_));
    }
private:
    std::shared_ptr<const p::ICameraProfile> profile_;
    std::shared_ptr<Recording> r_;
    std::shared_ptr<b::ICameraBackend> synthetic_{b::make_synthetic_backend()};
};
} // namespace profile_navigation_fixture
