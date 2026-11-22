// Configuration-only proof that selected camera policy reaches common MEL C2.
#include <gige_mel/provider.hpp>
#include <irmel/library/c2/C2Channel.h>

#include "profiles/navigation_handler.hpp"
#include "selection/profile_loader.hpp"
#include "support/recording_features.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace b = gige_mel::backend;
namespace p = b::profiles;
namespace ir = ams::iface::irmel;
using namespace std::chrono_literals;

namespace {
#define CHECK(expression) do { if (!(expression)) \
    throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #expression); } while (false)

constexpr auto device_id = "profile-policy-0";
constexpr auto rate_node = "AcquisitionFrameRateAbs";
constexpr auto exposure_node = "ExposureTimeAbs";

template<class T> T take(b::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

// Native reads and writes are serialized by the provider; test observations
// happen only between synchronous operations. No acquisition or optional sink.
class FixtureDevice final : public b::ICameraDevice {
public:
    b::Status admit(const p::ICameraProfile& profile) {
        auto result = profile.open(features);
        if (!result) return result.error();
        state_ = std::move(result).value();
        CHECK(!p::navigation_handler(*state_));
        return {};
    }
    b::CameraCapabilities capabilities() const override { return state_->capabilities(); }
    b::Result<b::CameraConfiguration> configuration() const override {
        return state_->configuration(features, [this] { return connected(); });
    }
    b::Result<b::CameraConfiguration> apply(const b::CameraConfiguration& request) override {
        return state_->apply(features, request, [this] { return connected(); },
                             [this](std::string_view) { return connected(); });
    }
    b::Result<std::unique_ptr<b::IFrameStream>> create_stream() override {
        return b::Error{b::ErrorCode::unsupported, "configuration-only fixture"};
    }
    b::Status close() override { closed = true; return {}; }

    mutable recording_features_fixture::RecordingFeatures features = recording_features_fixture::cognex_features();
    bool closed{false};
private:
    b::Status connected() const {
        if (closed) return b::Error{b::ErrorCode::disconnected, "fixture closed"};
        return {};
    }
    std::shared_ptr<const p::ICameraProfileState> state_;
};

class FixtureBackend final : public b::ICameraBackend {
public:
    explicit FixtureBackend(std::shared_ptr<const p::ICameraProfile> profile) : profile_(std::move(profile)) {}
    std::string_view id() const noexcept override { return "profile-policy-fixture"; }
    b::Result<std::vector<b::DeviceInfo>> enumerate() override {
        return std::vector<b::DeviceInfo>{{std::string(id()), device_id, "Typed feature fixture"}};
    }
    b::Result<std::shared_ptr<b::ICameraDevice>> open(std::string_view requested) override {
        if (requested != device_id) return b::Error{b::ErrorCode::not_found, "unknown fixture device"};
        if (opened.lock()) return b::Error{b::ErrorCode::busy, "fixture already open"};
        auto device = std::make_shared<FixtureDevice>();
        if (auto ok = device->admit(*profile_); !ok) return ok.error();
        opened = device;
        return std::shared_ptr<b::ICameraDevice>(std::move(device));
    }
    std::weak_ptr<FixtureDevice> opened;
private:
    std::shared_ptr<const p::ICameraProfile> profile_;
};

ir::Config c2_config(const ir::Control& control) {
    constexpr auto type = ir::ChannelType::CommandAndControl;
    for (const auto& capability : control.getCapabilities()) {
        const auto& types = capability.getChannelTypes();
        if (std::find(types.begin(), types.end(), type) == types.end()) continue;
        ir::Config config;
        config.setChanID(capability.getChanID());
        config.setChannelType(type);
        return config;
    }
    throw std::runtime_error("C2 not discoverable");
}

void send_camera_command(ir::C2Channel& c2, const std::vector<ir::CommandStatus>& statuses,
                         std::uint32_t id, const b::CameraConfiguration& request, bool accepted) {
    ir::CameraCommand command;
    command.setCommandID(id);
    command.setWidth(request.width);
    command.setHeight(request.height);
    command.setColumnOffset(request.offset_x);
    command.setRowOffset(request.offset_y);
    command.setIntegrationTime(request.exposure);
    command.setFrameRate(request.frame_rate_hz);
    command.setImageType(ir::ImageType::Staring);
    command.setNumberOfStages(0);
    const auto count = statuses.size();
    auto reply = c2.send(command);
    CHECK(reply.wait_for(1s) == std::future_status::ready);
    auto result = reply.get();
    CHECK(result && result.get()); // Both outcomes are ordinary responses, not error-only futures.
    CHECK(result.get()->getCommandID() == id);
    CHECK(result.get()->getCommandSuccess() == accepted);
    CHECK(statuses.size() == count + 1);
    const auto& status = statuses.back();
    CHECK(status.getCommandID() == id);
    CHECK(status.getState() == (accepted ? ir::CommandState::Accepted : ir::CommandState::Rejected));
    CHECK(status.getReasonID() == (accepted ? ir::CannotComply::NotSet : ir::CannotComply::CapabilityUnavailable));
}

void check_native_apply(const FixtureDevice& device, const b::CameraConfiguration& request) {
    const auto& features = device.features;
    CHECK((features.writes() == std::vector<std::string>{rate_node, exposure_node}));
    const auto rate_write = std::find_if(features.calls.begin(), features.calls.end(), [](const auto& call) {
        return call.is_write() && call.feature == rate_node;
    });
    CHECK(rate_write != features.calls.end() && rate_write->real == request.frame_rate_hz);
    CHECK(features.nodes.at(rate_node).real.value == request.frame_rate_hz);
    CHECK(take(device.configuration()) == request);
}

void selected_profile_camera_commands(std::shared_ptr<const p::ICameraProfile> profile, bool admits_six) {
    auto backend = std::make_shared<FixtureBackend>(std::move(profile));
    gige_mel::ProviderSettings settings;
    settings.device_id = device_id;
    std::vector<ir::CommandStatus> statuses; // outlives the callback, including on failure
    auto manager = gige_mel::make_api_manager(settings, backend);
    auto control = ir::getControl(settings.instance, manager);
    CHECK(control && control->init() == ir::Return::Success);
    auto c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(c2_config(*control)));
    CHECK(c2);
    CHECK(c2->registerMetadataCallback(std::function<void(ir::Channel&, const ir::CommandStatus*)>{
        [&](auto&, const auto* status) { statuses.push_back(*status); }}) == ir::Return::Success);
    auto device = backend->opened.lock();
    CHECK(device);
    b::CameraConfiguration request{2592, 1944, 0, 0, 6, 35ms};
    const auto caps = device->capabilities();
    CHECK(caps.minimum_frame_rate_hz <= 6 && caps.maximum_frame_rate_hz >= 6);
    CHECK(device->features.writes().empty());
    device->features.calls.clear();
    send_camera_command(*c2, statuses, 17, request, admits_six);
    if (admits_six) {
        check_native_apply(*device, request);
    } else {
        CHECK(caps.minimum_frame_rate_hz == 1 && caps.maximum_frame_rate_hz == 10);
        CHECK(device->features.writes().empty());
        CHECK(take(device->configuration()).frame_rate_hz == 10);

        // Simulate an out-of-band native change after init; not a fixture setter.
        device->features.nodes.at(rate_node).real.value = 16;
        CHECK(take(device->configuration()).frame_rate_hz == 16);
        device->features.calls.clear();
        request.frame_rate_hz = 5;
        send_camera_command(*c2, statuses, 18, request, true);
        check_native_apply(*device, request);
    }

    CHECK(control->detachChannel(c2) == ir::Return::Success);
    c2.reset();
    control.reset();
    manager.reset();
    CHECK(device->closed);
}
} // namespace

int main() {
    try {
        selected_profile_camera_commands(p::make_mono_free_running_profile(), true);
        auto cognex = take(gige_mel::selection::load_profile_module(GIGE_MEL_COGNEX_MODULE));
        selected_profile_camera_commands(take(cognex.create()), false);
        std::cout << "C2: default accepted 6 Hz; Cognex rejected 6 Hz and recovered from 16 to 5 Hz\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}