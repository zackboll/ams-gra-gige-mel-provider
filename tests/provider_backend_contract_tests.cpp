// Backend portability qualification (GIGE-MEL-007). The SAME MEL-facing
// scenarios run against every BackendCase below through the existing embedding
// seam gige_mel::make_api_manager(settings, backend) and the upstream
// Control/Buffer factories with settings.instance. Shared scenarios read every
// backend-dependent fact from case data and never branch on a backend name.
// Fixture-specific direct source-lease checks are kept separately at the end.
#include <gige_mel/provider.hpp>
#include "join_observer.hpp"
#include "support/contract_fixture_camera.hpp"

#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
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

std::chrono::nanoseconds host_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

template <class T> std::shared_ptr<T> response(mel::RequestFor<T> future) {
    CHECK(future.wait_for(0ms) == std::future_status::ready);
    auto result = future.get();
    CHECK(result);
    CHECK(result.get());
    return result.get();
}

template <class T> void expect_error(mel::RequestFor<T> future, mel::ErrorCode code) {
    CHECK(future.wait_for(0ms) == std::future_status::ready);
    auto result = future.get();
    CHECK(!result);
    CHECK(result.getError().getCode() == code);
    CHECK(!result.getError().getDescription().empty());
}

template <class Predicate> void eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

struct TestCleanup {
    std::function<void()> cleanup;
    ~TestCleanup() { cleanup(); }
};

// Watchdog gate: owners open it on every unwind path; expiry is a test failure.
struct WatchdogGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false}, opened{false}, expired{false};

    void enter_and_wait() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        if (!changed.wait_for(lock, 5s, [&] { return opened; })) expired = true;
    }
    void wait_for_entry() {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 2s, [&] { return entered; }));
        CHECK(!expired);
    }
    void open() {
        { std::lock_guard lock(mutex); opened = true; }
        changed.notify_all();
    }
    void check() {
        std::lock_guard lock(mutex);
        CHECK(entered && !expired);
    }
};

// ---------------------------------------------------------------------------
// Narrow test instrumentation, applied identically to both cases. It forwards
// every call unchanged to the case backend: frames, capabilities, identities
// and errors all come from the inner implementation. It records only which
// device IDs were opened, discovery calls, apply() calls and the readback the
// provider observed. It is neither an implementation under test nor a
// production interface.
struct ProbeLog {
    std::mutex mutex;
    std::vector<std::string> opens;
    unsigned enumerations{0}, applies{0};
    std::optional<backend::CameraConfiguration> last_readback;

    std::vector<std::string> opened() { std::lock_guard lock(mutex); return opens; }
    unsigned apply_count() { std::lock_guard lock(mutex); return applies; }
    unsigned enumerate_count() { std::lock_guard lock(mutex); return enumerations; }
    backend::CameraConfiguration readback() {
        std::lock_guard lock(mutex);
        CHECK(last_readback);
        return *last_readback;
    }
};

class ProbeDevice final : public backend::ICameraDevice {
public:
    ProbeDevice(std::shared_ptr<backend::ICameraDevice> inner, std::shared_ptr<ProbeLog> log)
        : inner_(std::move(inner)), log_(std::move(log)) {}
    backend::CameraCapabilities capabilities() const override { return inner_->capabilities(); }
    backend::Result<backend::CameraConfiguration> configuration() const override {
        auto actual = inner_->configuration();
        std::lock_guard lock(log_->mutex);
        if (actual) log_->last_readback = actual.value();
        else log_->last_readback.reset();
        return actual;
    }
    backend::Result<backend::CameraConfiguration> apply(const backend::CameraConfiguration& config) override {
        { std::lock_guard lock(log_->mutex); ++log_->applies; }
        return inner_->apply(config);
    }
    backend::Result<std::unique_ptr<backend::IFrameStream>> create_stream() override { return inner_->create_stream(); }
    backend::Status close() override { return inner_->close(); }

private:
    std::shared_ptr<backend::ICameraDevice> inner_;
    std::shared_ptr<ProbeLog> log_;
};

class ProbeBackend final : public backend::ICameraBackend {
public:
    ProbeBackend(std::shared_ptr<backend::ICameraBackend> inner, std::shared_ptr<ProbeLog> log)
        : inner_(std::move(inner)), log_(std::move(log)) {}
    std::string_view id() const noexcept override { return inner_->id(); }
    backend::Result<std::vector<backend::DeviceInfo>> enumerate() override {
        { std::lock_guard lock(log_->mutex); ++log_->enumerations; }
        return inner_->enumerate();
    }
    backend::Result<std::shared_ptr<backend::ICameraDevice>> open(std::string_view device_id) override {
        { std::lock_guard lock(log_->mutex); log_->opens.emplace_back(device_id); }
        auto opened = inner_->open(device_id);
        if (!opened) return opened.error();
        std::shared_ptr<backend::ICameraDevice> device = std::make_shared<ProbeDevice>(std::move(opened).value(), log_);
        return device;
    }

private:
    std::shared_ptr<backend::ICameraBackend> inner_;
    std::shared_ptr<ProbeLog> log_;
};

// ---------------------------------------------------------------------------
// Case description. A future adapter supplies one of these (its factory,
// explicit settings, identities, configurations and documented pixel, frame-ID
// and time facts) to run the same shared scenarios.
enum class Outcome { accepted, rejected_before_apply, rejected_by_backend };

// Capability probes shared by every case. Each case states the outcome its OWN
// capabilities imply; the assertions applied to that outcome are identical.
struct Probe {
    const char* label;
    backend::CameraConfiguration config;
};
const std::vector<Probe> capability_probes{
    {"64x48@(0,0) 120 Hz 1 ms", {64, 48, 0, 0, 120, 1ms}},
    {"81x60@(4,6) 40 Hz 1.25 ms", {81, 60, 4, 6, 40, 1250us}},
    {"400x300@(0,0) 30 Hz 1 ms", {400, 300, 0, 0, 30, 1ms}},
    {"700x48@(0,0) 30 Hz 1 ms", {700, 48, 0, 0, 30, 1ms}},
    {"64x48@(8,10) 50 Hz 25 ms", {64, 48, 8, 10, 50, 25ms}}, // exposure exceeds the 20 ms period
};

using PixelModel = std::function<std::uint8_t(std::uint32_t x, std::uint32_t y,
                                              const backend::CameraConfiguration&, std::uint32_t frame_id)>;
// (frame time, model frame index, host time before enable, host time in the
// image callback, configuration in force) -> consistent with the case's model.
struct TimeModel {
    // (frame time, host steady time before enable, host steady time in the
    // image callback, configuration in force)
    std::function<bool(std::chrono::nanoseconds, std::chrono::nanoseconds, std::chrono::nanoseconds,
                       const backend::CameraConfiguration&)> absolute;
    // (time difference, native frame-index difference, configuration) between
    // two frames delivered by the same acquisition.
    std::function<bool(std::chrono::nanoseconds, std::uint64_t, const backend::CameraConfiguration&)> spacing;
};

struct BackendCase {
    std::string name;
    std::function<std::shared_ptr<backend::ICameraBackend>()> factory;
    gige_mel::ProviderSettings settings;
    std::string backend_id, device_id; // identities expected through MEL
    backend::CameraConfiguration initial, alternate;
    std::vector<Outcome> probe_outcomes; // parallel to capability_probes
    PixelModel pixel;                    // ROI-local x/y, configuration, MEL frame ID
    // Native IDs are first + k * step (k >= 0, gaps allowed); MEL publishes the
    // low 32 bits.
    std::uint64_t first_native_id{0}, native_id_step{1};
    TimeModel time;
    std::string profile;
};

std::chrono::nanoseconds period(const backend::CameraConfiguration& c) {
    return std::chrono::nanoseconds{1'000'000'000LL / c.frame_rate_hz};
}

BackendCase synthetic_case() {
    BackendCase c;
    c.name = "synthetic";
    c.initial = {64, 48, 0, 0, 30, 1ms};
    c.alternate = {48, 32, 6, 3, 60, 2ms};
    c.factory = [initial = c.initial] {
        backend::SyntheticOptions options;
        options.frame_pool_size = 4;
        options.initial_configuration = initial;
        return backend::make_synthetic_backend(options);
    };
    c.settings.instance = "synthetic";
    c.settings.device_id = "synthetic-0";
    c.settings.maximum_registered_buffers = 16;
    c.settings.horizontal_radians_per_pixel = 0.001; // synthetic virtual optics
    c.settings.vertical_radians_per_pixel = 0.001;
    c.settings.timestamp_domain = backend::ClockDomain::host_monotonic;
    c.backend_id = "synthetic";
    c.device_id = "synthetic-0";
    // 640x480, unit increments, 1-120 Hz, atomic validation before writing.
    c.probe_outcomes = {Outcome::accepted, Outcome::accepted, Outcome::accepted,
                        Outcome::rejected_before_apply, Outcome::rejected_by_backend};
    // Implementation profile: (x + y + native_frame_id) & 255, ROI-local x/y.
    c.pixel = [](std::uint32_t x, std::uint32_t y, const backend::CameraConfiguration&, std::uint32_t id) {
        return static_cast<std::uint8_t>((static_cast<std::uint64_t>(x) + y + id) & 0xffU);
    };
    c.first_native_id = 1;
    c.native_id_step = 1;
    // Host steady clock; integration start = production time - exposure, and
    // production is paced at least one period apart.
    c.time.absolute = [](auto t, auto before, auto callback, const backend::CameraConfiguration& config) {
        return t >= before - config.exposure && t <= callback;
    };
    c.time.spacing = [](auto dt, std::uint64_t dk, const backend::CameraConfiguration& config) {
        return dk > 0 && dt >= static_cast<std::int64_t>(dk) * period(config);
    };
    c.profile = "backend=synthetic device=synthetic-0 instance=synthetic; 640x480, unit increments, 1-120 Hz; "
                "initial 64x48@(0,0) 30 Hz 1 ms; alternate 48x32@(6,3) 60 Hz 2 ms; tight rows; IDs 1,2,..; "
                "host-monotonic time; optics 0.001 rad/px";
    return c;
}

BackendCase fixture_case() {
    BackendCase c;
    c.name = "contract-fixture";
    c.factory = [] { return contract_fixture::make_backend(); };
    c.settings.instance = "portable-fixture-session";
    c.settings.device_id = contract_fixture::device_id;
    c.settings.maximum_registered_buffers = 12;
    // VIRTUAL test optics, deliberately unequal and unlike the synthetic values.
    c.settings.horizontal_radians_per_pixel = 0.0005;
    c.settings.vertical_radians_per_pixel = 0.00075;
    c.settings.timestamp_domain = backend::ClockDomain::device;
    c.backend_id = contract_fixture::backend_id;
    c.device_id = contract_fixture::device_id;
    c.initial = {80, 60, 4, 6, 40, 1'250'000ns};
    c.alternate = {96, 64, 8, 10, 50, 2'000'000ns};
    // 384x256 (prevalidated by the provider), increments and 10-90 Hz (backend
    // apply; ROI -> rate -> exposure with truthful partial application).
    c.probe_outcomes = {Outcome::rejected_by_backend, Outcome::rejected_by_backend, Outcome::rejected_before_apply,
                        Outcome::rejected_before_apply, Outcome::rejected_by_backend};
    // Fixture model: SENSOR coordinates (ROI offset + local) and the native
    // ID's low byte, which equals the MEL frame ID's low byte.
    c.pixel = [](std::uint32_t x, std::uint32_t y, const backend::CameraConfiguration& config, std::uint32_t id) {
        return contract_fixture::pixel(config.offset_x + x, config.offset_y + y, id);
    };
    c.first_native_id = contract_fixture::first_native_frame_id;
    c.native_id_step = contract_fixture::native_frame_id_step;
    // Virtual device clock from 7,200 s, advancing exactly one period per frame.
    c.time.absolute = [](auto t, auto, auto, const backend::CameraConfiguration&) {
        const std::chrono::nanoseconds origin{contract_fixture::virtual_clock_origin_ns};
        return t >= origin && t < origin + 1h;
    };
    c.time.spacing = [](auto dt, std::uint64_t dk, const backend::CameraConfiguration& config) {
        return dk > 0 && dt == static_cast<std::int64_t>(dk) * period(config);
    };
    c.profile = "backend=contract-fixture device=fixture-camera-17 instance=portable-fixture-session; 384x256 "
                "virtual sensor, increments W4 H2 X2 Y2, 10-90 Hz; initial 80x60@(4,6) 40 Hz 1.25 ms; alternate "
                "96x64@(8,10) 50 Hz 2 ms; padded rows (0xA5); IDs 0x2FFFFFFF0 + 7k; virtual device clock; "
                "virtual optics 0.0005/0.00075 rad/px";
    return c;
}

// ---------------------------------------------------------------------------
// Shared MEL client helpers.
struct Delivered {
    ir::FrameHeader header;
    std::shared_ptr<ir::Buffer> buffer;
    std::vector<std::byte> copy; // packed bytes read inside the callback, before any release
    std::chrono::nanoseconds received{};
    bool retained{true};
};

// Records every image. Deliveries before the retain limit keep their registered
// buffer; later ones are released inside the callback. The collector mutex is
// never held while the hook waits or while a buffer is released.
class Listener final : public ir::ImageListener {
public:
    std::function<void()> hook; // set only while acquisition is disabled

    void onImage(const ir::Channel&, const ir::FrameHeader& header, std::shared_ptr<ir::Buffer> buffer) override {
        Delivered d{header, buffer, {}, host_now(), true};
        const auto* first = static_cast<const std::byte*>(buffer->getImageAddress());
        d.copy.assign(first, first + static_cast<std::size_t>(header.getWidth()) * header.getHeight());
        {
            std::lock_guard lock(mutex_);
            d.retained = records_.size() < retain_until_;
            records_.push_back(d);
        }
        if (hook) hook();
        if (!d.retained && buffer->release() != ir::Return::Success)
            throw std::runtime_error("listener could not release a delivered buffer");
        { std::lock_guard lock(mutex_); ++completed_; }
        changed_.notify_all();
    }
    // Retain exactly the next `count` deliveries; release every later one.
    void retain_next(std::size_t count) {
        std::lock_guard lock(mutex_);
        retain_until_ = records_.size() + count;
    }
    void wait_for(std::size_t count) {
        std::unique_lock lock(mutex_);
        CHECK(changed_.wait_for(lock, 3s, [&] { return completed_ >= count; }));
    }
    std::size_t count() const { std::lock_guard lock(mutex_); return completed_; }
    Delivered at(std::size_t index) const { std::lock_guard lock(mutex_); return records_.at(index); }
    std::vector<Delivered> since(std::size_t first) const {
        std::lock_guard lock(mutex_);
        return {records_.begin() + static_cast<std::ptrdiff_t>(std::min(first, records_.size())), records_.end()};
    }
    void require_quiet(std::chrono::milliseconds interval = 120ms) {
        std::unique_lock lock(mutex_);
        const auto before = completed_;
        CHECK(!changed_.wait_for(lock, interval, [&] { return completed_ != before; }));
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Delivered> records_;
    std::size_t completed_{0};
    std::size_t retain_until_{0}; // by default every delivery is released in the callback
};

// One MEL client session against one case. Caller-owned image storage is
// declared first and destroyed last, after acquisition and Health are stopped,
// every buffer is released and every channel is detached.
struct Session {
    std::deque<std::vector<std::byte>> storage;
    const BackendCase& kase;
    std::shared_ptr<ProbeLog> log{std::make_shared<ProbeLog>()};
    std::shared_ptr<Listener> listener{std::make_shared<Listener>()};
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::shared_ptr<ir::ImageChannel> image;
    std::shared_ptr<ir::C2Channel> c2;
    std::shared_ptr<ir::HealthStatusChannel> health;
    std::vector<std::shared_ptr<ir::Buffer>> buffers;

    explicit Session(const BackendCase& selected, bool initialize = true) : kase(selected) {
        manager = gige_mel::make_api_manager(kase.settings, std::make_shared<ProbeBackend>(kase.factory(), log));
        CHECK(manager);
        control = ir::getControl(kase.settings.instance, manager);
        CHECK(control);
        if (initialize) {
            CHECK(control->init() == ir::Return::Success);
            attach();
        }
    }
    ~Session() {
        try { if (image) (void)image->disable(); } catch (...) {}
        try { if (health) (void)health->disable(); } catch (...) {}
        for (const auto& buffer : buffers) {
            try { (void)buffer->release(); } catch (...) {}
        }
        if (control) {
            try { if (image) (void)control->detachChannel(image); } catch (...) {}
            try { if (c2) (void)control->detachChannel(c2); } catch (...) {}
            try { if (health) (void)control->detachChannel(health); } catch (...) {}
        }
        image.reset();
        c2.reset();
        health.reset();
        control.reset();
        buffers.clear();
        manager.reset();
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    ir::Config config(ir::ChannelType type) const {
        const auto caps = control->getCapabilities();
        const auto found = std::find_if(caps.begin(), caps.end(),
                                        [type](const auto& cap) { return cap.getChannelTypes() == std::vector{type}; });
        CHECK(found != caps.end());
        ir::Config selected;
        selected.setChanID(found->getChanID());
        selected.setChannelType(type);
        if (type != ir::ChannelType::HealthAndStatus) selected.setImgLstnr(listener);
        return selected;
    }
    void attach() {
        image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(config(ir::ChannelType::IRSTImage)));
        c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(config(ir::ChannelType::CommandAndControl)));
        health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(
            control->attachChannel(config(ir::ChannelType::HealthAndStatus)));
        CHECK(image && c2 && health);
    }
    // Registers `count` buffers sized from the CURRENT advertised geometry.
    void add_buffers(std::size_t count) {
        const auto size = static_cast<std::size_t>(image->getCapabilities().getBufferSize());
        CHECK(size > 0);
        for (std::size_t i = 0; i < count; ++i) {
            storage.emplace_back(size);
            auto buffer = ir::getBuffer(kase.settings.instance, manager);
            CHECK(buffer);
            CHECK(buffer->init(storage.back().data(), size, static_cast<std::int64_t>(buffers.size())) ==
                  ir::Return::Success);
            CHECK(image->registerBuffer(buffer) == ir::Return::Success);
            buffers.push_back(buffer);
        }
    }
    void unregister_all() {
        for (const auto& buffer : buffers) {
            (void)buffer->release(); // already released buffers report Fail
            CHECK(image->unregisterBuffer(buffer) == ir::Return::Success);
        }
        buffers.clear();
    }
};

// Native frame index k for a published MEL frame ID under the case's documented
// model (first + k * step, low 32 bits). Gaps are legitimate.
std::uint64_t native_index(const BackendCase& kase, std::uint32_t mel_id) {
    for (std::uint64_t k = 0; k < (1U << 20); ++k)
        if (static_cast<std::uint32_t>((kase.first_native_id + k * kase.native_id_step) & 0xffffffffULL) == mel_id) return k;
    throw std::runtime_error("published frame ID " + std::to_string(mel_id) + " is not in the case's native-ID model");
}

void check_geometry(const ir::ChannelCapability& cap, const backend::CameraConfiguration& c) {
    CHECK(cap.getWidth() == c.width && cap.getHeight() == c.height);
    CHECK(cap.getRowPitch() == c.width);
    CHECK(cap.getBufferSize() == c.width * c.height && cap.getImageSize() == c.width * c.height);
    CHECK(cap.getBitDepth() == 8 && cap.getNumberOfBands() == 1 && cap.getFormat() == ir::PixelFormat::Mono);
}

// Common published-frame guarantees, with every expectation from case data.
// `bytes` are the packed MEL destination bytes (copied in the callback or read
// later from a retained buffer).
void check_frame(const BackendCase& kase, const Delivered& d, const backend::CameraConfiguration& c,
                 const std::vector<std::byte>& bytes, std::chrono::nanoseconds before_enable) {
    const auto& h = d.header;
    CHECK(h.getWidth() == c.width && h.getHeight() == c.height);
    CHECK(h.getColumnOffset() == c.offset_x && h.getRowOffset() == c.offset_y);
    CHECK(h.getIntegrationTime() == c.exposure);
    CHECK(h.getFormat() == ir::PixelFormat::Mono && h.getNumBands() == 1 && h.getBitsPerPixel() == 8);
    CHECK(h.getSubframeID() == 0 && h.getSubframeTotal() == 1 && h.getImageType() == ir::ImageType::Staring);
    (void)native_index(kase, h.getFrameID());
    CHECK(kase.time.absolute(h.getSystemTime(), before_enable, d.received, c));
    const double expected_h = c.width * kase.settings.horizontal_radians_per_pixel;
    const double expected_v = c.height * kase.settings.vertical_radians_per_pixel;
    CHECK(std::abs(h.getHorizontalFieldOfView() - expected_h) <= 1e-12);
    CHECK(std::abs(h.getVerticalFieldOfView() - expected_v) <= 1e-12);
    // Every published pixel, row by row, against the documented model. Source
    // row padding must never appear in the tightly packed destination.
    CHECK(bytes.size() == static_cast<std::size_t>(c.width) * c.height);
    for (std::uint32_t y = 0; y < c.height; ++y) {
        for (std::uint32_t x = 0; x < c.width; ++x) {
            const auto actual = std::to_integer<std::uint8_t>(bytes[static_cast<std::size_t>(y) * c.width + x]);
            if (actual != kase.pixel(x, y, c, h.getFrameID()))
                throw std::runtime_error("pixel mismatch at x=" + std::to_string(x) + " y=" + std::to_string(y) +
                                         " frame=" + std::to_string(h.getFrameID()) + ": got " + std::to_string(actual) +
                                         ", model " + std::to_string(kase.pixel(x, y, c, h.getFrameID())));
        }
    }
}

// Frames of ONE acquisition: increasing native index (gaps allowed) and time
// spacing consistent with the case's documented clock.
void check_sequence(const BackendCase& kase, const std::vector<Delivered>& frames, const backend::CameraConfiguration& c) {
    for (std::size_t i = 1; i < frames.size(); ++i) {
        const auto k0 = native_index(kase, frames[i - 1].header.getFrameID());
        const auto k1 = native_index(kase, frames[i].header.getFrameID());
        CHECK(k1 > k0);
        CHECK(kase.time.spacing(frames[i].header.getSystemTime() - frames[i - 1].header.getSystemTime(), k1 - k0, c));
    }
}

std::vector<std::byte> read_buffer(const Delivered& d) {
    const auto* first = static_cast<const std::byte*>(d.buffer->getImageAddress());
    return {first, first + static_cast<std::size_t>(d.header.getWidth()) * d.header.getHeight()};
}

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
    command.setSystemTime(-5ns);
    command.setConfig(R"({"schema":"gige-mel.camera-config","version":1,"width":)" + std::to_string(c.width) +
                      R"(,"height":)" + std::to_string(c.height) + R"(,"offset_x":)" + std::to_string(c.offset_x) +
                      R"(,"offset_y":)" + std::to_string(c.offset_y) + R"(,"frame_rate_hz":)" +
                      std::to_string(c.frame_rate_hz) + R"(,"exposure_ns":)" + std::to_string(c.exposure.count()) + "}");
    return command;
}

// Metadata collector. The callback copies the payload and notifies with the
// mutex released; waits select by payload predicate after a captured baseline.
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
    std::vector<T> all() { std::lock_guard lock(mutex); return items; }
    T last() { std::lock_guard lock(mutex); CHECK(!items.empty()); return items.back(); }
    // First report at index >= after that satisfies the predicate; returns its index.
    template <class Predicate> std::pair<std::size_t, T> wait_match(std::size_t after, Predicate predicate) {
        std::unique_lock lock(mutex);
        std::optional<std::pair<std::size_t, T>> found;
        CHECK(changed.wait_for(lock, 3s, [&] {
            for (auto i = after; i < items.size(); ++i)
                if (predicate(items[i])) { found.emplace(i, items[i]); return true; }
            return false;
        }));
        return *found;
    }
};

std::map<std::string, std::string> detail_values(const mel::MFA_StatusDetailed& report) {
    std::map<std::string, std::string> values;
    for (const auto& pair : report.getStatus()) CHECK(values.emplace(pair.getName(), pair.getValue()).second);
    CHECK(values.size() == 12);
    return values;
}

const ir::SubsystemCSCIInfo& only_csci(const std::vector<ir::SubsystemCSCIInfo>& list) {
    CHECK(list.size() == 1);
    return list.front();
}

mel::UCI_ID expected_bit_id() {
    return {{0x9c, 0x85, 0xc2, 0x63, 0x54, 0x6e, 0x4e, 0x11, 0x9b, 0x71, 0xd2, 0x40, 0x5a, 0x13, 0x80, 0x01},
            "gige_mel.provider_state.v1"};
}

// ===========================================================================
// Shared scenario A: initialization, identity and discovery.
void identity_and_discovery(const BackendCase& kase) {
    auto log = std::make_shared<ProbeLog>();
    auto manager = gige_mel::make_api_manager(kase.settings, std::make_shared<ProbeBackend>(kase.factory(), log));
    CHECK(manager);
    CHECK(log->opened().empty() && log->enumerate_count() == 0); // construction: no device I/O
    // Lookup uses settings.instance; any other name, including the other
    // case's instance and the standard factory's "synthetic", is rejected.
    for (const std::string& other : {std::string("synthetic"), std::string("portable-fixture-session"),
                                     kase.settings.instance + "-other", std::string()}) {
        if (other == kase.settings.instance) continue;
        CHECK(!ir::getControl(other, manager));
        CHECK(!ir::getBuffer(other, manager));
    }
    auto control = ir::getControl(kase.settings.instance, manager);
    CHECK(control && ir::getControl(kase.settings.instance, manager) == control);
    CHECK(ir::getBuffer(kase.settings.instance, manager));
    CHECK(control->getCapabilities().empty());
    CHECK(log->opened().empty()); // Control creation: still no device I/O
    CHECK(control->init() == ir::Return::Success);
    CHECK(log->opened() == std::vector<std::string>{kase.settings.device_id}); // opens exactly the stable ID
    CHECK(control->init() == ir::Return::Success);
    CHECK(log->opened().size() == 1);
    CHECK(log->readback() == kase.initial);
    control.reset();
    manager.reset();

    Reports<mel::MFA_StatusDetailed> details; // outlives the Session's Health drain
    Session s(kase);
    const auto discovered = s.control->getCapabilities();
    CHECK(discovered.size() == 3);
    for (const auto& cap : discovered) {
        if (cap.getChannelTypes() == std::vector{ir::ChannelType::HealthAndStatus})
            CHECK(cap.getWidth() == 0 && cap.getHeight() == 0 && cap.getBufferSize() == 0);
        else check_geometry(cap, kase.initial);
    }
    check_geometry(s.image->getCapabilities(), kase.initial);
    check_geometry(s.c2->getCapabilities(), kase.initial);
    const auto health_cap = s.health->getCapabilities();
    CHECK(health_cap.getWidth() == 0 && health_cap.getHeight() == 0 && health_cap.getBufferSize() == 0);
    CHECK(health_cap.getImageSize() == 0);

    CHECK(s.health->registerMetadataCallback(details.callback()) == ir::Return::Success);
    CHECK(s.health->enable() == ir::Return::Success);
    const auto values = detail_values(details.wait_match(0, [](const auto&) { return true; }).second);
    CHECK(values.at("gige_mel.backend_id") == kase.backend_id);
    CHECK(values.at("gige_mel.device_id") == kase.device_id);
    CHECK(values.at("gige_mel.acquisition_state") == "idle");
    CHECK(s.health->disable() == ir::Return::Success);
}

// ===========================================================================
// Shared scenario B: configuration and capability validation.
void check_status(const ir::CommandStatus& status, std::uint32_t id, ir::CannotComply reason) {
    CHECK(status.getCommandID() == id);
    CHECK(status.getState() == (reason == ir::CannotComply::NotSet ? ir::CommandState::Accepted : ir::CommandState::Rejected));
    CHECK(status.getReasonID() == reason);
    CHECK(status.getReasonDescription().empty() == (reason == ir::CannotComply::NotSet));
}

void configuration_and_capabilities(const BackendCase& kase) {
    Reports<ir::CommandStatus> statuses;   // C2 CommandStatus
    Reports<ir::CameraCommandResp> replies; // Image CameraCommandResp
    Session s(kase);
    CHECK(s.c2->registerMetadataCallback(statuses.callback()) == ir::Return::Success);
    CHECK(s.image->registerMetadataCallback(replies.callback()) == ir::Return::Success);

    // C2 CameraCommand: ready future, correlated Image reply, then C2 status.
    auto reply = response(s.c2->send(camera_command(7001, kase.alternate)));
    CHECK(reply->getCommandID() == 7001 && reply->getCommandSuccess());
    CHECK(replies.count() == 1 && replies.last().getCommandID() == 7001 && replies.last().getCommandSuccess());
    CHECK(statuses.count() == 1);
    check_status(statuses.last(), 7001, ir::CannotComply::NotSet);
    CHECK(s.log->readback() == kase.alternate);
    check_geometry(s.image->getCapabilities(), kase.alternate);
    check_geometry(s.c2->getCapabilities(), kase.alternate);

    // Image CameraCommand: Image reply only, no C2 status.
    reply = response(s.image->send(camera_command(7002, kase.initial)));
    CHECK(reply->getCommandID() == 7002 && reply->getCommandSuccess());
    CHECK(replies.count() == 2 && replies.last().getCommandID() == 7002);
    CHECK(statuses.count() == 1);
    CHECK(s.log->readback() == kase.initial);
    check_geometry(s.image->getCapabilities(), kase.initial);

    // Version-1 ConfigSet through C2: Success future and Accepted status, no Image reply.
    CHECK(*response(s.c2->send(config_set(7003, kase.alternate))) == ir::Return::Success);
    CHECK(statuses.count() == 2 && replies.count() == 2);
    check_status(statuses.last(), 7003, ir::CannotComply::NotSet);
    CHECK(s.log->readback() == kase.alternate);
    check_geometry(s.image->getCapabilities(), kase.alternate);

    // Shared capability probes, each judged by THIS case's capabilities.
    CHECK(kase.probe_outcomes.size() == capability_probes.size());
    for (std::size_t i = 0; i < capability_probes.size(); ++i) {
        const auto& probe = capability_probes[i];
        const auto id = static_cast<std::uint32_t>(7100 + i);
        const auto applies = s.log->apply_count();
        const auto before = s.log->readback();
        const auto status_count = statuses.count();
        reply = response(s.c2->send(camera_command(id, probe.config)));
        CHECK(reply->getCommandID() == id);
        CHECK(statuses.count() == status_count + 1);
        const auto actual = s.log->readback();
        switch (kase.probe_outcomes[i]) {
        case Outcome::accepted:
            CHECK(reply->getCommandSuccess());
            check_status(statuses.last(), id, ir::CannotComply::NotSet);
            CHECK(s.log->apply_count() == applies + 1);
            CHECK(actual == probe.config);
            break;
        case Outcome::rejected_before_apply: // provider prevalidation: no backend apply
            CHECK(!reply->getCommandSuccess());
            check_status(statuses.last(), id, ir::CannotComply::InvalidInputParameter);
            CHECK(s.log->apply_count() == applies);
            CHECK(actual == before);
            break;
        case Outcome::rejected_by_backend: // backend application failure (possibly partial)
            CHECK(!reply->getCommandSuccess());
            check_status(statuses.last(), id, ir::CannotComply::InvalidInputParameter);
            CHECK(s.log->apply_count() == applies + 1);
            CHECK(actual != probe.config);
            break;
        }
        // Subsequent state is the actual readback, whatever the backend did.
        check_geometry(s.image->getCapabilities(), actual);
        CHECK(*response(s.c2->send(config_set(7200 + static_cast<std::uint32_t>(i), kase.alternate))) == ir::Return::Success);
        check_geometry(s.image->getCapabilities(), kase.alternate);
        std::cout << "  [" << kase.name << "] probe " << probe.label << ": "
                  << (kase.probe_outcomes[i] == Outcome::accepted ? "accepted"
                      : kase.probe_outcomes[i] == Outcome::rejected_before_apply ? "rejected by provider prevalidation"
                                                                                 : "rejected by backend apply")
                  << '\n';
    }

    // Active acquisition prevents every configuration path, without apply.
    s.add_buffers(2);
    CHECK(s.image->enable() == ir::Return::Success);
    const auto applies = s.log->apply_count();
    const auto status_count = statuses.count();
    expect_error(s.c2->send(camera_command(7301, kase.initial)), mel::ErrorCode::InvalidState);
    check_status(statuses.last(), 7301, ir::CannotComply::StateOrSettings);
    expect_error(s.image->send(camera_command(7302, kase.initial)), mel::ErrorCode::InvalidState);
    expect_error(s.c2->send(config_set(7303, kase.initial)), mel::ErrorCode::InvalidState);
    check_status(statuses.last(), 7303, ir::CannotComply::StateOrSettings);
    CHECK(statuses.count() == status_count + 2);
    CHECK(s.log->apply_count() == applies);
    CHECK(s.image->disable() == ir::Return::Success);
    check_geometry(s.image->getCapabilities(), kase.alternate);
}

// ===========================================================================
// Shared scenario C: image publication and row repacking, in two geometries.
void acquire_and_check(Session& s, const backend::CameraConfiguration& c, std::size_t frames) {
    check_geometry(s.image->getCapabilities(), c);
    s.add_buffers(3); // sized from the current advertised geometry
    const auto first = s.listener->count();
    const auto before = host_now();
    CHECK(s.image->enable() == ir::Return::Success);
    s.listener->wait_for(first + frames);
    CHECK(s.image->disable() == ir::Return::Success);
    const auto delivered = s.listener->since(first);
    CHECK(delivered.size() >= frames);
    for (const auto& d : delivered) check_frame(s.kase, d, c, d.copy, before);
    check_sequence(s.kase, delivered, c);
    s.unregister_all();
}

void publication_and_repacking(const BackendCase& kase) {
    Session s(kase);
    acquire_and_check(s, kase.initial, 4);
    CHECK(*response(s.c2->send(config_set(7401, kase.alternate))) == ir::Return::Success);
    acquire_and_check(s, kase.alternate, 4);
    const auto stats = gige_mel::provider_statistics(s.manager);
    CHECK(stats.rejected_frames == 0 && stats.stream_failures == 0 && stats.callback_failures == 0);
}

// ===========================================================================
// Shared scenario D: registered MEL destination retention, release, restart.
// (MEL destination ownership only: the provider has copied the source pixels.)
void retention_release_and_restart(const BackendCase& kase) {
    Session s(kase);
    const auto c = kase.initial;
    s.add_buffers(3);
    s.listener->retain_next(1); // hold the first delivery; later ones release in the callback
    const auto before = host_now();
    CHECK(s.image->enable() == ir::Return::Success);
    s.listener->wait_for(1);
    const auto held = s.listener->at(0);
    CHECK(held.retained);
    const auto snapshot = read_buffer(held);
    CHECK(snapshot == held.copy);
    check_frame(kase, held, c, snapshot, before);
    s.listener->wait_for(5); // acquisition continues through the other two slots
    CHECK(read_buffer(held) == snapshot);
    CHECK(s.image->disable() == ir::Return::Success);
    CHECK(read_buffer(held) == snapshot);
    for (const auto& d : s.listener->since(1)) CHECK(d.buffer != held.buffer); // the held slot was never reused
    CHECK(s.image->enable() == ir::Return::Fail); // outstanding registered buffer
    CHECK(read_buffer(held) == snapshot);
    CHECK(held.buffer->release() == ir::Return::Success);
    CHECK(held.buffer->release() == ir::Return::Fail);
    const auto restart = s.listener->count();
    const auto restarted_at = host_now();
    CHECK(s.image->enable() == ir::Return::Success);
    s.listener->wait_for(restart + 1);
    CHECK(s.image->disable() == ir::Return::Success);
    const auto fresh = s.listener->since(restart);
    for (const auto& d : fresh) check_frame(kase, d, c, d.copy, restarted_at);
    check_sequence(kase, fresh, c);
    CHECK(native_index(kase, fresh.front().header.getFrameID()) > native_index(kase, held.header.getFrameID()));
    s.listener->require_quiet();
}

// ===========================================================================
// Shared scenario E: Health, BIT and Subsystem through public callbacks.
// Freshness comes from payload predicates evaluated after captured baselines,
// never from report counts or from "delivered after the command".
bool csci_in(const ir::SubsystemStatusResp& report, ir::CSCIMode mode, ir::Failure failure) {
    const auto csci = report.getCSCI();
    return report.getFailureLevel() == failure && csci.size() == 1 && csci.front().getMode() == mode;
}
std::uint32_t bit_revision(const ir::SubsystemStatusResp& report) {
    const auto csci = report.getCSCI();
    return only_csci(csci).getBIT_report();
}
bool has_pass_completion(const mel::BIT_Status& report) {
    return report.getCompletedBITs().size() == 1 && report.getCompletedBITs().front().getBitID() == expected_bit_id() &&
           report.getCompletedBITs().front().getResult() == mel::BIT_Result::Pass;
}

void health_bit_and_subsystem(const BackendCase& kase) {
    Reports<mel::MFA_Status> statuses; // collectors outlive the Session's Health drain
    Reports<mel::MFA_StatusDetailed> details;
    Reports<mel::BIT_Status> bits;
    Reports<ir::SubsystemStatusResp> subsystems;
    Session s(kase);
    CHECK(s.health->registerMetadataCallback(statuses.callback()) == ir::Return::Success);
    CHECK(s.health->registerMetadataCallback(details.callback()) == ir::Return::Success);
    CHECK(s.health->registerMetadataCallback(bits.callback()) == ir::Return::Success);
    CHECK(s.health->registerMetadataCallback(subsystems.callback()) == ir::Return::Success);
    CHECK(s.health->enable() == ir::Return::Success);
    const auto running = [](const mel::MFA_StatusDetailed& d) { return detail_values(d).at("gige_mel.acquisition_state") == "running"; };
    const auto idle = [](const mel::MFA_StatusDetailed& d) { return detail_values(d).at("gige_mel.acquisition_state") == "idle"; };

    // Valid idle state.
    statuses.wait_match(0, [](const auto& r) { return r.getMFAState() == mel::MFA_State::Standby; });
    details.wait_match(0, idle);
    subsystems.wait_match(0, [](const auto& r) { return csci_in(r, ir::CSCIMode::Idle, ir::Failure::Available); });

    // Owned acquisition: Operate / running / Imaging. Every candidate report is
    // after the baseline captured once enable returned; the predicate decides.
    s.add_buffers(2);
    CHECK(s.image->enable() == ir::Return::Success);
    auto base = std::array{statuses.count(), details.count(), subsystems.count(), bits.count()};
    statuses.wait_match(base[0], [](const auto& r) { return r.getMFAState() == mel::MFA_State::Operate; });
    details.wait_match(base[1], running);
    const auto imaging = subsystems.wait_match(base[2], [](const auto& r) {
        return csci_in(r, ir::CSCIMode::Imaging, ir::Failure::Available);
    });
    s.listener->wait_for(1);

    // Provider-software BIT through C2 while acquiring. The fresh manager has
    // no previous completion, so any completion found is this execution's.
    for (const auto& r : bits.all()) CHECK(r.getCompletedBITs().empty());
    const auto revision_before = bit_revision(subsystems.last());
    CHECK(*response(s.c2->send(ir::BIT_Command{42, {1}, {}, {}})) == ir::Return::Success);
    const auto completed = bits.wait_match(base[3], has_pass_completion).second;
    CHECK(completed.getActiveBITs().empty() && completed.getFaults().empty());
    const auto& items = completed.getCompletedBITs().front().getBitItem();
    CHECK(items.size() == 2);
    for (const auto& item : items) CHECK(item.getResult() == mel::BIT_Result::Pass && item.getFailReason().empty());
    // Committed BIT-state revision is monotonic; a later Imaging report carries
    // a revision beyond the one captured before execution.
    subsystems.wait_match(imaging.first, [&](const auto& r) {
        return csci_in(r, ir::CSCIMode::Imaging, ir::Failure::Available) && bit_revision(r) > revision_before;
    });

    // Ordinary stop returns current state to idle.
    CHECK(s.image->disable() == ir::Return::Success);
    base = std::array{statuses.count(), details.count(), subsystems.count(), bits.count()};
    statuses.wait_match(base[0], [](const auto& r) { return r.getMFAState() == mel::MFA_State::Standby; });
    details.wait_match(base[1], idle);
    subsystems.wait_match(base[2], [](const auto& r) { return csci_in(r, ir::CSCIMode::Idle, ir::Failure::Available); });
    CHECK(s.health->disable() == ir::Return::Success);

    // Every Detailed report names the selected backend/device; every Subsystem
    // report identifies the provider software (not the camera backend); within
    // one Health interval prepared-report sequence numbers increase.
    for (const auto& d : details.all()) {
        const auto values = detail_values(d);
        CHECK(values.at("gige_mel.backend_id") == kase.backend_id && values.at("gige_mel.device_id") == kase.device_id);
    }
    const auto reports = subsystems.all();
    for (std::size_t i = 0; i < reports.size(); ++i) {
        const auto csci = reports[i].getCSCI();
        CHECK(reports[i].getCsciCount() == 1 && only_csci(csci).getCsci() == "ams-gra-gige-mel-provider");
        CHECK(only_csci(csci).getCsci() != kase.backend_id && only_csci(csci).getConnectionEstablished());
        if (i > 0) CHECK(reports[i].getStatusSeqNum() > reports[i - 1].getStatusSeqNum());
    }
}

// ===========================================================================
// Shared scenario F: bounded admitted-callback drain. An admitted Image
// callback waits at a gate; disable() must reach its actual acquisition-worker
// join (observed through the test pthread_join interposer, not merely task
// submission) and must not return until the callback is released.
void disable_drains_admitted_image_callback(const BackendCase& kase) {
    auto gate = std::make_shared<WatchdogGate>();
    std::atomic<unsigned> entries{0};
    Session s(kase);
    s.add_buffers(2);
    s.listener->hook = [gate, &entries] {
        if (entries.fetch_add(1) == 0) gate->enter_and_wait(); // collector mutex is not held here
    };
    std::atomic<bool> joining{false};
    std::future<ir::Return> disabled;
    // Declared after the future: opens the gate before the future's destructor
    // (or the Session's) can wait, on every unwind path.
    TestCleanup cleanup{[&] {
        gate->open();
        if (disabled.valid()) disabled.wait();
    }};
    CHECK(s.image->enable() == ir::Return::Success);
    gate->wait_for_entry();
    disabled = std::async(std::launch::async, [&] {
        test_join::Observe observation(joining);
        return s.image->disable();
    });
    eventually([&] { return joining.load(std::memory_order_acquire); }); // reached the worker join
    CHECK(disabled.wait_for(30ms) == std::future_status::timeout);     // ...and waits for the callback
    CHECK(s.listener->count() == 0);                                   // the held callback has not completed
    gate->open();
    CHECK(disabled.wait_for(2s) == std::future_status::ready);
    CHECK(disabled.get() == ir::Return::Success);
    gate->check();
    CHECK(entries == 1 && s.listener->count() == 1);
    s.listener->require_quiet(); // nothing published after disable returned
    const auto held = s.listener->at(0);
    check_frame(kase, held, kase.initial, held.copy, std::chrono::nanoseconds::min() / 2);
}

// ===========================================================================
// Fixture-specific direct backend checks (NOT shared MEL-provider guarantees and
// NOT requirements for future SDK adapters: pool size, IDs, generations and the
// allocation strategy are this fixture's documented semantics).
namespace cf = contract_fixture;

std::vector<std::byte> bytes_of(const backend::FrameLease& lease) {
    std::vector<std::byte> copy(lease.bytes().size());
    std::copy(lease.bytes().begin(), lease.bytes().end(), copy.begin());
    return copy;
}
// Compares the lease's current bytes in place (no temporary allocation).
bool unchanged(const backend::FrameLease& lease, const std::vector<std::byte>& snapshot) {
    return std::equal(lease.bytes().begin(), lease.bytes().end(), snapshot.begin(), snapshot.end());
}

void drop(backend::FrameLease& lease) { lease = backend::FrameLease({}, {}, {}); }

void check_fixture_frame(const backend::FrameLease& lease, std::uint64_t k, std::uint64_t configuration_generation,
                         const backend::CameraConfiguration& c) {
    const auto& d = lease.descriptor();
    CHECK(d.width == c.width && d.height == c.height && d.configuration == c);
    CHECK(d.row_stride_bytes == cf::row_stride(c.width) && d.row_stride_bytes > c.width);
    CHECK(lease.bytes().size() == d.row_stride_bytes * d.height);
    CHECK(d.native_frame_id == cf::first_native_frame_id + k * cf::native_frame_id_step);
    CHECK(d.stream_generation == cf::first_stream_generation && d.configuration_generation == configuration_generation);
    CHECK(d.timestamp.valid && d.timestamp.domain == backend::ClockDomain::device &&
          d.timestamp.event == backend::TimestampEvent::integration_start &&
          d.timestamp.clock_generation == cf::clock_generation);
    CHECK(d.timestamp.value ==
          std::chrono::nanoseconds{cf::virtual_clock_origin_ns} + static_cast<std::int64_t>(k) * period(c));
    for (std::uint32_t y = 0; y < c.height; ++y) {
        for (std::size_t x = 0; x < d.row_stride_bytes; ++x) {
            const auto value = lease.bytes()[y * d.row_stride_bytes + x];
            const auto expected = x < c.width
                ? std::byte{cf::pixel(c.offset_x + static_cast<std::uint32_t>(x), c.offset_y + y, d.native_frame_id)}
                : cf::padding_byte;
            CHECK(value == expected);
        }
    }
}

void fixture_source_lease_lifetime() {
    auto observed = std::make_shared<cf::Observation>();
    auto camera = cf::make_backend(cf::Options{2}, observed);
    CHECK(camera->id() == std::string_view(cf::backend_id));
    auto unknown = camera->open("synthetic-0");
    CHECK(!unknown && unknown.error().code == backend::ErrorCode::not_found);
    auto device = camera->open(cf::device_id).value();
    CHECK(camera->open(cf::device_id).error().code == backend::ErrorCode::busy);
    const auto initial = device->configuration().value();
    auto stream = device->create_stream().value();
    CHECK(stream->start(4242));

    auto lease_a = stream->receive(1s).value();
    check_fixture_frame(lease_a, 0, 4242, initial);
    auto lease_b = stream->receive(1s).value();
    check_fixture_frame(lease_b, 1, 4242, initial);
    // Bounded storage: both slots leased -> bounded timeout, one starvation event.
    auto starved = stream->receive(40ms);
    CHECK(!starved && starved.error().code == backend::ErrorCode::timeout && !starved.error().message.empty());
    CHECK(stream->statistics().pool_starvation_events == 1 && stream->statistics().timeout_count == 1);
    const auto* slot_b = lease_b.bytes().data();
    drop(lease_b); // its slot is recycled, never A's
    auto lease_c = stream->receive(1s).value();
    CHECK(lease_c.bytes().data() == slot_b);
    check_fixture_frame(lease_c, 2, 4242, initial);
    CHECK(observed->pools_created == 1);

    // Prompt cancellation of a receive that is actually blocked (pool full).
    {
        const auto waits = observed->receive_waits.load();
        auto blocked = std::async(std::launch::async, [&] { return stream->receive(10s); });
        TestCleanup cancel{[&] { stream->request_stop(); if (blocked.valid()) blocked.wait(); }};
        eventually([&] { return observed->receive_waits.load() > waits; });
        CHECK(blocked.wait_for(20ms) == std::future_status::timeout);
        const auto stop_at = std::chrono::steady_clock::now();
        stream->request_stop();
        CHECK(blocked.wait_for(1s) == std::future_status::ready);
        CHECK(std::chrono::steady_clock::now() - stop_at < 1s);
        auto cancelled = blocked.get();
        CHECK(!cancelled && cancelled.error().code == backend::ErrorCode::stopped);
    } // the receiving thread is joined before the stream can be destroyed
    auto restarted = stream->start(4243); // refused while this pool has leases
    CHECK(!restarted && restarted.error().code == backend::ErrorCode::resource_exhausted);

    // Stop, logical close, then release the external owners in valid order.
    const auto snapshot_a = bytes_of(lease_a);
    stream->stop();
    CHECK(device->close());
    auto closed = device->configuration();
    CHECK(!closed && closed.error().code == backend::ErrorCode::disconnected);
    stream.reset();
    device.reset();
    camera.reset();
    CHECK(observed->streams_retired == 1 && observed->devices_retired == 1 && observed->backends_retired == 1);
    CHECK(observed->pools_retired == 0 && observed->shared_states_retired == 0); // retained by the leases
    CHECK(unchanged(lease_a, snapshot_a));
    check_fixture_frame(lease_a, 0, 4242, initial);
    drop(lease_c);
    CHECK(observed->pools_retired == 0 && observed->shared_states_retired == 0); // A still retains both
    CHECK(unchanged(lease_a, snapshot_a));
    drop(lease_a);
    CHECK(observed->pools_retired == 1 && observed->shared_states_retired == 1); // final release retires once
    std::this_thread::sleep_for(20ms);
    CHECK(observed->pools_created == 1 && observed->pools_retired == 1 && observed->shared_states_retired == 1);
}

void fixture_close_cancellation_and_partial_apply() {
    auto observed = std::make_shared<cf::Observation>();
    auto camera = cf::make_backend(cf::Options{1}, observed);
    auto device = camera->open(cf::device_id).value();
    // Truthful partial application: ROI is written before the rate is rejected.
    auto partial = device->apply({96, 64, 8, 10, 95, 2ms});
    CHECK(!partial && partial.error().code == backend::ErrorCode::invalid_argument && !partial.error().message.empty());
    const auto actual = device->configuration().value();
    CHECK(actual.width == 96 && actual.height == 64 && actual.offset_x == 8 && actual.offset_y == 10);
    CHECK(actual.frame_rate_hz == 40 && actual.exposure == 1'250'000ns);
    auto stream = device->create_stream().value();
    CHECK(device->create_stream().error().code == backend::ErrorCode::busy);
    CHECK(stream->start(9));
    auto lease = stream->receive(1s).value();
    const auto snapshot = bytes_of(lease);
    {
        // Logical close wakes a receive blocked on the single-slot pool.
        const auto waits = observed->receive_waits.load();
        auto blocked = std::async(std::launch::async, [&] { return stream->receive(10s); });
        TestCleanup cancel{[&] { (void)device->close(); if (blocked.valid()) blocked.wait(); }};
        eventually([&] { return observed->receive_waits.load() > waits; });
        CHECK(device->close());
        CHECK(blocked.wait_for(1s) == std::future_status::ready);
        auto woken = blocked.get();
        CHECK(!woken && woken.error().code == backend::ErrorCode::disconnected);
    }
    CHECK(!stream->start(10) && !device->create_stream() && !device->apply(actual));
    stream.reset();
    device.reset();
    camera.reset();
    CHECK(unchanged(lease, snapshot) && observed->pools_retired == 0);
    drop(lease);
    CHECK(observed->pools_retired == 1 && observed->shared_states_retired == 1);
}

} // namespace

int main(int argc, char** argv) {
    test_join::prepare();
    const std::vector<BackendCase> cases{synthetic_case(), fixture_case()};
    using Scenario = void (*)(const BackendCase&);
    const std::vector<std::pair<const char*, Scenario>> shared{
        {"identity, instance lookup and discovery", identity_and_discovery},
        {"configuration commands and selected capabilities", configuration_and_capabilities},
        {"image publication and row repacking", publication_and_repacking},
        {"destination retention, release and restart", retention_release_and_restart},
        {"Health, BIT and Subsystem state", health_bit_and_subsystem},
        {"disable drains admitted image callback", disable_drains_admitted_image_callback},
    };
    const std::vector<std::pair<const char*, void (*)()>> fixture_only{
        {"source lease outlives stream, device and backend", fixture_source_lease_lifetime},
        {"close cancellation and truthful partial apply", fixture_close_cancellation_and_partial_apply},
    };
    const auto selected = [&](const std::string& label) { return argc != 2 || label.find(argv[1]) != std::string::npos; };
    std::size_t shared_passed = 0, fixture_passed = 0;
    std::string current;
    try {
        for (const auto& kase : cases) {
            std::cout << "CASE " << kase.name << ": " << kase.profile << '\n';
            for (const auto& [name, run] : shared) {
                current = "[" + kase.name + "] " + name;
                if (!selected(current)) continue;
                run(kase);
                ++shared_passed;
                std::cout << "PASS " << current << std::endl;
            }
        }
        for (const auto& [name, run] : fixture_only) {
            current = std::string("[contract-fixture direct] ") + name;
            if (!selected(current)) continue;
            run();
            ++fixture_passed;
            std::cout << "PASS " << current << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << current << ": " << error.what() << '\n';
        return 1;
    }
    std::cout << shared_passed << " shared scenario runs (" << cases.size() << " cases x " << shared.size()
              << " scenarios when unfiltered) and " << fixture_passed << " fixture-specific checks passed\n";
    return 0;
}
