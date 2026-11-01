// GIGE-MEL-010 timestamp contract against the REAL Aravis 0.8.36 SDK and its
// in-process Fake device. Three kinds of evidence are kept separate:
//
//  O. NATIVE TIMESTAMP OBSERVATION: raw SDK values read by the production
//     adapter's read-only observation path and the diagnostic's shared
//     time-probe routine. Observation is not qualification; the production
//     FrameDescriptor timestamp stays FrameTimestamp{} throughout.
//  V. TEST-ONLY VIRTUAL INTEGRATION-START CLOCK (tests/support/
//     aravis_virtual_clock.*): GENUINE SDK frames from the real adapter, whose
//     descriptor timestamp alone is replaced by a declared TEST MODEL value,
//     drive the UNCHANGED common provider to real MEL Image callbacks. This
//     qualifies MEL publication of Aravis pixels under virtual time. It is
//     NOT hardware, camera or Aravis timestamp qualification.
//  N. FAIL-CLOSED: the same genuine frames with unqualified or discontinuous
//     timestamps are rejected or terminate exactly as the provider requires.
//  P. GIGE-MEL-010-R1 DIAGNOSTIC REGRESSIONS: the production time-probe
//     serializer with synthetic private TimingContext values (provenance
//     wording, exact UTF-8), and the production time_probe() routine with
//     the real Fake, a scripted non-Aravis backend fixture, or the narrow
//     test-only checkpoint seam (output and native cleanup). The synthetic
//     GigE contexts exercise rendering only: no GigE stream is constructed
//     and no lost-frequency event is reproduced.
//
// Optics are VIRTUAL test values. The Fake fills buffers from the host wall
// clock (pinned src/arvfakecamera.c:786); no Fake exposure timing is claimed.
#include <gige_mel/provider.hpp>
#include "aravis_fake_support.hpp"
#include "aravis_virtual_clock.hpp"

#include <arv.h>
#include <nlohmann/json.hpp>

#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
namespace backend = gige_mel::backend;
namespace aravis = gige_mel::backend::aravis;
namespace vc = aravis_virtual_clock;
using namespace std::chrono_literals;
using Json = nlohmann::json;

namespace {

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression)) {                                                                         \
            throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) +           \
                                     ": " #expression);                                              \
        }                                                                                            \
    } while (false)

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

constexpr std::byte sentinel{0xA5};
constexpr std::chrono::milliseconds bound{10000}; // every wait is bounded; expiry is a failure
// VIRTUAL test optics only (not a camera calibration).
constexpr double h_rad_per_px = 0.0002;
constexpr double v_rad_per_px = 0.0003;

// Generation 1: 10 000 us exposure and GainRaw 0 make the pinned Fake ramp
// scale exactly 1.0. Generation 2: 100 000 us makes it exactly 2.0 (the
// oracle below is evaluated independently). Both are proven by readback.
const backend::CameraConfiguration gen1{64, 48, 8, 4, 100, 10ms};
const backend::CameraConfiguration gen2{80, 40, 16, 12, 10, 100ms};

// Independent pixel oracle of the pinned Fake Mono8 diagonal ramp
// (src/arvfakecamera.c arv_fake_camera_diagonal_ramp):
//   value = CLAMP(((x + frame_id + y) % 255) * scale, 0, 255),
//   scale = 1 + gain + log10(exposure_us / 10000)   (GainRaw 0 here).
unsigned expected_pixel(std::uint32_t x, std::uint32_t y, std::uint64_t frame_id, std::chrono::nanoseconds exposure) {
    const double exposure_us = static_cast<double>(exposure.count()) / 1000.0;
    const double scale = 1.0 + std::log10(exposure_us / 10000.0);
    const double value = static_cast<double>((x + frame_id + y) % 255) * scale;
    return static_cast<unsigned>(std::clamp(value, 0.0, 255.0));
}

template <class T> std::shared_ptr<T> response(mel::RequestFor<T> future) {
    CHECK(future.wait_for(0ms) == std::future_status::ready);
    auto result = future.get();
    CHECK(result);
    CHECK(result.get());
    return result.get();
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

template <class Predicate> bool eventually(Predicate predicate, std::chrono::milliseconds limit = bound) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// Imposes a GenICam access mode on a command through the SDK's public DOM, so
// that the SDK ITSELF later accepts or rejects execution (SDK-arranged).
ArvDomNode* impose_access(ArvDevice* d, const char* feature, const char* mode) {
    auto* document = ARV_DOM_DOCUMENT(arv_device_get_genicam(d));
    auto* node = ARV_DOM_NODE(arv_device_get_feature(d, feature));
    auto* imposed = ARV_DOM_NODE(arv_dom_document_create_element(document, "ImposedAccessMode"));
    CHECK(imposed && arv_dom_node_append_child(node, imposed));
    arv_dom_node_append_child(imposed, ARV_DOM_NODE(arv_dom_document_create_text_node(document, mode)));
    return imposed;
}
void set_access(ArvDomNode* imposed, const char* mode) {
    GError* error = nullptr;
    arv_gc_property_node_set_string(ARV_GC_PROPERTY_NODE(imposed), mode, &error);
    CHECK(!error);
}

// GObject weak-reference counter; never keeps the observed object alive.
struct Finalized {
    std::atomic<int> count{0};
    static void notify(gpointer data, GObject*) { static_cast<Finalized*>(data)->count.fetch_add(1); }
    void watch(gpointer object) { g_object_weak_ref(G_OBJECT(object), &Finalized::notify, this); }
};

// Native SDK observations through the adapter's existing observation-only
// hooks (no decoded-facts edit, no command-result substitution). Declared
// before any backend so it outlives everything it observes.
struct Observer {
    Finalized streams, buffers;
    std::atomic<int> streams_created{0}, buffers_allocated{0}, writes{0}, opens{0};
    std::mutex mutex;
    std::vector<std::string> commands;
    std::function<void(std::string_view, ArvDevice*)> on_command; // SDK arrangement, set per test
    aravis::TestHooks hooks() {
        aravis::TestHooks h;
        h.after_native_open = [this](ArvDevice*) { opens.fetch_add(1); };
        h.after_stream_created = [this](ArvStream* s) { streams_created.fetch_add(1); streams.watch(s); };
        h.after_buffer_allocated = [this](ArvBuffer* b) { buffers_allocated.fetch_add(1); buffers.watch(b); };
        h.before_write = [this](std::string_view, ArvDevice*) { writes.fetch_add(1); };
        h.before_command = [this](std::string_view c, ArvDevice* d) {
            { std::lock_guard lock(mutex); commands.emplace_back(c); }
            if (on_command) on_command(c, d);
        };
        return h;
    }
    std::vector<std::string> command_log() { std::lock_guard lock(mutex); return commands; }
    int live_streams() const { return streams_created.load() - streams.count.load(); }
    int live_buffers() const { return buffers_allocated.load() - buffers.count.load(); }
};

guint32 acquisition_register(backend::ICameraDevice& device) {
    guint32 value = 0xffffffffu;
    CHECK(aravis::with_native_device_for_testing(device, [&](ArvDevice* d) {
        value = arv_fake_camera_get_acquisition_status(arv_fake_device_get_fake_camera(ARV_FAKE_DEVICE(d)));
    }));
    return value;
}
std::int64_t native_integer(backend::ICameraDevice& device, const char* feature) {
    std::int64_t value = -1;
    CHECK(aravis::with_native_device_for_testing(device, [&](ArvDevice* d) {
        GError* error = nullptr;
        value = arv_device_get_integer_feature_value(d, feature, &error);
        CHECK(!error);
    }));
    return value;
}

// One MEL Image callback, with checks made INSIDE the callback while the
// destination buffer is still exclusively the consumer's: the destination
// bytes are compared with the ORIGINAL SDK bytes and with the pixel oracle.
struct Delivery {
    ir::FrameHeader header;
    std::shared_ptr<ir::Buffer> buffer;
    std::int64_t context{0};
    void* address{nullptr};
    std::vector<std::byte> bytes; // copy of the published destination image
    std::optional<vc::SourceFrame> source;
    bool registered{false};       // address/context belong to a registered allocation
    bool matches_sdk{false};      // every destination byte == original SDK byte (row by row)
    bool matches_oracle{false};   // every destination byte == independent Fake oracle
    bool source_lease_held{false}; // the SDK source lease was still alive in the callback
    bool stale{false};            // published from a run already disabled
};

class Listener final : public ir::ImageListener {
public:
    std::map<const void*, std::int64_t> registered; // set before enable; read-only afterwards
    std::shared_ptr<vc::Ledger> ledger;
    std::atomic<bool> auto_release{false};
    std::atomic<std::uint64_t> closed_run{0}; // a frame of a run <= this is stale

    void onImage(const ir::Channel&, const ir::FrameHeader& header, std::shared_ptr<ir::Buffer> buffer) override {
        Delivery d;
        d.header = header;
        d.buffer = buffer;
        if (buffer) inspect(d, header, *buffer);
        {
            std::lock_guard lock(mutex_);
            deliveries_.push_back(d);
        }
        changed_.notify_all();
        if (auto_release && buffer) (void)buffer->release();
    }
    // Drops the listener's own shared_ptr copies (NOT Buffer::release()).
    void drop_buffer_refs() {
        std::lock_guard lock(mutex_);
        for (auto& d : deliveries_) d.buffer.reset();
    }
    std::size_t count() const { std::lock_guard lock(mutex_); return deliveries_.size(); }
    Delivery at(std::size_t index) const {
        std::unique_lock lock(mutex_);
        CHECK(changed_.wait_for(lock, bound, [&] { return deliveries_.size() > index; }));
        return deliveries_[index];
    }
    void wait_count(std::size_t n) const {
        std::unique_lock lock(mutex_);
        CHECK(changed_.wait_for(lock, bound, [&] { return deliveries_.size() >= n; }));
    }
    std::vector<Delivery> all() const { std::lock_guard lock(mutex_); return deliveries_; }

private:
    void inspect(Delivery& d, const ir::FrameHeader& header, ir::Buffer& buffer) const {
        d.address = buffer.getBufferAddress();
        d.context = buffer.getContext();
        const auto found = registered.find(d.address);
        d.registered = found != registered.end() && found->second == d.context;
        const std::size_t size = std::size_t{header.getWidth()} * header.getHeight();
        if (!d.address || static_cast<std::size_t>(buffer.getSize()) < size) return;
        const auto* begin = static_cast<const std::byte*>(d.address);
        d.bytes.assign(begin, begin + size);
        // The header frame ID is the low 32 bits of the source frame ID; the
        // Fake's IDs are 16-bit, so it identifies the source frame.
        d.source = ledger->by_frame_id(header.getFrameID());
        if (!d.source) return;
        const auto& s = *d.source;
        d.stale = s.run <= closed_run.load();
        const auto& od = s.original;
        // The provider calls onImage while it still holds the decorated lease
        // of THIS frame, whose SourceHold keeps the original SDK lease, so
        // original_data still points at this frame's genuine SDK bytes.
        d.source_lease_held = ledger->outstanding() >= 1;
        bool sdk = d.source_lease_held && s.original_data && od.width == header.getWidth() &&
                   od.height == header.getHeight() && s.original_size >= od.row_stride_bytes * od.height;
        bool oracle = sdk;
        for (std::uint32_t y = 0; y < od.height && (sdk || oracle); ++y)
            for (std::uint32_t x = 0; x < od.width; ++x) {
                const auto actual = d.bytes[std::size_t{y} * od.width + x];
                if (sdk && actual != s.original_data[std::size_t{y} * od.row_stride_bytes + x]) sdk = false;
                if (std::to_integer<unsigned>(actual) != expected_pixel(x, y, od.native_frame_id, od.configuration.exposure))
                    oracle = false;
            }
        d.matches_sdk = sdk;
        d.matches_oracle = oracle;
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<Delivery> deliveries_;
};

// One MEL session over the decorated real-SDK Fake backend. Members are
// declared so that storage, listener and observers outlive the provider.
struct Session {
    std::shared_ptr<Observer> seen = std::make_shared<Observer>();
    std::shared_ptr<vc::Clock> clock;
    std::shared_ptr<vc::Ledger> ledger = std::make_shared<vc::Ledger>();
    std::shared_ptr<Listener> listener = std::make_shared<Listener>();
    std::vector<std::vector<std::byte>> storage;
    std::shared_ptr<vc::Backend> decorated;
    gige_mel::ProviderSettings settings;
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::shared_ptr<ir::ImageChannel> image;
    std::vector<std::shared_ptr<ir::Buffer>> buffers;

    // with_clock false: the SAME decorator as a pure pass-through (control).
    explicit Session(bool with_clock) {
        if (with_clock) clock = std::make_shared<vc::Clock>();
        listener->ledger = ledger;
        decorated = std::make_shared<vc::Backend>(aravis_fake::make_backend(seen->hooks()), clock, ledger);
        settings.instance = "aravis-virtual-clock-test";
        settings.device_id = std::string(aravis_fake::device_id);
        settings.maximum_registered_buffers = 4;
        settings.horizontal_radians_per_pixel = h_rad_per_px;     // VIRTUAL test optics
        settings.vertical_radians_per_pixel = v_rad_per_px;       // VIRTUAL test optics
        settings.timestamp_domain = backend::ClockDomain::device; // the declared virtual domain
        manager = gige_mel::make_api_manager(settings, decorated);
        CHECK(manager);
        control = ir::getControl(settings.instance, manager);
        CHECK(control && control->init() == ir::Return::Success); // opens Fake_1 via the real SDK
        for (const auto& cap : control->getCapabilities()) {
            if (cap.getChannelTypes().front() != ir::ChannelType::IRSTImage) continue;
            ir::Config config;
            config.setChanID(cap.getChanID());
            config.setChannelType(ir::ChannelType::IRSTImage);
            config.setImgLstnr(listener);
            image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(config));
        }
        CHECK(image);
    }
    // Teardown: disable (drains callbacks), release held destinations,
    // detach, then drop the manager so the Fake device is closed before the
    // next scenario opens it.
    ~Session() {
        if (image) (void)image->disable();
        for (const auto& b : buffers) (void)b->release();
        if (image && control) (void)control->detachChannel(image);
        image.reset();
        control.reset();
        manager.reset();
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    std::shared_ptr<backend::ICameraDevice> inner() const {
        auto device = decorated->last_opened_inner();
        CHECK(device);
        return device;
    }
    // Configure through MEL CameraCommand; prove the actual SDK readback.
    void configure(std::uint32_t id, const backend::CameraConfiguration& c) {
        const auto reply = response(image->send(camera_command(id, c)));
        CHECK(reply->getCommandSuccess());
        CHECK(inner()->configuration().value() == c); // fresh native readback
        CHECK(native_integer(*inner(), "GainRaw") == 0);
        const auto cap = image->getCapabilities();
        CHECK(cap.getWidth() == c.width && cap.getHeight() == c.height && cap.getBufferSize() == c.width * c.height);
    }
    std::shared_ptr<ir::Buffer> add_buffer(std::size_t bytes, std::int64_t context) {
        storage.reserve(16); // stable addresses: never reallocated after the first registration
        CHECK(storage.size() < 16);
        storage.emplace_back(bytes, sentinel);
        auto buffer = ir::getBuffer(settings.instance, manager);
        CHECK(buffer && buffer->init(storage.back().data(), bytes, context) == ir::Return::Success);
        CHECK(image->registerBuffer(buffer) == ir::Return::Success);
        listener->registered[storage.back().data()] = context;
        buffers.push_back(buffer);
        return buffer;
    }
    gige_mel::ProviderStatistics stats() const { return gige_mel::provider_statistics(manager); }
};

bool all_sentinel(const std::vector<std::byte>& block) {
    return std::all_of(block.begin(), block.end(), [](std::byte b) { return b == sentinel; });
}
std::vector<std::byte> region(const void* address, std::size_t size) {
    const auto* begin = static_cast<const std::byte*>(address);
    return {begin, begin + size};
}

bool unqualified(const backend::FrameTimestamp& t) {
    return !t.valid && t.domain == backend::ClockDomain::unspecified && t.event == backend::TimestampEvent::unspecified &&
           t.value.count() == 0 && t.clock_generation == 0;
}
bool same_timestamp(const backend::FrameTimestamp& a, const backend::FrameTimestamp& b) {
    return a.value == b.value && a.domain == b.domain && a.event == b.event && a.valid == b.valid &&
           a.clock_generation == b.clock_generation;
}
// Every descriptor field except the timestamp is preserved by the decorator.
bool same_except_timestamp(const backend::FrameDescriptor& a, const backend::FrameDescriptor& b) {
    return a.width == b.width && a.height == b.height && a.row_stride_bytes == b.row_stride_bytes &&
           a.storage_bits_per_pixel == b.storage_bits_per_pixel &&
           a.significant_bits_per_pixel == b.significant_bits_per_pixel && a.bands == b.bands &&
           a.pixel_format == b.pixel_format && a.byte_order == b.byte_order && a.status == b.status &&
           a.native_frame_id == b.native_frame_id && a.stream_generation == b.stream_generation &&
           a.configuration_generation == b.configuration_generation && a.configuration == b.configuration;
}
std::uint64_t next_fake_id(std::uint64_t id) { return id >= 65535 ? 1 : id + 1; } // pinned 16-bit counter, 0 skipped

// Full check of one PUBLISHED Image callback against the source frame, the
// declared virtual model and the actual configuration.
void check_published(const Delivery& d, const vc::Model& model, const backend::CameraConfiguration& c, std::uint64_t run) {
    CHECK(d.buffer && d.registered && d.source && !d.stale);
    const auto& s = *d.source;
    CHECK(s.run == run && s.substituted);
    const auto& o = s.original;
    const auto& v = s.delivered;
    // A genuine adapter frame: unqualified at the source, unchanged except time.
    CHECK(unqualified(o.timestamp));
    CHECK(same_except_timestamp(o, v));
    CHECK(o.configuration == c && o.width == c.width && o.height == c.height && o.row_stride_bytes == c.width);
    CHECK(o.pixel_format == backend::PixelFormat::mono8 && o.status == backend::FrameStatus::complete);
    CHECK(s.delivered_data == s.original_data && s.delivered_size == s.original_size);
    const auto declared = vc::nominal(model, s.sequence);
    CHECK(declared && same_timestamp(v.timestamp, *declared));
    // Pixels: the destination equals the ORIGINAL SDK bytes, row by row, and
    // the independent pinned-Fake oracle.
    CHECK(d.source_lease_held && d.matches_sdk && d.matches_oracle);
    // FrameHeader.
    const auto& h = d.header;
    CHECK(h.getWidth() == c.width && h.getHeight() == c.height);
    CHECK(h.getBitsPerPixel() == 8 && h.getNumBands() == 1 && h.getFormat() == ir::PixelFormat::Mono);
    CHECK(h.getImageType() == ir::ImageType::Staring && h.getImageFlip() == ir::ImageFlip::None);
    CHECK(h.getColumnOffset() == c.offset_x && h.getRowOffset() == c.offset_y);
    CHECK(h.getIntegrationTime() == c.exposure); // from the actual configuration readback
    // systemTime is the declared VIRTUAL integration start (a test model
    // value; it makes no claim about actual Fake exposure timing).
    CHECK(h.getSystemTime() == declared->value);
    CHECK(h.getFrameID() == static_cast<std::uint32_t>(o.native_frame_id & 0xffffffffULL));
    CHECK(h.getSubframeID() == 0 && h.getSubframeTotal() == 1);
    CHECK(h.getHorizontalFieldOfView() == static_cast<double>(c.width) * h_rad_per_px); // VIRTUAL optics
    CHECK(h.getVerticalFieldOfView() == static_cast<double>(c.height) * v_rad_per_px);   // VIRTUAL optics
}

std::shared_ptr<ir::Buffer> registered_at(const Session& s, const void* address) {
    for (const auto& b : s.buffers)
        if (b->getBufferAddress() == address) return b;
    throw std::runtime_error("no registered buffer at the delivered address");
}

struct ForeignStream final : backend::IFrameStream {
    backend::Status start(std::uint64_t) override { return {}; }
    backend::Result<backend::FrameLease> receive(std::chrono::milliseconds) override {
        return backend::Error{backend::ErrorCode::timeout, "foreign"};
    }
    void request_stop() noexcept override {}
    void stop() noexcept override {}
    backend::StreamStatistics statistics() const override { return {}; }
};

// ===========================================================================
// O. Native timestamp observation (raw SDK facts; never qualification).

// O1. The adapter's read-only observation path on genuine SDK Fake buffers.
void native_observation() {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = std::move(camera->open(aravis_fake::device_id)).value();
    CHECK(device->apply(gen1).value() == gen1);
    const int writes = seen.writes.load();
    const auto context = aravis::read_timing_context(*device);
    CHECK(context);
    const auto& c = context.value();
    CHECK(seen.writes.load() == writes); // read-only
    CHECK(c.native_interface == aravis::NativeInterface::fake && c.native_device_type == "ArvFakeDevice");
    CHECK(!c.tick_frequency_hz); // not an ArvGvDevice: no supported tick-frequency accessor
    const std::map<std::string, std::string> identity(c.identity.begin(), c.identity.end());
    CHECK(identity.count("DeviceVendorName") && identity.count("DeviceModelName"));
    CHECK(identity.count("GenICamXmlSha256") && identity.at("GenICamXmlSha256").size() == 64);
    std::cout << "  Fake timing context:";
    for (const auto& [name, value] : c.identity) std::cout << ' ' << name << "='" << value << '\'';
    std::cout << '\n';

    auto stream = std::move(device->create_stream()).value();
    const auto wall_before = static_cast<std::uint64_t>(g_get_real_time()) * 1000u;
    CHECK(stream->start(51));
    std::vector<aravis::ObservedFrame> held;
    for (int i = 0; i < 4; ++i) {
        auto received = aravis::receive_with_native_timestamps(*stream, 2000ms);
        CHECK(received);
        held.push_back(std::move(received).value());
    }
    const auto wall_after = static_cast<std::uint64_t>(g_get_real_time()) * 1000u;
    for (std::size_t i = 0; i < held.size(); ++i) {
        const auto& d = held[i].frame.descriptor();
        const auto& n = held[i].native;
        CHECK(unqualified(d.timestamp)); // observation never qualifies the descriptor
        CHECK(n.native_frame_id == d.native_frame_id && n.stream_generation == d.stream_generation);
        CHECK(n.width == gen1.width && n.height == gen1.height && n.offset_x == gen1.offset_x && n.offset_y == gen1.offset_y);
        // Consistent with the pinned source (src/arvfakecamera.c:786-787): one
        // host wall-clock sample stored in both fields during the fill. The
        // provenance claim rests on that source, not on these numbers.
        CHECK(n.sdk_buffer_timestamp_ns == n.sdk_system_timestamp_ns);
        CHECK(n.sdk_buffer_timestamp_ns >= wall_before && n.sdk_buffer_timestamp_ns <= wall_after);
        if (i) CHECK(n.native_frame_id == next_fake_id(held[i - 1].native.native_frame_id));
        for (std::uint32_t y = 0; y < d.height; ++y)
            for (std::uint32_t x = 0; x < d.width; ++x)
                CHECK(std::to_integer<unsigned>(held[i].frame.bytes()[std::size_t{y} * d.row_stride_bytes + x]) ==
                      expected_pixel(x, y, d.native_frame_id, gen1.exposure));
        std::cout << "  frame " << n.native_frame_id << " gen " << n.stream_generation << ": sdk_buffer_timestamp_ns "
                  << n.sdk_buffer_timestamp_ns << ", sdk_system_timestamp_ns " << n.sdk_system_timestamp_ns
                  << ", descriptor timestamp valid=false\n";
    }
    // Ownership and queue order unchanged: the 4 observed leases hold the
    // whole pool, so the next receive starves; releasing one requeues it and
    // a plain receive returns the next consecutive SDK frame.
    const auto starved = aravis::receive_with_native_timestamps(*stream, 50ms);
    CHECK(!starved && starved.error().code == backend::ErrorCode::timeout);
    CHECK(stream->statistics().pool_starvation_events >= 1);
    const auto last_id = held.back().native.native_frame_id;
    held.erase(held.begin());
    auto plain = stream->receive(2000ms);
    CHECK(plain && unqualified(plain.value().descriptor().timestamp));
    CHECK(plain.value().descriptor().native_frame_id == next_fake_id(last_id));
    CHECK(seen.buffers_allocated == 4); // the fixed pool; observation allocates no pixel buffer
    ForeignStream foreign; // a stream that is not the adapter's is refused, never observed
    CHECK(aravis::receive_with_native_timestamps(foreign, 0ms).error().code == backend::ErrorCode::invalid_argument);
    held.clear();
    { auto released = std::move(plain); }
    stream->stop();
    stream.reset();
    CHECK(device->close());
    CHECK(!aravis::read_timing_context(*device)); // logically closed: no device I/O
    CHECK(seen.live_streams() == 0 && seen.live_buffers() == 0);
}

std::string temp_path(const char* stem) {
    const char* dir = std::getenv("TMPDIR");
    return std::string(dir && *dir ? dir : "/tmp") + "/" + stem + "-" + std::to_string(::getpid()) + ".jsonl";
}
bool exists(const std::string& path) { return std::ifstream(path).good(); }

std::vector<Json> parse_jsonl(const std::string& text) {
    std::vector<Json> records;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        CHECK(end != std::string::npos && end - start + 1 <= aravis::time_probe_max_line_bytes);
        records.push_back(Json::parse(text.substr(start, end - start))); // throws on invalid JSON
        start = end + 1;
    }
    return records;
}

// O2a. The diagnostic's shared time-probe routine: strict validation and
// exclusive output creation before ANY device I/O; failures remove output.
void time_probe_arguments() {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    const auto path = temp_path("gige-mel-time-probe-args");
    std::remove(path.c_str());
    const auto code = [&](std::string_view id, std::uint32_t n, const std::string& out, std::chrono::milliseconds t) {
        const auto o = aravis::time_probe(*camera, id, n, out, t);
        CHECK(!o.ok() && o.error && !o.report);
        return o.error->code;
    };
    CHECK(code("", 3, path, 10000ms) == backend::ErrorCode::invalid_argument);
    CHECK(code(aravis_fake::device_id, 0, path, 10000ms) == backend::ErrorCode::invalid_argument);
    CHECK(code(aravis_fake::device_id, 33, path, 10000ms) == backend::ErrorCode::invalid_argument);
    CHECK(code(aravis_fake::device_id, 3, path, 0ms) == backend::ErrorCode::invalid_argument);
    CHECK(code(aravis_fake::device_id, 3, path, 10001ms) == backend::ErrorCode::invalid_argument);
    CHECK(code(aravis_fake::device_id, 3, "", 10000ms) == backend::ErrorCode::invalid_argument);
    CHECK(seen.opens.load() == 0 && !exists(path));
    { std::ofstream(path) << "keep"; } // an existing output is never overwritten
    CHECK(code(aravis_fake::device_id, 3, path, 10000ms) == backend::ErrorCode::io_error);
    CHECK(seen.opens.load() == 0);
    { std::ifstream in(path); std::string text; in >> text; CHECK(text == "keep"); }
    std::remove(path.c_str());
    // Operational failure (unknown exact ID): error, output removed.
    CHECK(code("Fake_404", 3, path, 10000ms) == backend::ErrorCode::not_found && !exists(path));
    CHECK(seen.live_streams() == 0);
}

// O2b. The actual probe against the private Fake interface (6 real frames).
// Every open of the pinned Fake interface creates a new ArvFakeCamera
// (src/arvfakeinterface.c:78, src/arvfakedevice.c:202), so the probe sees the
// Fake's own current defaults; it writes nothing and reports what it read.
void time_probe_fake() {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    const backend::CameraConfiguration fake_default{512, 512, 0, 0, 25, 10ms};
    const auto path = temp_path("gige-mel-time-probe");
    std::remove(path.c_str());
    const int writes = seen.writes.load();
    const auto wall_before = static_cast<std::uint64_t>(g_get_real_time()) * 1000u;
    const auto outcome = aravis::time_probe(*camera, aravis_fake::device_id, 6, path);
    const auto wall_after = static_cast<std::uint64_t>(g_get_real_time()) * 1000u;
    if (!outcome.ok())
        throw std::runtime_error("time probe failed: " + (outcome.error ? outcome.error->message : std::string()) +
                                 (outcome.cleanup_error ? " / " + outcome.cleanup_error->message : std::string()));
    CHECK(seen.writes.load() == writes); // no feature write
    CHECK((seen.command_log() == std::vector<std::string>{"AcquisitionStart", "AcquisitionStop"}));
    CHECK(seen.live_streams() == 0 && seen.live_buffers() == 0); // stopped, closed, leases released
    const auto& r = *outcome.report;
    CHECK(r.frames.size() == 6 && r.output_lines == 8);
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.size() == r.output_bytes && !text.empty() && text.back() == '\n');
    const auto records = parse_jsonl(text);
    CHECK(records.size() == 8);
    // Exactly one header, COUNT frame records and one summary, in order.
    for (std::size_t i = 0; i < records.size(); ++i)
        CHECK(records[i].at("record") == (i == 0 ? "header" : i + 1 == records.size() ? "summary" : "frame"));
    const auto& h = records.front();
    CHECK(h.at("record") == "header" && h.at("schema") == "gige-mel.aravis-time-probe" && h.at("schema_version") == 2);
    CHECK(h.at("device_id") == "Fake_1" && h.at("interface") == "fake" && h.at("native_device_type") == "ArvFakeDevice");
    CHECK(h.at("compiled_sdk_version") == "0.8.36" && h.at("runtime_sdk_version") == "0.8.36");
    // R1 schema 2: no GigE frequency for the in-process Fake (never guessed);
    // the superseded schema-1 key is gone; the Fake path is source-established.
    CHECK(h.at("diagnostic_tick_frequency_hz").is_null() && !h.contains("tick_frequency_hz"));
    CHECK(h.at("actual_stream_timestamp_path") == "pinned_fake_fill_host_wall_clock");
    CHECK(h.at("sdk_stream_timestamp_paths").is_null());
    CHECK(contains(h.at("timing_origin").get<std::string>(), "wall clock"));
    CHECK(contains(h.at("sdk_buffer_timestamp_source").get<std::string>(), "src/arvfakecamera.c:786"));
    CHECK(h.at("timestamp_qualified") == false && !h.at("qualification_reason").get<std::string>().empty());
    CHECK(h.at("identity").at("DeviceVendorName").is_string());
    CHECK(h.at("configuration").at("width") == fake_default.width && h.at("configuration").at("exposure_ns") == 10000000);
    CHECK(r.configuration == fake_default);
    CHECK(h.at("receive_deadline_ms") == 10000 && h.at("requested_frames") == 6);
    std::uint64_t previous_id = 0, generation = 0;
    for (std::size_t i = 1; i <= 6; ++i) {
        const auto& f = records[i];
        for (const char* key : {"device_id", "interface", "compiled_sdk_version", "runtime_sdk_version",
                                "stream_generation", "native_frame_id", "width", "height", "offset_x", "offset_y",
                                "sdk_buffer_timestamp_ns", "sdk_system_timestamp_ns", "timestamp_qualified",
                                "qualification_reason"})
            CHECK(f.contains(key));
        CHECK(f.at("record") == "frame" && f.at("index") == i - 1 && f.at("timestamp_qualified") == false);
        CHECK(f.at("descriptor_timestamp_valid") == false && f.at("interface") == "fake");
        CHECK(f.at("actual_stream_timestamp_path") == "pinned_fake_fill_host_wall_clock");
        CHECK(f.at("width") == fake_default.width && f.at("height") == fake_default.height);
        CHECK(f.at("offset_x") == fake_default.offset_x && f.at("offset_y") == fake_default.offset_y);
        const auto id = f.at("native_frame_id").get<std::uint64_t>();
        const auto ts = f.at("sdk_buffer_timestamp_ns").get<std::uint64_t>(); // unsigned, unconverted
        CHECK(ts == f.at("sdk_system_timestamp_ns").get<std::uint64_t>());
        CHECK(ts >= wall_before && ts <= wall_after);
        CHECK(id == r.frames[i - 1].native.native_frame_id && ts == r.frames[i - 1].native.sdk_buffer_timestamp_ns);
        if (i == 1) {
            generation = f.at("stream_generation").get<std::uint64_t>();
        } else {
            // No ordering of the values is asserted: the source is the host
            // wall clock, which may step; and an ordered series would prove
            // no event meaning anyway.
            CHECK(id == next_fake_id(previous_id) && f.at("stream_generation") == generation);
        }
        previous_id = id;
    }
    CHECK(records.back().at("record") == "summary" && records.back().at("frames") == 6);
    CHECK(records.back().at("timestamp_qualified") == false && records.back().at("delivered_frames") == 6);
    const auto first = text.find('\n');
    std::cout << "  time-probe header: " << text.substr(0, first) << '\n';
    std::cout << "  time-probe frame:  " << text.substr(first + 1, text.find('\n', first + 1) - first - 1) << '\n';
    std::remove(path.c_str());
}

// ===========================================================================
// R. GIGE-MEL-010-R1 regressions. Every case calls the PRODUCTION serializer
// (aravis::render_time_probe_report / time_probe_json_string) or the
// production time_probe() routine; nothing here re-implements either.

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
}

// A synthetic, private report around a given timing context (no device).
aravis::TimeProbeReport synthetic_report(aravis::TimingContext context, std::string device_id = "GV-Synthetic") {
    aravis::TimeProbeReport r;
    r.device_id = std::move(device_id);
    r.compiled_sdk_version = "0.8.36";
    r.runtime_sdk_version = "0.8.36";
    r.context = std::move(context);
    r.configuration = gen1;
    aravis::TimeProbeFrame frame;
    frame.native = {1, 7, gen1.width, gen1.height, gen1.offset_x, gen1.offset_y, 1000000000123ull, 1793501463400067000ull};
    r.frames = {frame, frame};
    r.statistics.delivered_frames = 2;
    return r;
}
std::vector<Json> render_synthetic(const aravis::TimeProbeReport& r) {
    const auto lines = aravis::render_time_probe_report(r, 10000ms, 2);
    if (!lines) throw std::runtime_error("production renderer refused a valid report: " + lines.error().message);
    std::string text;
    for (const auto& line : lines.value()) text += line;
    return parse_jsonl(text);
}

// P1. Rendered timing provenance, table-driven over synthetic contexts
// (A1 GigE nonzero, A2 GigE zero, A3 GigE read unavailable, A4 Fake). The
// diagnostic's pre-stream frequency is preserved as observed, but never
// identifies the stream's internal conversion/fallback branch.
void provenance_table() {
    struct Case {
        const char* name;
        aravis::NativeInterface native;
        const char* type;
        std::optional<std::uint64_t> frequency;
        bool renders;
    };
    constexpr auto big = std::numeric_limits<std::uint64_t>::max(); // preserved exactly, unconverted
    const std::vector<Case> cases{
        {"A1 GigE nonzero 1 GHz", aravis::NativeInterface::gige_vision, "ArvGvDevice", 1000000000ull, true},
        {"A1 GigE nonzero max", aravis::NativeInterface::gige_vision, "ArvGvDevice", big, true},
        {"A2 GigE zero", aravis::NativeInterface::gige_vision, "ArvGvDevice", 0ull, true},
        {"A3 GigE read unavailable", aravis::NativeInterface::gige_vision, "ArvGvDevice", std::nullopt, false},
        {"A4 Fake", aravis::NativeInterface::fake, "ArvFakeDevice", std::nullopt, true},
        {"A4 Fake with a frequency", aravis::NativeInterface::fake, "ArvFakeDevice", 1000000000ull, false},
        {"other with a frequency", aravis::NativeInterface::other, "ArvUv3Device", 1ull, false},
    };
    std::optional<Json> gige_reference; // GigE headers must not depend on the observed value
    for (const auto& c : cases) {
        aravis::TimingContext context;
        context.native_interface = c.native;
        context.native_device_type = c.type;
        context.identity = {{"DeviceVendorName", "Synthetic"}};
        context.tick_frequency_hz = c.frequency;
        const auto report = synthetic_report(context);
        const auto lines = aravis::render_time_probe_report(report, 10000ms, 2);
        if (!c.renders) {
            // A3: a missing GigE read result is never rendered as "absent"
            // (read_timing_context already returns the read error), and a
            // non-GigE frequency is never reported. No line is produced.
            if (lines) throw std::runtime_error(std::string(c.name) + ": rendered an inconsistent timing context");
            CHECK(lines.error().code == backend::ErrorCode::invalid_state);
            CHECK(contains(lines.error().message, c.native == aravis::NativeInterface::gige_vision
                                                       ? "never reported as an absent frequency"
                                                       : "only for an ArvGvDevice"));
            std::cout << "  " << c.name << ": refused (" << lines.error().message.substr(0, 70) << "...)\n";
            continue;
        }
        const auto records = render_synthetic(report);
        CHECK(records.size() == 4);
        const auto& h = records.front();
        CHECK(h.at("schema_version") == 2 && h.at("timestamp_qualified") == false && !h.contains("tick_frequency_hz"));
        for (const auto& record : records) CHECK(record.at("timestamp_qualified") == false);
        for (std::size_t i = 1; i <= 2; ++i) {
            CHECK(records[i].at("descriptor_timestamp_valid") == false);
            CHECK(records[i].at("sdk_buffer_timestamp_ns").get<std::uint64_t>() == 1000000000123ull); // unconverted
            CHECK(records[i].at("sdk_system_timestamp_ns").get<std::uint64_t>() == 1793501463400067000ull);
            CHECK(records[i].at("actual_stream_timestamp_path") == h.at("actual_stream_timestamp_path"));
        }
        const auto basis = h.at("actual_stream_timestamp_path_basis").get<std::string>();
        const auto source = h.at("sdk_buffer_timestamp_source").get<std::string>();
        const auto read = h.at("diagnostic_tick_frequency_read").get<std::string>();
        if (c.native == aravis::NativeInterface::gige_vision) {
            // The observed value is preserved exactly (A1 and A2 alike) ...
            CHECK(h.at("diagnostic_tick_frequency_hz").get<std::uint64_t>() == *c.frequency);
            CHECK(contains(read, "before stream creation") && contains(read, "NOT the value the SDK stream sampled") &&
                  contains(read, "not proof"));
            // ... and the actual stream path stays unverified whatever it is.
            CHECK(h.at("actual_stream_timestamp_path") == "unverified");
            CHECK(contains(basis, "not established by this diagnostic") && contains(source, "unverified"));
            // Both conditional SDK behaviors, from the pinned source.
            const auto& paths = h.at("sdk_stream_timestamp_paths");
            const auto origin = paths.at("stream_construction_frequency_source").get<std::string>();
            const auto nonzero = paths.at("stream_construction_frequency_nonzero").get<std::string>();
            const auto zero = paths.at("stream_construction_frequency_zero").get<std::string>();
            CHECK(contains(origin, "src/arvgvstream.c:1613") && contains(origin, "error is ignored"));
            CHECK(contains(nonzero, "leader ticks converted") && contains(nonzero, "src/arvgvstream.c:480-482"));
            CHECK(contains(nonzero, "proves no integration start, epoch, UTC, PTP, continuity or accuracy"));
            CHECK(contains(zero, "host reception wall-clock time") && contains(zero, "src/arvgvstream.c:484"));
            auto comparable = h;
            comparable.erase("diagnostic_tick_frequency_hz");
            if (!gige_reference) gige_reference = comparable;
            CHECK(comparable == *gige_reference); // A1 and A2 differ only in the number itself
            // The superseded reviewed claims of a definite path are gone.
            CHECK(!contains(h.dump(), "SDK conversion of the GVSP leader timestamp with the stream's"));
            CHECK(!contains(h.dump(), "probe-time tick frequency is 0"));
        } else {
            CHECK(h.at("diagnostic_tick_frequency_hz").is_null() && contains(read, "not an ArvGvDevice"));
            CHECK(h.at("actual_stream_timestamp_path") == "pinned_fake_fill_host_wall_clock");
            CHECK(contains(source, "src/arvfakecamera.c:786") && contains(basis, "src/arvfakestream.c:93"));
            CHECK(contains(basis, "no GigE stream is constructed") && h.at("sdk_stream_timestamp_paths").is_null());
        }
        std::cout << "  " << c.name << ": diagnostic_tick_frequency_hz=" << h.at("diagnostic_tick_frequency_hz").dump()
                  << ", actual_stream_timestamp_path=" << h.at("actual_stream_timestamp_path").dump() << '\n';
    }
    CHECK(gige_reference.has_value());
}

// P2. Exact UTF-8 through the production encoder: decoding the produced JSON
// with nlohmann/json returns the original string (B1-B5); malformed input is
// an explicit error, never mojibake or U+FFFD (B6).
const std::vector<std::string>& valid_utf8() {
    static const std::vector<std::string> valid{
        "Aravis Fake 0.8.36",                                                               // B1 ASCII
        "C\xC3\xA1mara \xE2\x80\x93 K\xC3\xB6nig",                                          // B2 "Cámara – König"
        "\xE3\x82\xAB\xE3\x83\xA1\xE3\x83\xA9 \xCE\x9A\xCE\xAC\xCE\xBC\xCE\xB5\xCF\x81\xCE\xB1", // B3 "カメラ Κάμερα"
        "Cam \xF0\x9F\x93\xB7 \xF4\x8F\xBF\xBF",                                            // B4 U+1F4F7, U+10FFFF
        std::string("q\"b\\s/ \n\r\t\b\f \x01\x1f\x7f nul:") + '\0' + "end",               // B5 escapes, NUL, DEL
        "\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80", // range boundaries
        "",
    };
    return valid;
}

void utf8_identity() {
    const auto& valid = valid_utf8();
    for (const auto& text : valid) {
        const auto encoded = aravis::time_probe_json_string(text);
        if (!encoded) throw std::runtime_error("valid UTF-8 refused: " + encoded.error().message);
        const auto decoded = Json::parse(encoded.value());
        CHECK(decoded.is_string() && decoded.get<std::string>() == text); // exact semantic equality
        for (const char c : encoded.value()) CHECK(static_cast<unsigned char>(c) >= 0x20 && c != 0x7f); // no raw controls
    }
    std::cout << "  B2 decoded: " << Json::parse(aravis::time_probe_json_string(valid[1]).value()).get<std::string>()
              << " (exact); B3: " << valid[2] << "; B4: " << valid[3] << '\n';
    struct Bad {
        const char* name;
        std::string bytes;
        const char* reason;
    };
    const std::vector<Bad> invalid{
        {"stray continuation", "K\x80nig", "continuation byte without a lead byte"},
        {"malformed continuation", "C\xC3(mara", "invalid continuation byte"},
        {"malformed third byte", "\xE2\x80(", "invalid continuation byte"},
        {"malformed fourth byte", "\xF0\x9F\x93(", "invalid continuation byte"},
        {"truncated 2-byte", "C\xC3", "truncated sequence"},
        {"truncated 3-byte", "\xE2\x80", "truncated sequence"},
        {"truncated 4-byte", "x\xF0\x9F\x93", "truncated sequence"},
        {"overlong C0", "\xC0\xAF", "overlong encoding"},
        {"overlong C1", "\xC1\xBF", "overlong encoding"},
        {"overlong E0", "\xE0\x80\xAF", "overlong encoding"},
        {"overlong F0", "\xF0\x80\x80\xAF", "overlong encoding"},
        {"surrogate", "\xED\xA0\x80", "UTF-16 surrogate code point"},
        {"above U+10FFFF", "\xF4\x90\x80\x80", "code point above U+10FFFF"},
        {"F5 lead", "\xF5\x80\x80\x80", "never occurs in UTF-8"},
        {"FF byte", "\xFF", "never occurs in UTF-8"},
    };
    for (const auto& bad : invalid) {
        const auto encoded = aravis::time_probe_json_string(bad.bytes);
        if (encoded) throw std::runtime_error(std::string("malformed UTF-8 accepted: ") + bad.name);
        CHECK(contains(encoded.error().message, "not well-formed UTF-8") && contains(encoded.error().message, bad.reason));
    }
    // Through the full production report: identity and device ID round-trip
    // exactly; a malformed identity value names its field and yields no line.
    aravis::TimingContext context;
    context.native_interface = aravis::NativeInterface::gige_vision;
    context.native_device_type = "ArvGvDevice";
    context.tick_frequency_hz = 125000000ull;
    for (const auto& text : valid) {
        context.identity = {{"DeviceVendorName", text}, {"DeviceModelName", valid[2]}};
        const auto records = render_synthetic(synthetic_report(context, valid[1]));
        CHECK(records.front().at("identity").at("DeviceVendorName").get<std::string>() == text);
        CHECK(records.front().at("identity").at("DeviceModelName").get<std::string>() == valid[2]);
        CHECK(records.front().at("device_id").get<std::string>() == valid[1] && records[1].at("device_id") == valid[1]);
    }
    context.identity = {{"DeviceVendorName", "ok"}, {"DeviceModelName", "C\xC3(mara"}};
    const auto refused = aravis::render_time_probe_report(synthetic_report(context), 10000ms, 2);
    CHECK(!refused && contains(refused.error().message, "'header.identity.DeviceModelName'"));
    CHECK(contains(refused.error().message, "never substituted"));
    // A malformed DEVICE_ID argument is refused before output or device I/O.
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    const auto path = temp_path("gige-mel-time-probe-utf8-id");
    std::remove(path.c_str());
    const auto o = aravis::time_probe(*camera, "Fake_\xC3", 1, path);
    CHECK(!o.ok() && o.error && o.error->code == backend::ErrorCode::invalid_argument && !exists(path));
    CHECK(seen.opens.load() == 0);
}

// P3. A REAL SDK identity value arranged through the private Fake/GenICam
// setup: DeviceModelName is a StringReg in the Fake device memory (pinned
// src/arv-fake-camera.xml:46-52: 0x68, 32 bytes; the register cache is
// disabled by default, src/arvgc.c:493), so the production
// read_timing_context() reads the arranged bytes through the SDK. Valid
// multibyte UTF-8 reaches the JSONL file exactly; malformed bytes fail the
// probe naming the field, after native cleanup, with the output removed.
void utf8_identity_through_sdk() {
    const std::string model = "C\xC3\xA1mara \xE2\x80\x93 K\xC3\xB6nig \xF0\x9F\x93\xB7"; // 23 bytes < 32
    const std::string broken = "K\xC3(nig";
    for (const bool valid : {true, false}) {
        const std::string& arranged = valid ? model : broken;
        Observer seen;
        aravis::TestHooks hooks = seen.hooks();
        hooks.after_native_open = [&](ArvDevice* d) {
            seen.opens.fetch_add(1);
            char bytes[32] = {};
            std::copy(arranged.begin(), arranged.end(), bytes);
            CHECK(arv_fake_camera_write_memory(arv_fake_device_get_fake_camera(ARV_FAKE_DEVICE(d)), 0x68, sizeof bytes, bytes));
        };
        auto camera = aravis_fake::make_backend(hooks);
        const auto path = temp_path(valid ? "gige-mel-time-probe-utf8-sdk" : "gige-mel-time-probe-bad-utf8-sdk");
        std::remove(path.c_str());
        const auto outcome = aravis::time_probe(*camera, aravis_fake::device_id, 2, path);
        CHECK(seen.opens.load() == 1 && seen.live_streams() == 0 && seen.live_buffers() == 0);
        CHECK((seen.command_log() == std::vector<std::string>{"AcquisitionStart", "AcquisitionStop"}));
        if (valid) {
            if (!outcome.ok())
                throw std::runtime_error("UTF-8 probe failed: " + (outcome.error ? outcome.error->message : std::string()));
            // The SDK identity and the parsed JSON string compare equal.
            const auto& identity = outcome.report->context.identity;
            const auto found = std::find_if(identity.begin(), identity.end(),
                                            [](const auto& p) { return p.first == "DeviceModelName"; });
            CHECK(found != identity.end() && found->second == model);
            const auto records = parse_jsonl(read_file(path));
            CHECK(records.size() == 4 && records.front().at("identity").at("DeviceModelName").get<std::string>() == model);
            std::cout << "  SDK DeviceModelName round-trip: " << records.front().at("identity").at("DeviceModelName").get<std::string>()
                      << '\n';
            std::remove(path.c_str());
        } else {
            CHECK(!outcome.ok() && !outcome.report && outcome.error && !outcome.cleanup_error);
            CHECK(contains(outcome.error->message, "'header.identity.DeviceModelName'"));
            CHECK(contains(outcome.error->message, "not well-formed UTF-8"));
            CHECK(contains(outcome.error->message, "output was removed"));
            CHECK(!exists(path)); // no mojibake, no replacement identity, no file
            std::cout << "  SDK malformed DeviceModelName: " << outcome.error->message.substr(0, 96) << "...\n";
        }
    }
}

// Private backend fixture (NOT the Aravis adapter, no native SDK object): its
// open() either throws a controlled std::runtime_error or returns an Error.
class ScriptedBackend final : public backend::ICameraBackend {
public:
    explicit ScriptedBackend(bool throws) : throws_(throws) {}
    std::string_view id() const noexcept override { return "scripted-test-backend"; }
    backend::Result<std::vector<backend::DeviceInfo>> enumerate() override { return std::vector<backend::DeviceInfo>{}; }
    backend::Result<std::shared_ptr<backend::ICameraDevice>> open(std::string_view) override {
        opens.fetch_add(1);
        if (throws_) throw std::runtime_error("injected backend.open() failure");
        return backend::Error{backend::ErrorCode::busy, "injected open error"};
    }
    std::atomic<int> opens{0};

private:
    bool throws_;
};

// Logically closed: the exact-ID reservation was released, so a fresh
// adapter can open the Fake again.
void require_reopenable() {
    Observer again;
    auto reopen = aravis_fake::make_backend(again.hooks());
    auto device = reopen->open(aravis_fake::device_id);
    CHECK(device && device.value()->close());
}

// P4. Exclusive output ownership and exception-safe cleanup (C1-C5).
void output_cleanup() {
    const auto path = temp_path("gige-mel-time-probe-cleanup");
    std::remove(path.c_str());
    // C1. Existing output: refused before open; original bytes unchanged.
    {
        const std::string original = std::string("existing report \xE2\x80\x94 keep\n\x01\x02") + '\0' + "tail";
        { std::ofstream(path, std::ios::binary) << original; }
        ScriptedBackend thrower(true);
        const auto o = aravis::time_probe(thrower, "GV-1", 2, path);
        CHECK(!o.ok() && o.error->code == backend::ErrorCode::io_error && contains(o.error->message, "no device I/O"));
        CHECK(thrower.opens.load() == 0 && read_file(path) == original);
        std::remove(path.c_str());
    }
    // C2. Argument errors: no open and no output (O2 covers the full set).
    {
        ScriptedBackend thrower(true);
        CHECK(aravis::time_probe(thrower, "GV-1", 0, path).error->code == backend::ErrorCode::invalid_argument);
        CHECK(aravis::time_probe(thrower, "", 2, path).error->code == backend::ErrorCode::invalid_argument);
        CHECK(aravis::time_probe(thrower, "GV-1", 2, path, 0ms).error->code == backend::ErrorCode::invalid_argument);
        CHECK(thrower.opens.load() == 0 && !exists(path));
    }
    // C3. open() returns an Error: that error, output removed.
    {
        ScriptedBackend refusing(false);
        const auto o = aravis::time_probe(refusing, "GV-1", 2, path);
        CHECK(!o.ok() && o.error->code == backend::ErrorCode::busy && o.error->message == "injected open error");
        CHECK(refusing.opens.load() == 1 && !exists(path) && !o.cleanup_error);
    }
    // C4. open() throws: reported, bounded, output removed, no device cleanup invented.
    {
        ScriptedBackend thrower(true);
        const auto begin = std::chrono::steady_clock::now();
        const auto o = aravis::time_probe(thrower, "GV-1", 2, path);
        CHECK(std::chrono::steady_clock::now() - begin < 2s);
        CHECK(!o.ok() && !o.report && o.error && o.error->code == backend::ErrorCode::io_error);
        CHECK(contains(o.error->message, "injected backend.open() failure") && contains(o.error->message, "output was removed"));
        CHECK(!contains(o.error->message, "device was stopped and closed") && !o.cleanup_error);
        CHECK(thrower.opens.load() == 1 && !exists(path));
    }
    // C4 inside the real adapter's open() (TestHooks::after_reservation):
    // no native open, stream or command; the reservation is released.
    {
        Observer seen;
        aravis::TestHooks hooks = seen.hooks();
        hooks.after_reservation = [](std::string_view) { throw std::runtime_error("injected inside Aravis open"); };
        auto camera = aravis_fake::make_backend(hooks);
        const auto o = aravis::time_probe(*camera, aravis_fake::device_id, 2, path);
        CHECK(!o.ok() && contains(o.error->message, "injected inside Aravis open") && !exists(path));
        CHECK(seen.opens.load() == 0 && seen.streams_created.load() == 0 && seen.command_log().empty());
        require_reopenable();
    }
    // C5. A deterministic throw at each production boundary of the REAL probe
    // against the Fake: the stream and device are retired (before rendering,
    // or during unwinding), the output is removed, the error is reported.
    struct Stage {
        const char* name;
        std::vector<std::string> commands;
        bool unwound_native; // native cleanup happened in the exception path
    };
    const std::vector<Stage> stages{
        {"opened", {}, true},
        {"started", {"AcquisitionStart", "AcquisitionStop"}, true},
        {"render", {"AcquisitionStart", "AcquisitionStop"}, false},  // already finished before serialization
        {"written", {"AcquisitionStart", "AcquisitionStop"}, false}, // a partial file existed at the throw
    };
    for (const auto& stage : stages) {
        Observer seen;
        auto camera = aravis_fake::make_backend(seen.hooks());
        bool reached = false, file_at_throw = false;
        int live_at_throw = -1;
        aravis::TimeProbeTestFaults faults;
        faults.checkpoint = [&](std::string_view at) {
            if (at != stage.name) return;
            reached = true;
            file_at_throw = exists(path);
            live_at_throw = seen.live_streams();
            throw std::runtime_error(std::string("injected at ") + stage.name);
        };
        const auto o = aravis::time_probe_with_faults_for_testing(*camera, aravis_fake::device_id, 3, path, 10000ms, faults);
        CHECK(reached && file_at_throw); // the exclusive output existed when the exception started
        CHECK(!o.ok() && !o.report && o.error && o.error->code == backend::ErrorCode::io_error && !o.cleanup_error);
        CHECK(contains(o.error->message, std::string("injected at ") + stage.name));
        CHECK(contains(o.error->message, "device was stopped and closed") && contains(o.error->message, "output was removed"));
        CHECK(live_at_throw == (std::string_view(stage.name) == "started" ? 1 : 0));
        CHECK(!exists(path) && seen.live_streams() == 0 && seen.live_buffers() == 0);
        CHECK(seen.command_log() == stage.commands);
        require_reopenable();
        std::cout << "  C5 throw at '" << stage.name << "': output removed; native cleanup "
                  << (stage.unwound_native ? "during unwinding" : "before serialization") << "; commands "
                  << stage.commands.size() << '\n';
    }
}

// P5. C6: operational failure AFTER native stream creation and start, with
// and without a separately arranged cleanup failure. Every popped REAL
// buffer is marked incomplete through the documented decoded-facts seam (the
// SDK buffer is untouched), so the one receive deadline expires; in the
// second run the SDK itself also rejects AcquisitionStop (imposed RO access).
void operational_failure_cleanup() {
    for (const bool stop_fails : {false, true}) {
        Observer seen;
        ArvDomNode* stop_access = nullptr;
        aravis::TestHooks hooks = seen.hooks();
        hooks.after_native_open = [&](ArvDevice* d) {
            seen.opens.fetch_add(1);
            stop_access = impose_access(d, "AcquisitionStop", "RW");
        };
        hooks.decoded_facts = [](gige_mel::backend::aravis_profile::NativeFrameFacts& f) {
            f.status = gige_mel::backend::aravis_profile::NativeStatus::incomplete;
        };
        if (stop_fails)
            seen.on_command = [&](std::string_view c, ArvDevice*) {
                if (c == "AcquisitionStop") set_access(stop_access, "RO");
            };
        auto camera = aravis_fake::make_backend(hooks);
        const auto path = temp_path("gige-mel-time-probe-operational");
        std::remove(path.c_str());
        const auto begin = std::chrono::steady_clock::now();
        const auto o = aravis::time_probe(*camera, aravis_fake::device_id, 2, path, 300ms);
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        CHECK(elapsed >= 300ms && elapsed < 5s); // the ONE overall deadline bounds it
        CHECK(!o.ok() && !o.report && o.error && o.error->code == backend::ErrorCode::timeout);
        CHECK(contains(o.error->message, "(after 0 of 2 frames)")); // the first failure, preserved
        CHECK(seen.streams_created.load() == 1 && seen.live_streams() == 0 && seen.live_buffers() == 0);
        CHECK(!exists(path));
        const auto log = seen.command_log();
        CHECK(log.size() >= 2 && log[0] == "AcquisitionStart" && log[1] == "AcquisitionStop");
        if (stop_fails) {
            CHECK(o.cleanup_error && contains(o.cleanup_error->message, "AcquisitionStop"));
            CHECK(!contains(o.error->message, "AcquisitionStop")); // not replaced by the cleanup failure
        } else {
            CHECK(!o.cleanup_error && log.size() == 2);
        }
        require_reopenable();
        std::cout << "  C6 stop " << (stop_fails ? "rejected by the SDK" : "accepted") << ": error=" << o.error->message.substr(0, 48)
                  << "...; cleanup_error=" << (o.cleanup_error ? o.cleanup_error->message.substr(0, 60) + "..." : "none") << '\n';
    }
}

// ===========================================================================
// V. TEST-ONLY virtual integration-start clock: positive MEL publication.

// V0. The declared model and its checked arithmetic; overflow fails closed.
void virtual_clock_model() {
    const vc::Model m{std::chrono::seconds{1000}, 10ms, 7};
    const auto t = vc::nominal(m, 3);
    CHECK(t && t->valid && t->domain == backend::ClockDomain::device &&
          t->event == backend::TimestampEvent::integration_start && t->clock_generation == 7);
    CHECK(t->value == std::chrono::seconds{1000} + 3 * 10ms);
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    CHECK(vc::value_at({std::chrono::nanoseconds{max}, 1ns, 1}, 0) == std::chrono::nanoseconds{max});
    CHECK(!vc::value_at({std::chrono::nanoseconds{max}, 1ns, 1}, 1));          // epoch + offset overflows
    CHECK(!vc::value_at({0ns, std::chrono::nanoseconds{max / 2 + 1}, 1}, 2));  // n * period overflows
    CHECK(!vc::value_at({0ns, 1ns, 1}, std::numeric_limits<std::uint64_t>::max()));
    CHECK(!vc::value_at({-1ns, 1ns, 1}, 0) && !vc::value_at({0ns, 0ns, 1}, 0) && !vc::value_at({0ns, -1ns, 1}, 0));
    // The decorator turns an overflow into a receive error; it never delivers
    // a wrapped or unchecked value. The genuine SDK frame is still released.
    auto clock = std::make_shared<vc::Clock>();
    clock->set_model({std::chrono::nanoseconds{max}, 1ns, 1});
    auto ledger = std::make_shared<vc::Ledger>();
    Observer seen;
    vc::Backend decorated(aravis_fake::make_backend(seen.hooks()), clock, ledger);
    auto device = std::move(decorated.open(aravis_fake::device_id)).value();
    CHECK(device->apply(gen1).value() == gen1);
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(61));
    auto first = stream->receive(2000ms); // sequence 0: representable
    CHECK(first && first.value().descriptor().timestamp.value == std::chrono::nanoseconds{max});
    CHECK(unqualified(ledger->all().front().original.timestamp));
    const auto overflow = stream->receive(2000ms); // sequence 1: overflow
    CHECK(!overflow && overflow.error().code == backend::ErrorCode::io_error && contains(overflow.error().message, "overflow"));
    CHECK(ledger->outstanding() == 1); // only `first` is still held; the overflowed frame was released
    { auto released = std::move(first); }
    CHECK(ledger->outstanding() == 0);
    stream->stop();
    stream.reset();
    CHECK(device->close());
    CHECK(seen.live_streams() == 0 && seen.live_buffers() == 0);
}

// V1. Positive publication of GENUINE Aravis SDK pixels under the virtual
// clock: real MEL Image callbacks, registered buffers, full pixel and header
// checks. Plus the before/after control within the same pinned DSO.
void positive_publication() {
    Session s(true);
    const vc::Model model{std::chrono::seconds{1000}, 10ms, 7}; // TEST MODEL values
    s.clock->set_model(model);
    s.configure(9001, gen1);
    const std::size_t bytes = std::size_t{gen1.width} * gen1.height;
    for (int i = 0; i < 4; ++i) s.add_buffer(bytes, 101 + i);
    // Destination bytes are untouched until a valid frame is admitted.
    for (const auto& block : s.storage) CHECK(all_sentinel(block));
    CHECK(s.ledger->frames() == 0 && s.listener->count() == 0);
    s.listener->auto_release = true;
    CHECK(s.image->enable() == ir::Return::Success);
    s.listener->wait_count(12);
    CHECK(s.image->disable() == ir::Return::Success);
    const auto deliveries = s.listener->all();
    std::set<const void*> addresses;
    std::uint64_t previous_sequence = 0;
    std::chrono::nanoseconds previous_time{-1};
    for (std::size_t i = 0; i < deliveries.size(); ++i) {
        const auto& d = deliveries[i];
        check_published(d, model, gen1, 1);
        addresses.insert(d.address);
        if (i) CHECK(d.source->sequence > previous_sequence);
        CHECK(d.header.getSystemTime() > previous_time);
        previous_sequence = d.source->sequence;
        previous_time = d.header.getSystemTime();
    }
    CHECK(addresses.size() <= 4); // only registered allocations; no additional destination
    const auto stats = s.stats();
    CHECK(stats.delivered_frames == deliveries.size() && stats.rejected_frames == 0);
    CHECK(stats.stream_failures == 0 && stats.callback_failures == 0);
    // Every backend delivery is accounted for (one may be discarded at stop).
    const auto frames = s.ledger->frames();
    const auto counted = stats.delivered_frames + stats.dropped_no_buffer + stats.rejected_frames;
    CHECK(counted <= frames && frames - counted <= 1);
    CHECK(s.ledger->wait_outstanding(0, bound) && s.ledger->peak_outstanding() == 1);
    CHECK(eventually([&] { return s.seen->live_streams() == 0; }));
    std::cout << "  published " << deliveries.size() << " Aravis Fake frames (IDs " << deliveries.front().header.getFrameID()
              << ".." << deliveries.back().header.getFrameID() << ") with virtual systemTime "
              << deliveries.front().header.getSystemTime().count() << ".." << deliveries.back().header.getSystemTime().count()
              << " ns; backend frames " << frames << ", dropped " << stats.dropped_no_buffer << '\n';
}

// V2. Backpressure with every registered destination held by the consumer.
void backpressure_and_release() {
    Session s(true);
    const vc::Model model{std::chrono::seconds{2000}, 10ms, 11};
    s.clock->set_model(model);
    s.configure(9101, gen1);
    const std::size_t bytes = std::size_t{gen1.width} * gen1.height;
    for (int i = 0; i < 4; ++i) s.add_buffer(bytes, 201 + i);
    CHECK(s.image->enable() == ir::Return::Success);
    s.listener->wait_count(4); // the consumer holds all four (no release)
    auto held = s.listener->all();
    CHECK(held.size() == 4);
    std::set<const void*> addresses;
    for (const auto& d : held) {
        check_published(d, model, gen1, 1);
        addresses.insert(d.address);
    }
    CHECK(addresses.size() == 4); // four distinct registered allocations, nothing else
    // Further genuine frames arrive and are dropped: no buffer, no callback,
    // no extra destination; the held bytes are never overwritten.
    const auto base = s.stats();
    const auto frames = s.ledger->frames();
    CHECK(s.ledger->wait_frames(frames + 6, bound));
    CHECK(eventually([&] { return s.stats().dropped_no_buffer >= base.dropped_no_buffer + 5; }));
    CHECK(s.listener->count() == 4 && s.stats().delivered_frames == 4 && s.stats().rejected_frames == 0);
    for (const auto& d : held) CHECK(region(d.address, bytes) == d.bytes);
    // Source SDK leases are released independently of the held MEL
    // destinations: at most the frame currently being processed is held.
    CHECK(s.ledger->wait_outstanding(1, bound));
    CHECK(s.ledger->peak_outstanding() == 1);
    // Dropping shared_ptr copies is NOT Buffer::release(): still no reuse.
    const void* first_address = held.front().address;
    const auto first_context = held.front().context;
    s.listener->drop_buffer_refs();
    for (auto& d : held) d.buffer.reset();
    const auto frames2 = s.ledger->frames();
    CHECK(s.ledger->wait_frames(frames2 + 5, bound));
    CHECK(s.listener->count() == 4);
    for (const auto& d : held) CHECK(region(d.address, bytes) == d.bytes);
    // Explicit release of one slot: publication resumes into exactly that
    // slot; the other three still-held slots keep their bytes.
    auto slot = registered_at(s, first_address);
    CHECK(slot->release() == ir::Return::Success);
    CHECK(slot->release() == ir::Return::Fail); // a second release is not a new release
    s.listener->wait_count(5);
    const auto resumed = s.listener->at(4);
    check_published(resumed, model, gen1, 1);
    CHECK(resumed.address == first_address && resumed.context == first_context);
    CHECK(resumed.source->sequence > held.back().source->sequence);
    for (std::size_t i = 1; i < held.size(); ++i) CHECK(region(held[i].address, bytes) == held[i].bytes);
    // That slot is held again: no sixth callback, no additional allocation.
    const auto frames3 = s.ledger->frames();
    CHECK(s.ledger->wait_frames(frames3 + 4, bound));
    CHECK(s.listener->count() == 5 && s.stats().delivered_frames == 5);
    CHECK(s.image->disable() == ir::Return::Success);
    for (std::size_t i = 1; i < held.size(); ++i) CHECK(region(held[i].address, bytes) == held[i].bytes);
    CHECK(region(first_address, bytes) == resumed.bytes);
    std::cout << "  backpressure: 4 held, dropped " << s.stats().dropped_no_buffer << " genuine frames, resumed into context "
              << resumed.context << " after explicit release\n";
}

// V3. Explicit disable drains; no late/stale callback; reconfigure while
// disabled with actual readback; a new generation publishes its own geometry.
void disable_and_restart() {
    Session s(true);
    const vc::Model model1{std::chrono::seconds{3000}, 10ms, 21};
    s.clock->set_model(model1);
    s.configure(9201, gen1);
    const std::size_t bytes1 = std::size_t{gen1.width} * gen1.height;
    s.add_buffer(bytes1, 301);
    s.add_buffer(bytes1, 302);
    CHECK(s.image->enable() == ir::Return::Success);
    s.listener->wait_count(2); // both held
    CHECK(s.image->disable() == ir::Return::Success); // drains publication
    s.listener->closed_run = 1;
    const auto first_run = s.listener->all();
    CHECK(first_run.size() == 2 && s.stats().delivered_frames == 2);
    for (const auto& d : first_run) check_published(d, model1, gen1, 1);
    // No late callback: acquisition stopped and the backend delivers nothing.
    CHECK(eventually([&] { return s.seen->live_streams() == 0; }));
    CHECK(acquisition_register(*s.inner()) == 0);
    const auto frames_after = s.ledger->frames();
    std::this_thread::sleep_for(150ms);
    CHECK(s.ledger->frames() == frames_after && s.listener->count() == 2);
    // Reconfigure while disabled; actual SDK readback; held bytes unchanged.
    s.configure(9202, gen2);
    for (const auto& d : first_run) CHECK(region(d.address, bytes1) == d.bytes);
    // Existing contract: no restart while destinations are still held.
    CHECK(s.image->enable() == ir::Return::Fail);
    CHECK(s.ledger->runs() == 1 && s.listener->count() == 2);
    // Explicit release, then retire the undersized generation-1 buffers.
    for (const auto& d : first_run) {
        auto buffer = registered_at(s, d.address);
        CHECK(buffer->release() == ir::Return::Success);
        CHECK(s.image->unregisterBuffer(buffer) == ir::Return::Success);
    }
    const std::size_t bytes2 = std::size_t{gen2.width} * gen2.height;
    s.add_buffer(bytes2, 311);
    s.add_buffer(bytes2, 312);
    const vc::Model model2{std::chrono::seconds{4000}, 20ms, 22}; // a new fixed generation for run 2
    s.clock->set_model(model2);
    s.listener->auto_release = true;
    CHECK(s.image->enable() == ir::Return::Success);
    s.listener->wait_count(2 + 5);
    CHECK(s.image->disable() == ir::Return::Success);
    const auto all = s.listener->all();
    std::set<std::uint64_t> run1_ids;
    for (const auto& d : first_run) run1_ids.insert(d.source->original.native_frame_id);
    for (std::size_t i = 2; i < all.size(); ++i) {
        const auto& d = all[i];
        check_published(d, model2, gen2, 2); // geometry, ROI, exposure and virtual time of generation 2
        CHECK(d.source->original.stream_generation != first_run.front().source->original.stream_generation);
        CHECK(d.source->original.configuration_generation > first_run.front().source->original.configuration_generation);
        CHECK(!run1_ids.count(d.source->original.native_frame_id));
    }
    // The generation-1 destinations were never written again.
    for (const auto& d : first_run) CHECK(region(d.address, bytes1) == d.bytes);
    for (const auto& d : all) CHECK(!d.stale);
    std::cout << "  restart: stream generation " << first_run.front().source->original.stream_generation << " -> "
              << all.back().source->original.stream_generation << ", " << all.size() - 2 << " frames at " << gen2.width
              << 'x' << gen2.height << "@(" << gen2.offset_x << ',' << gen2.offset_y << "), " << gen2.exposure.count()
              << " ns integration\n";
}

// ===========================================================================
// N. Fail-closed timestamp admission with the same genuine SDK frames.

using Make = std::function<backend::FrameTimestamp(const backend::FrameTimestamp& nominal,
                                                   const backend::FrameTimestamp& original)>;
struct Variant {
    const char* name;
    Make make;
};
std::vector<Variant> rejected_variants() {
    return {
        {"production default FrameTimestamp{} (the adapter's own value)",
         [](const backend::FrameTimestamp&, const backend::FrameTimestamp& original) { return original; }},
        {"valid == false", [](auto n, const auto&) { n.valid = false; return n; }},
        {"ClockDomain mismatch (host_monotonic)", [](auto n, const auto&) { n.domain = backend::ClockDomain::host_monotonic; return n; }},
        {"ClockDomain mismatch (utc)", [](auto n, const auto&) { n.domain = backend::ClockDomain::utc; return n; }},
        {"TimestampEvent::reception", [](auto n, const auto&) { n.event = backend::TimestampEvent::reception; return n; }},
        {"TimestampEvent::unspecified", [](auto n, const auto&) { n.event = backend::TimestampEvent::unspecified; return n; }},
    };
}

// Enables, waits for >= `frames` genuine backend deliveries that the provider
// rejected, disables, and proves: no callback, no destination write, no
// stream/callback failure (an ordinary per-frame rejection, not terminal).
void expect_all_rejected(Session& s, const char* label, std::size_t frames) {
    const auto before = s.stats();
    const auto ledger_before = s.ledger->frames();
    const auto callbacks = s.listener->count();
    CHECK(s.image->enable() == ir::Return::Success);
    CHECK(s.ledger->wait_frames(ledger_before + frames, bound));
    CHECK(eventually([&] { return s.stats().rejected_frames >= before.rejected_frames + frames; }));
    CHECK(s.image->enable() == ir::Return::Success); // still enabled and NOT failed
    CHECK(s.image->disable() == ir::Return::Success);
    const auto after = s.stats();
    CHECK(s.listener->count() == callbacks);
    CHECK(after.delivered_frames == before.delivered_frames && after.dropped_no_buffer == before.dropped_no_buffer);
    CHECK(after.stream_failures == before.stream_failures && after.callback_failures == before.callback_failures);
    CHECK(contains(after.last_error, "timestamp contract"));
    for (const auto& block : s.storage) CHECK(all_sentinel(block));
    for (const auto& f : s.ledger->all()) CHECK(unqualified(f.original.timestamp)); // the adapter never qualifies
    std::cout << "  rejected [" << label << "]: " << after.rejected_frames - before.rejected_frames
              << " genuine frames, 0 callbacks, destinations unchanged\n";
}

// N1. Every unqualified timestamp variant is rejected per frame.
void rejected_timestamps() {
    Session s(true);
    const vc::Model model{std::chrono::seconds{6000}, 10ms, 41};
    s.clock->set_model(model);
    s.configure(9301, gen1);
    const std::size_t bytes = std::size_t{gen1.width} * gen1.height;
    s.add_buffer(bytes, 401);
    s.add_buffer(bytes, 402);
    for (const auto& v : rejected_variants()) {
        s.clock->set_plan([make = v.make](std::uint64_t, const auto& n, const auto& o) { return make(n, o); });
        expect_all_rejected(s, v.name, 4);
    }
    CHECK(s.listener->count() == 0 && s.stats().delivered_frames == 0);
}

// N2. A rejected initial frame does not establish the accepted stream's
// clock baseline: the rejected frames carry clock_generation 99 and a value
// one hour AHEAD; had either been adopted, the following declared frames
// (generation 41, lower value) would be a terminal discontinuity.
void rejected_initial_then_accepted() {
    Session s(true);
    const vc::Model model{std::chrono::seconds{7000}, 10ms, 41};
    s.clock->set_model(model);
    s.configure(9401, gen1);
    const std::size_t bytes = std::size_t{gen1.width} * gen1.height;
    s.add_buffer(bytes, 501);
    s.add_buffer(bytes, 502);
    s.listener->auto_release = true;
    std::uint64_t run = 0;
    for (const auto& v : rejected_variants()) {
        // The production default stays exactly FrameTimestamp{} (generation 0,
        // value 0); every other variant is also moved to generation 99, one hour
        // ahead of the declared frames that follow it.
        const bool production_default = std::string(v.name).find("default") != std::string::npos;
        s.clock->set_plan([make = v.make, production_default](std::uint64_t seq, const auto& n, const auto& o) {
            if (seq >= 2) return n;
            auto bad = make(n, o);
            if (!production_default) {
                bad.clock_generation = 99;
                bad.value = n.value + std::chrono::hours{1};
            }
            return bad;
        });
        const auto before = s.stats();
        const auto callbacks = s.listener->count();
        CHECK(s.image->enable() == ir::Return::Success);
        ++run;
        s.listener->wait_count(callbacks + 3);
        CHECK(s.image->disable() == ir::Return::Success);
        s.listener->closed_run = run;
        const auto after = s.stats();
        CHECK(after.stream_failures == before.stream_failures); // NOT a discontinuity
        CHECK(after.rejected_frames >= before.rejected_frames + 2);
        const auto all = s.listener->all();
        for (std::size_t i = callbacks; i < all.size(); ++i) {
            CHECK(all[i].source && all[i].source->run == run && all[i].source->sequence >= 2);
            check_published(all[i], model, gen1, run);
        }
        CHECK(all[callbacks].source->sequence == 2); // the first ACCEPTED frame is the first published
    }
    std::cout << "  rejected-first frames did not establish the clock baseline (" << run << " variants)\n";
}

// Bytes of every registered destination equal its last published image.
void destinations_hold_last_published(const Session& s) {
    const auto all = s.listener->all();
    for (const auto& block : s.storage) {
        const Delivery* last = nullptr;
        for (const auto& d : all)
            if (d.address == block.data()) last = &d;
        if (last) CHECK(std::equal(last->bytes.begin(), last->bytes.end(), block.begin()));
        else CHECK(all_sentinel(block));
    }
}

// N3. Within one accepted stream, a backward timestamp or a clock_generation
// change is a TERMINAL discontinuity: the frame is not written or
// published, the stream fails, nothing is published afterwards, and only an
// explicit disable + enable recovers (with a fresh clock baseline).
void discontinuity_and_recovery() {
    struct Case {
        const char* name;
        std::function<backend::FrameTimestamp(const backend::FrameTimestamp&, const vc::Model&)> at3;
    };
    const std::vector<Case> cases{
        {"backward value", [](const auto& n, const vc::Model& m) {
             auto t = n;
             t.value = vc::value_at(m, 1).value(); // earlier than the accepted sequence 2
             return t;
         }},
        {"clock_generation change", [](const auto& n, const vc::Model&) {
             auto t = n;
             t.clock_generation = n.clock_generation + 1;
             return t;
         }},
    };
    for (const auto& c : cases) {
        Session s(true);
        const vc::Model model{std::chrono::seconds{8000}, 10ms, 51};
        s.clock->set_model(model);
        s.configure(9501, gen1);
        const std::size_t bytes = std::size_t{gen1.width} * gen1.height;
        s.add_buffer(bytes, 601);
        s.add_buffer(bytes, 602);
        s.listener->auto_release = true; // never starved: every accepted frame is published
        // Sequence 3 only; later frames would be declared again, yet must
        // never be received: the failed stream is stopped.
        s.clock->set_plan([&c, model](std::uint64_t seq, const auto& n, const auto&) { return seq == 3 ? c.at3(n, model) : n; });
        CHECK(s.image->enable() == ir::Return::Success);
        // The provider increments stream_failures BEFORE it records the
        // message (src/provider.cpp run()), so wait for both facts together.
        CHECK(eventually([&] {
            const auto st = s.stats();
            return st.stream_failures == 1 && contains(st.last_error, "timestamp epoch changed or clock regressed");
        }));
        const auto failed = s.stats();
        CHECK(failed.stream_failures == 1 && contains(failed.last_error, "timestamp epoch changed or clock regressed"));
        CHECK(failed.rejected_frames == 0 && failed.callback_failures == 0);
        CHECK(s.listener->count() == 3 && failed.delivered_frames == 3);
        const auto published = s.listener->all();
        for (std::size_t i = 0; i < 3; ++i) {
            check_published(published[i], model, gen1, 1);
            CHECK(published[i].source->sequence == i);
        }
        destinations_hold_last_published(s); // the sequence-3 frame wrote nothing
        // Terminal: reported as a failed enable while still enabled; the
        // backend stream is stopped; no later frame or callback.
        CHECK(s.image->enable() == ir::Return::Fail);
        CHECK(eventually([&] { return s.seen->live_streams() == 0; }));
        const auto frames = s.ledger->frames();
        CHECK(frames == 4);
        std::this_thread::sleep_for(200ms);
        CHECK(s.ledger->frames() == frames && s.listener->count() == 3);
        CHECK(s.stats().stream_failures == 1 && s.image->enable() == ir::Return::Fail); // no automatic recovery
        // Explicit recovery: disable, resolve the (virtual) time mapping, enable.
        CHECK(s.image->disable() == ir::Return::Success);
        s.listener->closed_run = 1;
        const vc::Model resolved{std::chrono::seconds{8000}, 10ms, 52};
        s.clock->set_model(resolved);
        s.clock->set_plan({});
        CHECK(s.image->enable() == ir::Return::Success);
        s.listener->wait_count(3 + 3);
        CHECK(s.image->disable() == ir::Return::Success);
        const auto after = s.listener->all();
        for (std::size_t i = 3; i < after.size(); ++i) check_published(after[i], resolved, gen1, 2); // fresh run
        CHECK(after[3].source->sequence == 0); // fresh baseline
        CHECK(s.stats().stream_failures == 1); // recovery added no failure
        std::cout << "  terminal [" << c.name << "]: 3 published, the 4th not written, stream stopped; explicit "
                  << "recovery published " << after.size() - 3 << '\n';
    }
}

// N4. The four outcomes stay distinguishable: ordinary per-frame rejection
// (N1/N2, non-terminal), terminal clock discontinuity (N3), deliberate
// cancellation and native acquisition failure (here).
void failure_classification() {
    Session s(true);
    const vc::Model model{std::chrono::seconds{9000}, 10ms, 61};
    s.clock->set_model(model);
    s.configure(9601, gen1);
    const std::size_t bytes = std::size_t{gen1.width} * gen1.height;
    s.add_buffer(bytes, 701);
    s.add_buffer(bytes, 702);
    s.listener->auto_release = true;
    // Native acquisition failure (SDK-arranged): after the adapter validated
    // AcquisitionStart as writable, the SDK itself is made to reject it.
    ArvDomNode* start_access = nullptr;
    CHECK(aravis::with_native_device_for_testing(*s.inner(), [&](ArvDevice* d) {
        start_access = impose_access(d, "AcquisitionStart", "RW");
    }));
    std::atomic<bool> fail_start{true};
    s.seen->on_command = [&](std::string_view c, ArvDevice*) {
        if (c == "AcquisitionStart" && fail_start.exchange(false)) set_access(start_access, "RO");
    };
    CHECK(s.image->enable() == ir::Return::Fail);
    auto st = s.stats();
    CHECK(contains(st.last_error, "AcquisitionStart") && contains(st.last_error, "read only"));
    CHECK(st.stream_failures == 0 && st.rejected_frames == 0 && st.delivered_frames == 0); // not a stream failure
    CHECK(s.ledger->runs() == 0 && s.ledger->frames() == 0 && s.listener->count() == 0);
    CHECK(s.seen->live_streams() == 0 && acquisition_register(*s.inner()) == 0);
    for (const auto& block : s.storage) CHECK(all_sentinel(block));
    // Explicit recovery of the native condition.
    CHECK(aravis::with_native_device_for_testing(*s.inner(), [&](ArvDevice*) { set_access(start_access, "RW"); }));
    // Deliberate cancellation: disable during publication is not a failure.
    CHECK(s.image->enable() == ir::Return::Success);
    s.listener->wait_count(3);
    CHECK(s.image->disable() == ir::Return::Success);
    st = s.stats();
    CHECK(st.stream_failures == 0 && st.callback_failures == 0 && st.rejected_frames == 0);
    for (const auto& d : s.listener->all()) check_published(d, model, gen1, 1);
    std::cout << "  classified: native start failure (enable Fail, no stream failure), deliberate cancellation "
              << "(no failure); per-frame rejection and terminal discontinuity covered separately\n";
}

// C. Control within the same pinned DSO: the SAME decorator without a clock
// (pure pass-through) restores rejection of the same genuine SDK frames.
void control_without_virtual_clock() {
    Session s(false);
    s.configure(9701, gen1);
    const std::size_t bytes = std::size_t{gen1.width} * gen1.height;
    s.add_buffer(bytes, 801);
    s.add_buffer(bytes, 802);
    expect_all_rejected(s, "no virtual clock: production adapter timestamps", 6);
    for (const auto& f : s.ledger->all()) {
        CHECK(!f.substituted && same_timestamp(f.original.timestamp, f.delivered.timestamp));
        CHECK(f.original.configuration == gen1);
    }
    CHECK(s.listener->count() == 0);
}

void fatal_glib_log(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer) {
    std::cerr << "FAIL GLib " << ((level & G_LOG_LEVEL_CRITICAL) ? "critical" : "warning") << " ["
              << (domain ? domain : "") << "]: " << (message ? message : "") << '\n';
    std::abort();
}

// Bounded watchdog per scenario: a hang is a failure, never a pass.
class Watchdog {
public:
    Watchdog(const char* name, std::chrono::seconds limit)
        : thread_([this, name, limit] {
              std::unique_lock lock(mutex_);
              if (!done_cv_.wait_for(lock, limit, [&] { return done_; })) {
                  std::cerr << "FAIL watchdog: scenario '" << name << "' exceeded " << limit.count() << " s\n";
                  std::_Exit(3);
              }
          }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex_); done_ = true; }
        done_cv_.notify_all();
        thread_.join();
    }
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    std::mutex mutex_;
    std::condition_variable done_cv_;
    bool done_{false};
    std::thread thread_; // last: started after the state above exists
};

} // namespace

int main(int argc, char** argv) {
    std::cout << "Aravis runtime " << aravis::runtime_version() << " (compiled " << aravis::compiled_version()
              << ") loaded from " << aravis_fake::loaded_library_path()
              << "; Fake device only; TEST-ONLY virtual clock; virtual optics\n";
    if (aravis::runtime_version() != "0.8.36" || aravis::compiled_version() != "0.8.36") {
        std::cerr << "FAIL: the qualified Aravis 0.8.36 library is not the one loaded\n";
        return 1;
    }
    g_log_set_always_fatal(static_cast<GLogLevelFlags>(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));
    for (const char* domain : {static_cast<const char*>(nullptr), "Aravis", "GLib", "GLib-GObject"})
        g_log_set_handler(domain, static_cast<GLogLevelFlags>(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL),
                          fatal_glib_log, nullptr);
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"O1 native SDK timestamp observation (read-only, unqualified)", native_observation},
        {"O2 time-probe argument validation before device I/O", time_probe_arguments},
        {"O3 time-probe against the private Fake interface (JSONL)", time_probe_fake},
        {"P1 R1 rendered timing provenance (A1-A4, production formatter)", provenance_table},
        {"P2 R1 exact UTF-8 and malformed-input policy (B1-B6, production encoder)", utf8_identity},
        {"P3 R1 SDK identity UTF-8 through the Fake GenICam (time_probe)", utf8_identity_through_sdk},
        {"P4 R1 exclusive output and exception cleanup (C1-C5)", output_cleanup},
        {"P5 R1 operational failure after stream start; separate cleanup error (C6)", operational_failure_cleanup},
        {"V0 virtual clock model and checked arithmetic", virtual_clock_model},
        {"V1 positive MEL Image publication of genuine SDK pixels (virtual time)", positive_publication},
        {"V2 backpressure, held buffers and explicit release", backpressure_and_release},
        {"V3 disable, reconfigure and restart generation", disable_and_restart},
        {"N1 unqualified timestamps rejected per frame", rejected_timestamps},
        {"N2 rejected initial frame does not establish the clock baseline", rejected_initial_then_accepted},
        {"N3 terminal clock discontinuity and explicit recovery", discontinuity_and_recovery},
        {"N4 native acquisition failure versus deliberate cancellation", failure_classification},
        {"C  control: no virtual clock restores rejection", control_without_virtual_clock},
    };
    // Usage: aravis_timestamp_contract_tests [FILTER [REPEAT]]
    const std::string filter = argc >= 2 ? argv[1] : "";
    const int repeat = argc >= 3 ? std::max(1, std::atoi(argv[2])) : 1;
    std::size_t passed = 0;
    for (int r = 0; r < repeat; ++r)
        for (const auto& [name, run] : tests) {
            if (!filter.empty() && std::string(name).find(filter) == std::string::npos) continue;
            try {
                Watchdog watchdog(name, std::chrono::seconds{120});
                run();
            } catch (const std::exception& error) {
                std::cerr << "FAIL " << name << ": " << error.what() << '\n';
                return 1;
            }
            ++passed;
            std::cout << "PASS " << name << std::endl;
        }
    std::cout << passed << " Aravis timestamp contract scenarios passed (virtual-clock MEL publication is "
              << "test-qualified only; no hardware timestamp qualification)\n";
    return passed ? 0 : 1;
}
