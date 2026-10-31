// Focused, TEST-ONLY embedding probe (GIGE-MEL-008): the unchanged common
// provider driven through make_api_manager(settings, internal Aravis backend)
// and the upstream getControl/getBuffer functions, with the real Aravis 0.8.36
// library and its in-process Fake interface. It exercises the MEL configuration
// paths and, since GIGE-MEL-009, NEGATIVE image admission: real SDK Fake
// acquisition runs through the provider, but every frame carries the default
// invalid FrameTimestamp{}, so the unchanged provider rejects it and no Image
// is published. This is not a supported Aravis MEL image profile and not
// successful MEL image publication. Optics below are VIRTUAL test values.
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

// TEST-ONLY observing decorator around the real Aravis backend. It forwards
// every call unchanged and records, for each frame the adapter returns to the
// common provider, whether that frame meets every NON-time admission
// condition the provider checks, plus its timestamp. It never alters a frame
// or a result, so the provider sees exactly what the adapter returned.
struct FrameLog {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t frames{0}, admissible_except_time{0}, default_timestamp{0};
    std::uint64_t configuration_generation{0};
    backend::CameraConfiguration last_configuration{};
    void record(const backend::FrameLease& f, std::uint64_t started_generation) {
        const auto& d = f.descriptor();
        const auto& c = d.configuration;
        const bool layout = d.width == c.width && d.height == c.height && d.configuration_generation == started_generation &&
                            d.pixel_format == backend::PixelFormat::mono8 && d.storage_bits_per_pixel == 8 &&
                            d.significant_bits_per_pixel == 8 && d.bands == 1 && d.row_stride_bytes >= d.width &&
                            d.height > 0 && d.status == backend::FrameStatus::complete &&
                            f.bytes().size() >= d.row_stride_bytes * d.height;
        const auto& t = d.timestamp;
        const bool unqualified = !t.valid && t.domain == backend::ClockDomain::unspecified &&
                                 t.event == backend::TimestampEvent::unspecified && t.value.count() == 0 &&
                                 t.clock_generation == 0;
        {
            std::lock_guard lock(mutex);
            ++frames;
            if (layout) ++admissible_except_time;
            if (unqualified) ++default_timestamp;
            configuration_generation = d.configuration_generation;
            last_configuration = c;
        }
        changed.notify_all();
    }
};

class ObservedStream final : public backend::IFrameStream {
public:
    ObservedStream(std::unique_ptr<backend::IFrameStream> inner, std::shared_ptr<FrameLog> log)
        : inner_(std::move(inner)), log_(std::move(log)) {}
    backend::Status start(std::uint64_t generation) override {
        generation_ = generation;
        return inner_->start(generation);
    }
    backend::Result<backend::FrameLease> receive(std::chrono::milliseconds timeout) override {
        auto result = inner_->receive(timeout);
        if (result) log_->record(result.value(), generation_);
        return result;
    }
    void request_stop() noexcept override { inner_->request_stop(); }
    void stop() noexcept override { inner_->stop(); }
    backend::StreamStatistics statistics() const override { return inner_->statistics(); }

private:
    std::unique_ptr<backend::IFrameStream> inner_;
    std::shared_ptr<FrameLog> log_;
    std::uint64_t generation_{0};
};

class ObservedDevice final : public backend::ICameraDevice {
public:
    ObservedDevice(std::shared_ptr<backend::ICameraDevice> inner, std::shared_ptr<FrameLog> log)
        : inner_(std::move(inner)), log_(std::move(log)) {}
    backend::CameraCapabilities capabilities() const override { return inner_->capabilities(); }
    backend::Result<backend::CameraConfiguration> configuration() const override { return inner_->configuration(); }
    backend::Result<backend::CameraConfiguration> apply(const backend::CameraConfiguration& c) override {
        return inner_->apply(c);
    }
    backend::Result<std::unique_ptr<backend::IFrameStream>> create_stream() override {
        auto created = inner_->create_stream();
        if (!created) return created.error();
        return std::unique_ptr<backend::IFrameStream>(std::make_unique<ObservedStream>(std::move(created).value(), log_));
    }
    backend::Status close() override { return inner_->close(); }

private:
    std::shared_ptr<backend::ICameraDevice> inner_;
    std::shared_ptr<FrameLog> log_;
};

class ObservedBackend final : public backend::ICameraBackend {
public:
    ObservedBackend(std::shared_ptr<backend::ICameraBackend> inner, std::shared_ptr<FrameLog> log)
        : inner_(std::move(inner)), log_(std::move(log)) {}
    std::string_view id() const noexcept override { return inner_->id(); }
    backend::Result<std::vector<backend::DeviceInfo>> enumerate() override { return inner_->enumerate(); }
    backend::Result<std::shared_ptr<backend::ICameraDevice>> open(std::string_view id) override {
        auto opened = inner_->open(id);
        if (!opened) return opened.error();
        return std::shared_ptr<backend::ICameraDevice>(std::make_shared<ObservedDevice>(std::move(opened).value(), log_));
    }

private:
    std::shared_ptr<backend::ICameraBackend> inner_;
    std::shared_ptr<FrameLog> log_;
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
    auto frames = std::make_shared<FrameLog>(); // outlives the provider worker

    gige_mel::ProviderSettings settings;
    settings.instance = "aravis-fake-probe";
    settings.device_id = std::string(aravis_fake::device_id);
    settings.maximum_registered_buffers = 4;
    settings.horizontal_radians_per_pixel = 0.0002; // VIRTUAL test optics only
    settings.vertical_radians_per_pixel = 0.0003;   // VIRTUAL test optics only
    // The provider requires a declared domain; the adapter's frames carry
    // FrameTimestamp{} (no domain), so none can satisfy it.
    settings.timestamp_domain = backend::ClockDomain::device;

    auto manager = gige_mel::make_api_manager(
        settings, std::make_shared<ObservedBackend>(aravis_fake::make_backend(), frames));
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

    // NEGATIVE MEL ADMISSION (GIGE-MEL-009). Real SDK Fake acquisition is
    // started through the common provider. The adapter returns genuine Mono8
    // frames that meet every non-time admission condition but carry
    // FrameTimestamp{} (invalid, unspecified domain/event); the unchanged
    // provider must reject every one: no Image callback, destination bytes
    // untouched, no stream failure.
    constexpr std::byte sentinel{0xA5};
    const auto bytes = static_cast<std::size_t>(image->getCapabilities().getBufferSize());
    std::vector<std::shared_ptr<ir::Buffer>> buffers;
    for (int i = 0; i < 2; ++i) {
        storage.emplace_back(bytes, sentinel);
        auto buffer = ir::getBuffer(settings.instance, manager);
        CHECK(buffer && buffer->init(storage.back().data(), bytes, i) == ir::Return::Success);
        CHECK(image->registerBuffer(buffer) == ir::Return::Success);
        buffers.push_back(buffer);
    }
    const auto before = gige_mel::provider_statistics(manager);
    CHECK(image->enable() == ir::Return::Success); // AcquisitionStart through the real SDK
    {
        // Observe actual returned backend frames AND the provider rejecting them.
        std::unique_lock lock(frames->mutex);
        CHECK(frames->changed.wait_for(lock, 5s, [&] { return frames->frames >= 3; }));
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (gige_mel::provider_statistics(manager).rejected_frames < before.rejected_frames + 3) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(5ms);
    }
    std::size_t observed = 0;
    {
        std::lock_guard lock(frames->mutex);
        observed = frames->frames;
        // Every returned frame met the non-time admission conditions and
        // carried exactly the default, invalid timestamp.
        CHECK(frames->admissible_except_time == frames->frames);
        CHECK(frames->default_timestamp == frames->frames);
        CHECK(frames->last_configuration == recovery);
    }
    baseline = details.count();
    values = detail_values(details.wait_match(baseline, [](const auto&) { return true; }));
    CHECK(values.at("gige_mel.acquisition_state") == "running"); // owned, not faulted
    // Configuration is rejected while acquisition is owned (existing meaning).
    const auto blocked = error_of(image->send(camera_command(8007, initial)));
    CHECK(contains(blocked.getDescription(), "disable image acquisition before configuring"));

    // Explicit disable joins and drains the provider worker (AcquisitionStop).
    CHECK(image->disable() == ir::Return::Success);
    const auto after = gige_mel::provider_statistics(manager);
    CHECK(after.rejected_frames >= before.rejected_frames + 3);
    CHECK(after.delivered_frames == before.delivered_frames && after.dropped_no_buffer == before.dropped_no_buffer);
    CHECK(after.stream_failures == before.stream_failures && after.callback_failures == before.callback_failures);
    CHECK(contains(after.last_error, "timestamp contract"));
    CHECK(listener->images == 0); // zero Image callbacks
    for (const auto& block : storage)
        if (block.size() == bytes)
            CHECK(std::all_of(block.begin(), block.end(), [&](std::byte value) { return value == sentinel; }));
    {
        // Rejections came from returned frames; each rejected frame was one.
        std::lock_guard lock(frames->mutex);
        CHECK(after.rejected_frames - before.rejected_frames <= frames->frames && frames->frames >= observed);
    }
    baseline = details.count();
    values = detail_values(details.wait_match(baseline, [](const auto&) { return true; }));
    CHECK(values.at("gige_mel.acquisition_state") == "idle" && values.at("gige_mel.configuration_valid") == "true");

    // Recovery: configuration works again, buffers clean up, and a second
    // explicit enable/disable cycle starts a fresh acquisition.
    CHECK(response(image->send(camera_command(8008, initial)))->getCommandSuccess());
    check_geometry(image->getCapabilities(), initial);
    for (const auto& buffer : buffers) CHECK(image->unregisterBuffer(buffer) == ir::Return::Success);
    buffers.clear();
    const auto initial_bytes = static_cast<std::size_t>(initial.width) * initial.height;
    for (int i = 0; i < 2; ++i) {
        storage.emplace_back(initial_bytes, sentinel);
        auto buffer = ir::getBuffer(settings.instance, manager);
        CHECK(buffer && buffer->init(storage.back().data(), initial_bytes, 10 + i) == ir::Return::Success);
        CHECK(image->registerBuffer(buffer) == ir::Return::Success);
        buffers.push_back(buffer);
    }
    const std::size_t frames_before_restart = [&] { std::lock_guard lock(frames->mutex); return frames->frames; }();
    CHECK(image->enable() == ir::Return::Success);
    {
        std::unique_lock lock(frames->mutex);
        CHECK(frames->changed.wait_for(lock, 5s, [&] { return frames->frames >= frames_before_restart + 2; }));
        CHECK(frames->last_configuration == initial && frames->default_timestamp == frames->frames);
    }
    CHECK(image->disable() == ir::Return::Success);
    CHECK(listener->images == 0 && gige_mel::provider_statistics(manager).stream_failures == before.stream_failures);
    CHECK(health->disable() == ir::Return::Success);
    for (const auto& buffer : buffers) CHECK(image->unregisterBuffer(buffer) == ir::Return::Success);
    for (const auto& block : storage)
        if (block.size() == bytes || block.size() == initial_bytes)
            CHECK(std::all_of(block.begin(), block.end(), [&](std::byte value) { return value == sentinel; }));
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
    std::cout << "PASS Aravis MEL configuration-path and negative timestamp-admission embedding probe\n";
    return 0;
}
