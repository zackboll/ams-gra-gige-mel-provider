// Focused, TEST-ONLY embedding probe (GIGE-MEL-008): the unchanged common
// provider driven through make_api_manager(settings, internal Aravis backend)
// and the upstream getControl/getBuffer functions, with the real Aravis 0.8.36
// library and its in-process Fake interface. It exercises the MEL CONFIGURATION
// paths only. It is not a supported Aravis MEL profile: acquisition is not
// implemented, so Image enable must fail. Optics below are VIRTUAL test values.
#include <gige_mel/provider.hpp>
#include "aravis_fake_support.hpp"

#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
namespace backend = gige_mel::backend;
using namespace std::chrono_literals;

namespace {

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression)) {                                                                         \
            throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) +           \
                                     ": " #expression);                                              \
        }                                                                                            \
    } while (false)

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

template <class T> std::shared_ptr<T> response(mel::RequestFor<T> future) {
    CHECK(future.wait_for(0ms) == std::future_status::ready);
    auto result = future.get();
    CHECK(result);
    CHECK(result.get());
    return result.get();
}

template <class T> mel::Error error_of(mel::RequestFor<T> future) {
    CHECK(future.wait_for(0ms) == std::future_status::ready);
    auto result = future.get();
    CHECK(!result);
    return result.getError();
}

// Metadata collector: copies payloads, notifies with the mutex released;
// waits select by payload predicate after a captured baseline.
template <class T> struct Reports {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<T> items;
    std::function<void(ir::Channel&, const T*)> callback() {
        return [this](ir::Channel&, const T* value) {
            { std::lock_guard lock(mutex); items.push_back(*value); }
            changed.notify_all();
        };
    }
    std::size_t count() { std::lock_guard lock(mutex); return items.size(); }
    T last() { std::lock_guard lock(mutex); CHECK(!items.empty()); return items.back(); }
    template <class Predicate> T wait_match(std::size_t after, Predicate predicate) {
        std::unique_lock lock(mutex);
        std::optional<T> found;
        CHECK(changed.wait_for(lock, 5s, [&] {
            for (auto i = after; i < items.size(); ++i)
                if (predicate(items[i])) { found = items[i]; return true; }
            return false;
        }));
        return *found;
    }
};

std::map<std::string, std::string> detail_values(const mel::MFA_StatusDetailed& report) {
    std::map<std::string, std::string> values;
    for (const auto& pair : report.getStatus()) values.emplace(pair.getName(), pair.getValue());
    return values;
}

// Counts image callbacks; none may ever arrive.
class CountingListener final : public ir::ImageListener {
public:
    std::atomic<int> images{0};
    void onImage(const ir::Channel&, const ir::FrameHeader&, std::shared_ptr<ir::Buffer> buffer) override {
        images.fetch_add(1);
        if (buffer) (void)buffer->release();
    }
};

ir::CameraCommand camera_command(std::uint32_t id, const backend::CameraConfiguration& c) {
    ir::CameraCommand command;
    command.setCommandID(id);
    command.setWidth(c.width);
    command.setHeight(c.height);
    command.setColumnOffset(c.offset_x);
    command.setRowOffset(c.offset_y);
    command.setFrameRate(c.frame_rate_hz);
    command.setIntegrationTime(c.exposure);
    command.setImageType(ir::ImageType::Staring);
    command.setNumberOfStages(0);
    return command;
}

ir::ConfigSetCommand config_set(std::uint32_t id, const backend::CameraConfiguration& c) {
    ir::ConfigSetCommand command;
    command.setCommandID(id);
    command.setConfig(R"({"schema":"gige-mel.camera-config","version":1,"width":)" + std::to_string(c.width) +
                      R"(,"height":)" + std::to_string(c.height) + R"(,"offset_x":)" + std::to_string(c.offset_x) +
                      R"(,"offset_y":)" + std::to_string(c.offset_y) + R"(,"frame_rate_hz":)" +
                      std::to_string(c.frame_rate_hz) + R"(,"exposure_ns":)" + std::to_string(c.exposure.count()) + "}");
    return command;
}

void check_geometry(const ir::ChannelCapability& cap, const backend::CameraConfiguration& c) {
    CHECK(cap.getWidth() == c.width && cap.getHeight() == c.height && cap.getRowPitch() == c.width);
    CHECK(cap.getBufferSize() == c.width * c.height && cap.getFormat() == ir::PixelFormat::Mono);
}

void embedding_probe() {
    // Collectors and caller storage are declared first: they outlive Health
    // disable/drain and every channel.
    Reports<ir::CommandStatus> statuses;
    Reports<ir::CameraCommandResp> replies;
    Reports<mel::MFA_StatusDetailed> details;
    std::vector<std::vector<std::byte>> storage;
    auto listener = std::make_shared<CountingListener>();

    gige_mel::ProviderSettings settings;
    settings.instance = "aravis-fake-probe";
    settings.device_id = std::string(aravis_fake::device_id);
    settings.maximum_registered_buffers = 4;
    settings.horizontal_radians_per_pixel = 0.0002; // VIRTUAL test optics only
    settings.vertical_radians_per_pixel = 0.0003;   // VIRTUAL test optics only
    settings.timestamp_domain = backend::ClockDomain::device; // unused: no frames

    auto manager = gige_mel::make_api_manager(settings, aravis_fake::make_backend());
    CHECK(manager);
    CHECK(!ir::getControl("synthetic", manager));
    auto control = ir::getControl(settings.instance, manager);
    CHECK(control && control->getCapabilities().empty());
    CHECK(control->init() == ir::Return::Success); // opens exactly Fake_1 via the real SDK
    const backend::CameraConfiguration initial{512, 512, 0, 0, 25, 10ms};
    const auto caps = control->getCapabilities();
    CHECK(caps.size() == 3); // existing common advertisement, unchanged
    std::shared_ptr<ir::ImageChannel> image;
    std::shared_ptr<ir::C2Channel> c2;
    std::shared_ptr<ir::HealthStatusChannel> health;
    for (const auto& cap : caps) {
        ir::Config config;
        config.setChanID(cap.getChanID());
        config.setChannelType(cap.getChannelTypes().front());
        if (cap.getChannelTypes().front() != ir::ChannelType::HealthAndStatus) {
            check_geometry(cap, initial);
            config.setImgLstnr(listener);
        }
        auto channel = control->attachChannel(config);
        if (auto i = std::dynamic_pointer_cast<ir::ImageChannel>(channel)) image = i;
        if (auto c = std::dynamic_pointer_cast<ir::C2Channel>(channel)) c2 = c;
        if (auto h = std::dynamic_pointer_cast<ir::HealthStatusChannel>(channel)) health = h;
    }
    CHECK(image && c2 && health);
    CHECK(c2->registerMetadataCallback(statuses.callback()) == ir::Return::Success);
    CHECK(image->registerMetadataCallback(replies.callback()) == ir::Return::Success);
    CHECK(health->registerMetadataCallback(details.callback()) == ir::Return::Success);

    struct Cleanup {
        std::function<void()> run;
        ~Cleanup() { try { run(); } catch (...) {} }
    } cleanup{[&] {
        (void)image->disable();
        (void)health->disable(); // drains admitted Health callbacks
        (void)control->detachChannel(image);
        (void)control->detachChannel(c2);
        (void)control->detachChannel(health);
    }};

    // Detailed Health: backend/device identity, valid configuration, idle.
    CHECK(health->enable() == ir::Return::Success);
    auto values = detail_values(details.wait_match(0, [](const auto& r) {
        return detail_values(r).count("gige_mel.configuration_valid") != 0;
    }));
    CHECK(values.at("gige_mel.backend_id") == "aravis" && values.at("gige_mel.device_id") == "Fake_1");
    CHECK(values.at("gige_mel.configuration_valid") == "true" && values.at("gige_mel.acquisition_state") == "idle");

    // C2 CameraCommand: Image reply and correlated C2 status; actual geometry.
    const backend::CameraConfiguration a{640, 480, 16, 8, 40, 1'250'000ns};
    auto reply = response(c2->send(camera_command(8001, a)));
    CHECK(reply->getCommandID() == 8001 && reply->getCommandSuccess());
    CHECK(replies.count() == 1 && replies.last().getCommandID() == 8001);
    CHECK(statuses.count() == 1 && statuses.last().getCommandID() == 8001);
    CHECK(statuses.last().getState() == ir::CommandState::Accepted);
    check_geometry(image->getCapabilities(), a);
    check_geometry(c2->getCapabilities(), a);

    // Image CameraCommand: Image reply only.
    const backend::CameraConfiguration b{1024, 768, 0, 2, 50, 2ms};
    reply = response(image->send(camera_command(8002, b)));
    CHECK(reply->getCommandID() == 8002 && reply->getCommandSuccess());
    CHECK(replies.count() == 2 && statuses.count() == 1);
    check_geometry(image->getCapabilities(), b);

    // ConfigSet version 1 through C2.
    const backend::CameraConfiguration c{800, 600, 8, 4, 25, 1ms};
    CHECK(*response(c2->send(config_set(8003, c))) == ir::Return::Success);
    CHECK(statuses.count() == 2 && statuses.last().getCommandID() == 8003);
    CHECK(statuses.last().getState() == ir::CommandState::Accepted);
    check_geometry(image->getCapabilities(), c);

    // Exposure quantized by the SDK (1250.5 us -> 1250 us): the provider sees
    // the truthful readback, reports a mismatch, and keeps it valid.
    const auto quantized = response(c2->send(camera_command(8004, {800, 600, 8, 4, 25, 1'250'500ns})));
    CHECK(!quantized->getCommandSuccess());
    CHECK(statuses.last().getCommandID() == 8004 && statuses.last().getState() == ir::CommandState::Rejected);
    CHECK(contains(statuses.last().getReasonDescription(), "readback differs"));

    // Unrepresentable native readback (30 Hz -> 30.0003 Hz): explicit error,
    // configuration becomes not-known-valid in Health.
    const auto unrepresentable = error_of(c2->send(camera_command(8005, {800, 600, 8, 4, 30, 1ms})));
    CHECK(unrepresentable.getCode() == mel::ErrorCode::Unsupported);
    CHECK(contains(unrepresentable.getDescription(), "30.00030000300003"));
    CHECK(statuses.last().getCommandID() == 8005 && statuses.last().getState() == ir::CommandState::Rejected);
    CHECK(statuses.last().getReasonID() == ir::CannotComply::CapabilityUnavailable);
    std::size_t baseline = details.count();
    values = detail_values(details.wait_match(baseline, [](const auto& r) {
        return detail_values(r).at("gige_mel.configuration_valid") == "false";
    }));
    CHECK(contains(values.at("gige_mel.last_error_history"), "30.00030000300003"));
    CHECK(image->enable() == ir::Return::Fail); // no buffers and invalid configuration

    // Explicit recovery with an exactly representable configuration.
    const backend::CameraConfiguration recovery{640, 480, 0, 0, 50, 2ms};
    CHECK(*response(c2->send(config_set(8006, recovery))) == ir::Return::Success);
    CHECK(statuses.last().getCommandID() == 8006 && statuses.last().getState() == ir::CommandState::Accepted);
    check_geometry(image->getCapabilities(), recovery);
    baseline = details.count();
    details.wait_match(baseline, [](const auto& r) { return detail_values(r).at("gige_mel.configuration_valid") == "true"; });

    // Image enable with valid configuration and registered buffers: the
    // adapter's explicit unsupported stream yields the existing Fail result.
    const auto bytes = static_cast<std::size_t>(image->getCapabilities().getBufferSize());
    std::vector<std::shared_ptr<ir::Buffer>> buffers;
    for (int i = 0; i < 2; ++i) {
        storage.emplace_back(bytes);
        auto buffer = ir::getBuffer(settings.instance, manager);
        CHECK(buffer && buffer->init(storage.back().data(), bytes, i) == ir::Return::Success);
        CHECK(image->registerBuffer(buffer) == ir::Return::Success);
        buffers.push_back(buffer);
    }
    CHECK(image->enable() == ir::Return::Fail);
    const auto stats = gige_mel::provider_statistics(manager);
    CHECK(contains(stats.last_error, "acquisition is not implemented"));
    CHECK(stats.delivered_frames == 0 && stats.stream_failures == 0);
    baseline = details.count();
    values = detail_values(details.wait_match(baseline, [](const auto&) { return true; }));
    CHECK(values.at("gige_mel.acquisition_state") == "idle" && values.at("gige_mel.configuration_valid") == "true");
    // Configuration remains possible: no acquisition is owned.
    CHECK(response(image->send(camera_command(8007, initial)))->getCommandSuccess());
    CHECK(health->disable() == ir::Return::Success);
    for (const auto& buffer : buffers) CHECK(image->unregisterBuffer(buffer) == ir::Return::Success);
    CHECK(listener->images == 0); // no image callback ever ran
}

} // namespace

int main() {
    std::cout << "Aravis runtime " << gige_mel::backend::aravis::runtime_version() << " loaded from "
              << aravis_fake::loaded_library_path() << "; Fake interface; virtual test optics; not a supported MEL "
              << "image profile\n";
    try {
        embedding_probe();
    } catch (const std::exception& error) {
        std::cerr << "FAIL Aravis MEL embedding probe: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS Aravis MEL configuration-path embedding probe\n";
    return 0;
}
