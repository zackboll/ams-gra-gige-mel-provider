#pragma once

#include "contract_fixture_camera.hpp"
#include "recording_features.hpp"
#include "profiles/image_field_of_view.hpp"

#include <mutex>
#include <stdexcept>

// SDK-free composition: the selected state interprets one recording feature
// store. Streams get a snapshot from that same store via profile preflight.
// No profile IDs, optical constants or model computation belong here.
namespace profile_fov_fixture {
namespace b = gige_mel::backend;
namespace p = b::profiles;
namespace rf = recording_features_fixture;
using namespace std::chrono_literals;
inline constexpr std::string_view device_id = "profile-fov-fixture";
inline constexpr b::CameraConfiguration initial{80, 60, 4, 6, 40, 1250us};

inline rf::RecordingFeatures features() {
    auto f = rf::baseline();
    f.nodes["SensorWidth"] = rf::integer_node(384, 1, 384, 1, rf::Access::read_only);
    f.nodes["SensorHeight"] = rf::integer_node(256, 1, 256, 1, rf::Access::read_only);
    f.nodes["Width"] = rf::integer_node(initial.width, 4, 384, 4);
    f.nodes["Height"] = rf::integer_node(initial.height, 2, 256, 2);
    f.nodes["OffsetX"] = rf::integer_node(initial.offset_x, 0, 384, 2);
    f.nodes["OffsetY"] = rf::integer_node(initial.offset_y, 0, 256, 2);
    f.nodes["AcquisitionFrameRate"] = rf::float_node(initial.frame_rate_hz, 10, 90, "Hz");
    f.nodes["ExposureTimeAbs"] = rf::float_node(1250, 100, 100000, "us");
    f.nodes["AcquisitionMode"] = rf::text_node("Continuous");
    f.coupled_roi = true;
    return f;
}

struct Observation {
    std::shared_ptr<contract_fixture::Observation> native = std::make_shared<contract_fixture::Observation>();
    // Test-only source fault injection. Set before enable, not during a run.
    std::function<b::Result<b::ImageFieldOfView>()> failure;
    // Simulate actual native readback differing from a write, for a C2 rejection.
    std::optional<std::uint32_t> override_written_x;
    unsigned stream_creation_attempts{}; // provider-facing calls, even if preflight fails

    void record(const b::CameraConfiguration& actual) {
        std::lock_guard lock(mutex);
        requests.push_back(actual);
    }
    std::vector<b::CameraConfiguration> fov_requests() const {
        std::lock_guard lock(mutex);
        return requests;
    }
private:
    mutable std::mutex mutex;
    std::vector<b::CameraConfiguration> requests;
};

class Device : public b::ICameraDevice {
public:
    Device(rf::RecordingFeatures f, std::shared_ptr<const p::ICameraProfileState> state,
           std::shared_ptr<Observation> observed)
        : observed_(std::move(observed)), features_(std::move(f)), state_(std::move(state)) {
        features_.after_write = [observed = observed_](auto& service, const auto& call) {
            if (call.feature == "OffsetX" && observed->override_written_x)
                service.nodes.at("OffsetX").integer.value = *observed->override_written_x;
        };
    }
    b::CameraCapabilities capabilities() const override { return state_->capabilities(); }
    b::Result<b::CameraConfiguration> configuration() const override { return state_->configuration(features_, connected); }
    b::Result<b::CameraConfiguration> apply(const b::CameraConfiguration& c) override {
        return state_->apply(features_, c, connected, [](auto) { return b::Status{}; });
    }
    b::Result<std::unique_ptr<b::IFrameStream>> create_stream() override {
        ++observed_->stream_creation_attempts;
        auto actual = state_->preflight(features_);
        if (!actual) return actual.error();
        return contract_fixture::make_stream(actual.value(), observed_->native);
    }
    b::Status close() override { return {}; }
protected:
    const std::shared_ptr<Observation> observed_;
private:
    static b::Status connected() { return {}; }
    mutable rf::RecordingFeatures features_;
    const std::shared_ptr<const p::ICameraProfileState> state_;
};

class FovDevice final : public Device, public b::IImageFieldOfViewSource {
public:
    FovDevice(rf::RecordingFeatures f, std::shared_ptr<const p::ICameraProfileState> state,
              std::shared_ptr<Observation> observed, std::shared_ptr<const p::IProfileImageFieldOfView> handler)
        : Device(std::move(f), std::move(state), std::move(observed)), handler_(std::move(handler)) {}
    b::Result<b::ImageFieldOfView> image_field_of_view(const b::CameraConfiguration& c) const override {
        observed_->record(c);
        if (observed_->failure) return observed_->failure();
        return handler_->image_field_of_view(c);
    }
private:
    const std::shared_ptr<const p::IProfileImageFieldOfView> handler_;
};

class Backend final : public b::ICameraBackend {
public:
    Backend(std::shared_ptr<const p::ICameraProfile> profile, std::shared_ptr<Observation> observed)
        : profile_(std::move(profile)), observed_(std::move(observed)) {}
    std::string_view id() const noexcept override { return "profile-fov-test"; }
    b::Result<std::vector<b::DeviceInfo>> enumerate() override {
        return std::vector<b::DeviceInfo>{{std::string(id()), std::string(device_id), "Profile FOV fixture"}};
    }
    b::Result<std::shared_ptr<b::ICameraDevice>> open(std::string_view id) override {
        if (id != device_id) return b::Error{b::ErrorCode::not_found, "unknown FOV fixture device"};
        auto f = features();
        auto opened = profile_->open(f);
        if (!opened) return opened.error();
        auto state = std::move(opened).value();
        if (auto handler = p::retain_image_field_of_view(state))
            return std::shared_ptr<b::ICameraDevice>(std::make_shared<FovDevice>(
                std::move(f), std::move(state), observed_, std::move(handler)));
        return std::shared_ptr<b::ICameraDevice>(std::make_shared<Device>(std::move(f), std::move(state), observed_));
    }
private:
    const std::shared_ptr<const p::ICameraProfile> profile_;
    const std::shared_ptr<Observation> observed_;
};
} // namespace profile_fov_fixture