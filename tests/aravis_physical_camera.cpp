// GIGE-MEL-018 OPT-IN PHYSICAL-CAMERA MEL TEST. Built only with
// -DGIGE_MEL_ARAVIS_HARDWARE_TESTS=ON (default OFF) and never part of hosted
// CI. It needs a reachable GigE Vision camera that already satisfies profile
// aravis-mono8-free-running-v1 (Mono8, TriggerMode Off, ExposureAuto Off,
// frame-rate control enabled); the test itself writes NO camera feature.
//
// Usage: aravis_physical_camera_tests DEVICE_ID [DISCOVERY_INTERFACE]
//
// Unchanged production pieces: aravis::make_backend() (GigE Vision only) ->
// the TEST-ONLY virtual integration-start clock decorator
// (tests/support/aravis_virtual_clock.*) -> gige_mel::make_api_manager() ->
// Control/Image/C2 -> registered consumer host buffers -> onImage.
//
//   G. PRODUCTION GATE: without the virtual clock every physical frame carries
//      the adapter's invalid FrameTimestamp{} and the provider rejects it.
//   V. TEST-ONLY MEL DELIVERY: with the virtual clock, genuine camera pixels
//      reach the consumer. systemTime is a declared TEST MODEL value, NOT
//      camera time, exposure start, PTP or UTC; not timestamp qualification.
//   S. Commanded Standby (GIGE-MEL-017): acquisition stops, held buffers keep
//      their bytes, explicit release, restart, normal cleanup.
// Optics are VIRTUAL test values, not a camera calibration.
#include <gige_mel/provider.hpp>
#include "aravis_backend.hpp"
#include "aravis_virtual_clock.hpp"

#include <arv.h>

#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
namespace backend = gige_mel::backend;
namespace aravis = gige_mel::backend::aravis;
namespace vc = aravis_virtual_clock;
using namespace std::chrono_literals;

namespace {

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression)) {                                                                         \
            throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) +           \
                                     ": " #expression);                                              \
        }                                                                                            \
    } while (false)

constexpr std::byte sentinel{0xA5};
constexpr std::chrono::milliseconds bound{15000}; // every wait is bounded; expiry is a failure
constexpr double h_rad_per_px = 0.0002;           // VIRTUAL test optics only
constexpr double v_rad_per_px = 0.0002;

std::string device_id;

bool unqualified(const backend::FrameTimestamp& t) {
    return !t.valid && t.value.count() == 0 && t.domain == backend::ClockDomain::unspecified &&
           t.event == backend::TimestampEvent::unspecified && t.clock_generation == 0;
}

std::vector<std::byte> region(const void* address, std::size_t size) {
    const auto* begin = static_cast<const std::byte*>(address);
    return {begin, begin + size};
}

// One MEL Image callback; checks run INSIDE the callback while the destination
// is exclusively the consumer's and the source SDK lease is still held.
struct Delivery {
    ir::FrameHeader header;
    std::shared_ptr<ir::Buffer> buffer;
    void* address{nullptr};
    std::int64_t context{0};
    std::vector<std::byte> bytes;
    std::optional<vc::SourceFrame> source;
    bool registered{false}, matches_sdk{false}, source_lease_held{false};
    std::size_t non_sentinel{0};
};

class Listener final : public ir::ImageListener {
public:
    std::vector<std::pair<const void*, std::int64_t>> registered; // set before enable
    std::shared_ptr<vc::Ledger> ledger;
    std::atomic<bool> auto_release{false};

    void onImage(const ir::Channel&, const ir::FrameHeader& header, std::shared_ptr<ir::Buffer> buffer) override {
        Delivery d;
        d.header = header;
        d.buffer = buffer;
        if (buffer) inspect(d, header, *buffer);
        { std::lock_guard lock(mutex_); deliveries_.push_back(std::move(d)); }
        changed_.notify_all();
        if (auto_release && buffer) (void)buffer->release();
    }
    std::size_t count() const { std::lock_guard lock(mutex_); return deliveries_.size(); }
    void wait_count(std::size_t n) const {
        std::unique_lock lock(mutex_);
        CHECK(changed_.wait_for(lock, bound, [&] { return deliveries_.size() >= n; }));
    }
    Delivery at(std::size_t i) const { std::lock_guard lock(mutex_); return deliveries_.at(i); }

private:
    void inspect(Delivery& d, const ir::FrameHeader& header, ir::Buffer& buffer) const {
        d.address = buffer.getBufferAddress();
        d.context = buffer.getContext();
        for (const auto& [address, context] : registered)
            d.registered = d.registered || (address == d.address && context == d.context);
        const std::size_t size = std::size_t{header.getWidth()} * header.getHeight();
        if (!d.address || static_cast<std::size_t>(buffer.getSize()) < size) return;
        const auto* begin = static_cast<const std::byte*>(d.address);
        d.bytes.assign(begin, begin + size);
        d.non_sentinel = static_cast<std::size_t>(
            std::count_if(d.bytes.begin(), d.bytes.end(), [](std::byte b) { return b != sentinel; }));
        d.source = ledger->by_frame_id(header.getFrameID());
        if (!d.source) return;
        const auto& s = *d.source;
        const auto& od = s.original;
        // The provider calls onImage while it holds the decorated lease of
        // THIS frame, so original_data still points at its genuine SDK bytes.
        d.source_lease_held = ledger->outstanding() >= 1;
        bool sdk = d.source_lease_held && s.original_data && od.width == header.getWidth() &&
                   od.height == header.getHeight() && s.original_size >= std::size_t{od.row_stride_bytes} * od.height;
        for (std::uint32_t y = 0; sdk && y < od.height; ++y)
            sdk = std::memcmp(d.bytes.data() + std::size_t{y} * od.width,
                              s.original_data + std::size_t{y} * od.row_stride_bytes, od.width) == 0;
        d.matches_sdk = sdk;
    }
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<Delivery> deliveries_;
};

// One MEL session over aravis::make_backend() (decorated). Members are
// declared so that storage, listener and observers outlive the provider.
struct Session {
    std::shared_ptr<vc::Clock> clock;
    std::shared_ptr<vc::Ledger> ledger = std::make_shared<vc::Ledger>();
    std::shared_ptr<Listener> listener = std::make_shared<Listener>();
    std::vector<std::vector<std::byte>> storage;
    std::shared_ptr<vc::Backend> decorated;
    gige_mel::ProviderSettings settings;
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::shared_ptr<ir::ImageChannel> image;
    std::shared_ptr<ir::C2Channel> c2;
    std::vector<std::shared_ptr<ir::Buffer>> buffers;
    std::vector<ir::CommandStatus> statuses; // C2 callbacks run synchronously in send()
    backend::CameraConfiguration configuration;

    explicit Session(bool with_clock) {
        if (with_clock) clock = std::make_shared<vc::Clock>();
        listener->ledger = ledger;
        decorated = std::make_shared<vc::Backend>(aravis::make_backend(), clock, ledger);
        settings.instance = "aravis-physical-camera-test";
        settings.device_id = device_id;
        settings.maximum_registered_buffers = 2;
        settings.horizontal_radians_per_pixel = h_rad_per_px;
        settings.vertical_radians_per_pixel = v_rad_per_px;
        settings.timestamp_domain = backend::ClockDomain::device; // the declared virtual domain
        manager = gige_mel::make_api_manager(settings, decorated);
        CHECK(manager);
        control = ir::getControl(settings.instance, manager);
        CHECK(control);
        if (control->init() != ir::Return::Success)
            throw std::runtime_error("Control::init failed for '" + device_id + "': " +
                                     gige_mel::provider_statistics(manager).last_error +
                                     " (the camera must already satisfy aravis-mono8-free-running-v1)");
        for (const auto& cap : control->getCapabilities()) {
            const auto types = cap.getChannelTypes();
            ir::Config config;
            config.setChanID(cap.getChanID());
            if (types.front() == ir::ChannelType::IRSTImage) {
                config.setChannelType(ir::ChannelType::IRSTImage);
                config.setImgLstnr(listener);
                image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(config));
            } else if (types == std::vector{ir::ChannelType::CommandAndControl}) {
                config.setChannelType(ir::ChannelType::CommandAndControl);
                c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(config));
            }
        }
        CHECK(image && c2);
        CHECK(c2->registerMetadataCallback(std::function<void(ir::Channel&, const ir::CommandStatus*)>{
                  [this](auto&, auto* status) { statuses.push_back(*status); }}) == ir::Return::Success);
        auto actual = decorated->last_opened_inner()->configuration(); // fresh native readback
        CHECK(actual);
        configuration = actual.value();
    }
    ~Session() {
        if (image) (void)image->disable();
        for (const auto& b : buffers) (void)b->release();
        if (c2 && control) (void)control->detachChannel(c2);
        if (image && control) (void)control->detachChannel(image);
        c2.reset();
        image.reset();
        control.reset();
        manager.reset();
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    std::size_t frame_bytes() const { return std::size_t{configuration.width} * configuration.height; }
    void add_buffer(std::int64_t context) {
        storage.reserve(4); // stable addresses: never reallocated after the first registration
        CHECK(storage.size() < 4);
        storage.emplace_back(frame_bytes(), sentinel);
        auto buffer = ir::getBuffer(settings.instance, manager);
        CHECK(buffer && buffer->init(storage.back().data(), frame_bytes(), context) == ir::Return::Success);
        CHECK(image->registerBuffer(buffer) == ir::Return::Success);
        listener->registered.emplace_back(storage.back().data(), context);
        buffers.push_back(buffer);
    }
    gige_mel::ProviderStatistics stats() const { return gige_mel::provider_statistics(manager); }
    // No new SDK frame during at least three frame periods (bounded).
    bool no_new_frames() const {
        const auto period = std::chrono::milliseconds{1000 / std::max<std::uint32_t>(1, configuration.frame_rate_hz)};
        const auto before = ledger->frames();
        std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(3 * period + 300ms, 5000ms));
        return ledger->frames() == before;
    }
};

void check_published(const Delivery& d, const vc::Model& model, const backend::CameraConfiguration& c, std::uint64_t run) {
    CHECK(d.buffer && d.registered && d.source);
    const auto& s = *d.source;
    CHECK(s.run == run && s.substituted);
    CHECK(unqualified(s.original.timestamp)); // a genuine production adapter frame
    CHECK(s.original.configuration == c && s.original.width == c.width && s.original.height == c.height);
    CHECK(s.original.pixel_format == backend::PixelFormat::mono8 && s.original.status == backend::FrameStatus::complete);
    CHECK(d.source_lease_held && d.matches_sdk); // destination == genuine SDK bytes, row by row
    CHECK(d.non_sentinel > 0);                   // the consumer memory was really written
    const auto declared = vc::nominal(model, s.sequence);
    CHECK(declared && d.header.getSystemTime() == declared->value); // TEST MODEL value, not camera time
    const auto& h = d.header;
    CHECK(h.getWidth() == c.width && h.getHeight() == c.height && h.getBitsPerPixel() == 8 && h.getNumBands() == 1);
    CHECK(h.getFormat() == ir::PixelFormat::Mono && h.getImageType() == ir::ImageType::Staring);
    CHECK(h.getColumnOffset() == c.offset_x && h.getRowOffset() == c.offset_y);
    CHECK(h.getIntegrationTime() == c.exposure); // actual configuration readback
    CHECK(h.getFrameID() == static_cast<std::uint32_t>(s.original.native_frame_id & 0xffffffffULL));
}

// G. The production timestamp gate on physical frames (no virtual clock).
void production_gate() {
    Session s(false);
    s.add_buffer(11);
    CHECK(s.image->enable() == ir::Return::Success);
    CHECK(s.ledger->wait_frames(3, bound));
    CHECK(s.image->disable() == ir::Return::Success);
    const auto stats = s.stats();
    CHECK(stats.delivered_frames == 0 && s.listener->count() == 0 && stats.rejected_frames >= 2);
    for (const auto& f : s.ledger->all()) CHECK(!f.substituted && unqualified(f.original.timestamp));
    const auto& block = s.storage.front();
    CHECK(std::all_of(block.begin(), block.end(), [](std::byte b) { return b == sentinel; }));
    std::cout << "  production gate: " << s.ledger->frames() << " physical frames received, " << stats.rejected_frames
              << " rejected (invalid FrameTimestamp), 0 published, destination untouched\n";
}

// V + S. Test-only MEL delivery, backpressure, commanded Standby, explicit
// release, restart and normal cleanup.
void delivery_standby_restart() {
    Session s(true);
    const auto& c = s.configuration;
    const vc::Model model{std::chrono::seconds{5000},
                          std::chrono::milliseconds{1000 / std::max<std::uint32_t>(1, c.frame_rate_hz)}, 18};
    s.clock->set_model(model);
    const auto cap = s.image->getCapabilities();
    CHECK(cap.getWidth() == c.width && cap.getHeight() == c.height && cap.getBufferSize() == s.frame_bytes());
    s.add_buffer(101);
    s.add_buffer(102);

    // Run 1: both destinations are held by the consumer (no release).
    CHECK(s.image->enable() == ir::Return::Success);
    s.listener->wait_count(2);
    const auto first = s.listener->at(0), second = s.listener->at(1);
    check_published(first, model, c, 1);
    check_published(second, model, c, 1);
    CHECK(first.address != second.address && second.source->sequence > first.source->sequence);
    // Backpressure: further physical frames are dropped; held bytes are kept.
    const auto base = s.stats();
    CHECK(s.ledger->wait_frames(s.ledger->frames() + 2, bound));
    CHECK(s.listener->count() == 2 && s.stats().dropped_no_buffer >= base.dropped_no_buffer + 1);
    CHECK(region(first.address, s.frame_bytes()) == first.bytes);
    CHECK(region(second.address, s.frame_bytes()) == second.bytes);

    // Commanded Standby through C2 (GIGE-MEL-017): synchronous stop.
    ir::ModeCmd command;
    command.setState(mel::MFA_State::Standby);
    command.setMode(ir::MFA_Mode::Unused);
    command.setCommandID(1801);
    auto future = s.c2->send(command);
    CHECK(future.wait_for(0ms) == std::future_status::ready);
    auto result = future.get();
    CHECK(result && *result.get() == ir::MFA_Mode::Unused);
    CHECK(s.statuses.size() == 1 && s.statuses.back().getCommandID() == 1801 &&
          s.statuses.back().getState() == ir::CommandState::Accepted);
    CHECK(s.ledger->wait_outstanding(0, bound));  // every SDK source lease returned
    CHECK(s.no_new_frames());                     // camera acquisition shut down
    CHECK(s.image->enable() == ir::Return::Fail); // existing contract: destinations still held
    CHECK(region(first.address, s.frame_bytes()) == first.bytes);
    CHECK(region(second.address, s.frame_bytes()) == second.bytes);

    // Explicit release, then restart acquisition.
    CHECK(first.buffer->release() == ir::Return::Success);
    CHECK(second.buffer->release() == ir::Return::Success);
    s.listener->auto_release = true;
    CHECK(s.image->enable() == ir::Return::Success);
    s.listener->wait_count(4);
    const auto restarted = s.listener->at(3);
    check_published(restarted, model, c, 2);
    CHECK(restarted.source->original.stream_generation != first.source->original.stream_generation);
    CHECK(s.image->disable() == ir::Return::Success);
    CHECK(s.ledger->wait_outstanding(0, bound));
    CHECK(s.no_new_frames());
    const auto stats = s.stats();
    CHECK(stats.stream_failures == 0 && stats.callback_failures == 0 && stats.rejected_frames == 0);
    std::cout << "  delivered " << stats.delivered_frames << " physical frames (" << c.width << 'x' << c.height
              << " Mono8, integration " << c.exposure.count() << " ns, " << c.frame_rate_hz << " Hz); held IDs "
              << first.header.getFrameID() << ',' << second.header.getFrameID() << ", " << stats.dropped_no_buffer
              << " dropped while held; Standby accepted (command 1801); stream generation "
              << first.source->original.stream_generation << " -> " << restarted.source->original.stream_generation
              << "; non-sentinel bytes in first destination " << first.non_sentinel << '/' << first.bytes.size()
              << "; systemTime is a TEST MODEL value\n";
}

// R. Clean device release: a fresh production backend can open and close the
// exact device after every manager is gone.
void clean_release() {
    auto camera = aravis::make_backend();
    auto opened = camera->open(device_id);
    if (!opened) throw std::runtime_error("reopen after release failed: " + opened.error().message);
    CHECK(opened.value()->close());
    std::cout << "  device reopened and closed by a fresh production backend\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || !*argv[1]) {
        std::cerr << "usage: aravis_physical_camera_tests DEVICE_ID [DISCOVERY_INTERFACE]\n"
                     "  OPT-IN physical camera test (GIGE-MEL-018); writes no camera feature.\n";
        return 2;
    }
    device_id = argv[1];
    if (argc == 3) arv_gv_interface_set_discovery_interface_name(argv[2]); // this test process only
    std::cout << "Aravis runtime " << aravis::runtime_version() << " (compiled " << aravis::compiled_version()
              << "); PHYSICAL device " << device_id << (argc == 3 ? std::string(" via ") + argv[2] : std::string())
              << "; TEST-ONLY virtual clock; virtual optics\n";
    if (aravis::runtime_version() != "0.8.36" || aravis::compiled_version() != "0.8.36") {
        std::cerr << "FAIL: the qualified Aravis 0.8.36 library is not the one loaded\n";
        return 1;
    }
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"G  production timestamp gate rejects physical frames", production_gate},
        {"V  test-only MEL delivery, Standby, release and restart", delivery_standby_restart},
        {"R  clean device release", clean_release},
    };
    for (const auto& [name, run] : tests) {
        try {
            run();
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
        std::cout << "PASS " << name << std::endl;
    }
    std::cout << tests.size() << " physical-camera scenarios passed (test-only virtual clock; NOT hardware "
              << "timestamp qualification)\n";
    return 0;
}
