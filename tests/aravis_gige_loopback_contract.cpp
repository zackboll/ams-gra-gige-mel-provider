// GIGE-MEL-011 GigE Vision LOOPBACK transport contract. The server is the
// pinned Aravis 0.8.36 network camera simulator ArvGvFakeCamera bound to
// 127.0.0.1 in this process (tests/support/aravis_gv_loopback.*). The client
// is the UNCHANGED production aravis::make_backend(): Aravis's GigE Vision
// interface, ArvGvDevice (GVCP over UDP) and ArvGvStream (GVSP over UDP).
// This is NOT the in-process ArvFakeInterface/ArvFakeStream used by the other
// SDK tests, and the project never calls arv_fake_camera_fill_buffer(): the
// simulator fills its own buffer on its own thread and transmits it as GVSP
// packets; the client reassembles them from its own UDP socket.
//
// Evidence kinds are kept separate:
//  A-D  transport, configuration, pixels and lifetime through the network;
//  E    the production capture_one() and time_probe() diagnostic routines;
//  M    MEL Image publication of network pixels under the EXISTING TEST-ONLY
//       virtual clock (tests/support/aravis_virtual_clock.*), and the
//       negative control without it (production FrameTimestamp{} rejected);
//  L    SIMULATED packet loss through the pinned "gvsp-lost-ratio" property.
//
// Simulated network transport only: no physical camera, NIC, switch, PTP,
// exposure timing or timestamp is qualified. Optics are VIRTUAL test values.
#include <gige_mel/provider.hpp>
#include "aravis_private.hpp" // observation-only hooks on the same GigE interface source
#include "aravis_gv_loopback.hpp"
#include "aravis_virtual_clock.hpp"

#include <arv.h>
#include <nlohmann/json.hpp>

#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
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
namespace lb = aravis_gv_loopback;
namespace vc = aravis_virtual_clock;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
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

const std::string device_id{lb::expected_device_id};
constexpr std::byte sentinel{0xA5};
constexpr std::chrono::milliseconds bound{10000}; // every wait is bounded; expiry is a failure
constexpr double h_rad_per_px = 0.0002;           // VIRTUAL test optics
constexpr double v_rad_per_px = 0.0003;           // VIRTUAL test optics

// Oracle profile: 10 000 us exposure and GainRaw 0 make the pinned Fake ramp
// scale exactly 1.0, so pixel(x, y) = (x + y + frame_id) % 255. The second
// profile (100 000 us) makes it exactly 2.0. Both are proven by readback.
const backend::CameraConfiguration oracle{64, 48, 8, 4, 25, 10ms};
const backend::CameraConfiguration second{80, 40, 16, 12, 10, 100ms};
// Pinned Fake defaults of a fresh simulator (src/arvfakecamera.h:57-82).
const backend::CameraConfiguration fake_default{512, 512, 0, 0, 25, 10ms};

// Pinned simulator register addresses (src/arvfakecamera.h).
constexpr std::uint32_t reg_width = 0x100, reg_height = 0x104, reg_offset_x = 0x130, reg_offset_y = 0x134;
constexpr std::uint32_t reg_exposure_us = 0x120, reg_frame_period_us = 0x138, reg_pixel_format = 0x128;
constexpr std::uint32_t reg_trigger_mode = 0x300, reg_gain_raw = 0x110;
constexpr std::uint32_t mono8_pfnc = 0x01080001; // ARV_PIXEL_FORMAT_MONO_8

// Independent oracle of the pinned Fake Mono8 diagonal ramp
// (src/arvfakecamera.c arv_fake_camera_diagonal_ramp, GainRaw 0):
//   CLAMP(((x + frame_id + y) % 255) * (1 + log10(exposure_us / 10000)), 0, 255)
unsigned expected_pixel(std::uint32_t x, std::uint32_t y, std::uint64_t frame_id, std::chrono::nanoseconds exposure) {
    const double exposure_us = static_cast<double>(exposure.count()) / 1000.0;
    const double scale = 1.0 + std::log10(exposure_us / 10000.0);
    const double value = static_cast<double>((x + frame_id + y) % 255) * scale;
    return static_cast<unsigned>(std::clamp(value, 0.0, 255.0));
}
// Forward distance on the pinned 16-bit Fake frame counter (1..65535; 0 skipped).
std::uint64_t id_distance(std::uint64_t from, std::uint64_t to) { return (to + 65535 - from) % 65535; }

template <class Predicate> void wait_until(Predicate predicate, std::chrono::milliseconds limit, const char* what) {
    const auto deadline = Clock::now() + limit;
    while (!predicate()) {
        if (Clock::now() >= deadline) throw std::runtime_error(std::string("timed out waiting for ") + what);
        std::this_thread::sleep_for(1ms);
    }
}
template <class Predicate> bool eventually(Predicate predicate, std::chrono::milliseconds limit = bound) {
    const auto end = Clock::now() + limit;
    while (!predicate()) {
        if (Clock::now() >= end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

void native(backend::ICameraDevice& device, const std::function<void(ArvDevice*)>& fn) {
    CHECK(aravis::with_native_device_for_testing(device, fn));
}
// CLIENT feature reads through the SDK: each is a GVCP round trip.
std::string native_string(backend::ICameraDevice& device, const char* feature) {
    std::string value;
    bool ok = false;
    native(device, [&](ArvDevice* d) {
        GError* error = nullptr;
        const char* text = arv_device_get_string_feature_value(d, feature, &error);
        ok = !error && text;
        if (ok) value = text;
        if (error) g_error_free(error);
    });
    CHECK(ok);
    return value;
}
std::int64_t native_integer(backend::ICameraDevice& device, const char* feature) {
    std::int64_t value = -1;
    bool ok = false;
    native(device, [&](ArvDevice* d) {
        GError* error = nullptr;
        value = arv_device_get_integer_feature_value(d, feature, &error);
        ok = !error;
        if (error) g_error_free(error);
    });
    CHECK(ok);
    return value;
}
std::string native_type(backend::ICameraDevice& device) {
    std::string type;
    native(device, [&](ArvDevice* d) { type = G_OBJECT_TYPE_NAME(d); });
    return type;
}
std::string socket_text(GSocketAddress* address) {
    auto* inet = G_INET_SOCKET_ADDRESS(address);
    char* host = g_inet_address_to_string(g_inet_socket_address_get_address(inet));
    std::string text = std::string(host ? host : "?") + ":" + std::to_string(g_inet_socket_address_get_port(inet));
    g_free(host);
    return text;
}
bool owns_udp_port(std::uint32_t port) {
    for (const auto& s : lb::owned_udp_sockets())
        if (s.port == port && !s.ipv6 && s.address == lb::loopback_address) return true;
    return false;
}

// The live simulator's audit at one point of the scenario: exactly the one
// documented global-discovery listener, recognized by its recorded identity
// and reported separately from the loopback-bound sockets; nothing else.
lb::SocketAudit audit_live(const lb::Simulator& sim, std::string_view where) {
    auto audit = sim.require_socket_audit(where);
    const auto& listener = sim.sockets().global_discovery;
    CHECK(audit.passed() && audit.exception_observed() && audit.unexpected.empty());
    CHECK(audit.discovery.size() == 1 && audit.expected_discovery == 1);
    const auto& seen = audit.discovery.front();
    CHECK(seen.inode == listener.inode && seen.fd == listener.fd);
    CHECK(seen.address == "255.255.255.255" && seen.port == lb::gvcp_port && !seen.ipv6);
    CHECK(seen.datagram_udp && seen.reuse_address && seen.reuse_port && seen.bound_device.empty());
    for (const auto& s : audit.loopback) CHECK(s.inode != listener.inode && s.address == lb::loopback_address);
    return audit;
}

// GObject weak-reference counter; never keeps the observed object alive.
// `last` records a process-wide finalization ordinal, so retirement ORDER
// (stream before device) is observable.
std::atomic<int> finalization_ordinal{0};
struct Finalized {
    std::atomic<int> count{0};
    std::atomic<int> last{0};
    static void notify(gpointer data, GObject*) {
        auto* self = static_cast<Finalized*>(data);
        self->last.store(finalization_ordinal.fetch_add(1) + 1);
        self->count.fetch_add(1);
    }
    void watch(gpointer object) { g_object_weak_ref(G_OBJECT(object), &Finalized::notify, this); }
};

// OBSERVATION-ONLY hooks on the IDENTICAL GigE Vision interface source that
// aravis::make_backend() uses ({"GigEVision", arv_gv_interface_get_instance}).
// Used only where a native object must be observed (stream type, pool size,
// finalization order, blocked native waits); no hook arranges or replaces
// anything. Declared before any backend so it outlives what it observes.
struct Observer {
    Finalized streams, buffers, devices;
    std::atomic<int> streams_created{0}, buffers_allocated{0}, devices_opened{0}, native_waits{0};
    std::mutex mutex;
    std::vector<std::string> stream_types, commands;
    aravis::TestHooks hooks() {
        aravis::TestHooks h;
        h.after_native_open = [this](ArvDevice* d) { devices_opened.fetch_add(1); devices.watch(d); };
        h.after_stream_created = [this](ArvStream* s) {
            { std::lock_guard lock(mutex); stream_types.emplace_back(G_OBJECT_TYPE_NAME(s)); }
            streams_created.fetch_add(1);
            streams.watch(s);
        };
        h.after_buffer_allocated = [this](ArvBuffer* b) { buffers_allocated.fetch_add(1); buffers.watch(b); };
        h.before_command = [this](std::string_view c, ArvDevice*) { std::lock_guard lock(mutex); commands.emplace_back(c); };
        h.before_native_wait = [this] { native_waits.fetch_add(1); };
        return h;
    }
    std::shared_ptr<backend::ICameraBackend> backend() {
        return aravis::make_backend_for_interface({"GigEVision", &arv_gv_interface_get_instance}, hooks());
    }
    std::vector<std::string> types() { std::lock_guard lock(mutex); return stream_types; }
    std::vector<std::string> command_log() { std::lock_guard lock(mutex); return commands; }
    int live_streams() const { return streams_created.load() - streams.count.load(); }
    int live_buffers() const { return buffers_allocated.load() - buffers.count.load(); }
    int live_devices() const { return devices_opened.load() - devices.count.load(); }
};

std::shared_ptr<backend::ICameraDevice> open_device(backend::ICameraBackend& camera) {
    auto opened = camera.open(device_id);
    if (!opened) throw std::runtime_error("open " + device_id + " over GVCP failed: " + opened.error().message);
    return std::move(opened).value();
}

// Applies `c` over GVCP; proves it by client readback AND by the simulator's
// own registers (server side), plus GainRaw 0, Mono8 and free-running mode.
void apply_and_prove(lb::Simulator& sim, backend::ICameraDevice& device, const backend::CameraConfiguration& c) {
    const auto applied = device.apply(c);
    if (!applied) throw std::runtime_error("apply over GVCP failed: " + applied.error().message);
    CHECK(applied.value() == c);
    CHECK(device.configuration().value() == c);
    CHECK(sim.read_register(reg_width) == c.width && sim.read_register(reg_height) == c.height);
    CHECK(sim.read_register(reg_offset_x) == c.offset_x && sim.read_register(reg_offset_y) == c.offset_y);
    CHECK(std::int64_t{sim.read_register(reg_exposure_us)} * 1000 == c.exposure.count());
    CHECK(sim.read_register(reg_frame_period_us) == 1000000u / c.frame_rate_hz);
    CHECK(native_integer(device, "GainRaw") == 0 && sim.read_register(reg_gain_raw) == 0);
    CHECK(native_string(device, "PixelFormat") == "Mono8" && sim.read_register(reg_pixel_format) == mono8_pfnc);
    CHECK(native_string(device, "TriggerMode") == "Off" && sim.read_register(reg_trigger_mode) == 0);
    CHECK(native_string(device, "AcquisitionMode") == "Continuous" && sim.free_running());
}
std::shared_ptr<backend::ICameraDevice> open_configured(lb::Simulator& sim, backend::ICameraBackend& camera,
                                                        const backend::CameraConfiguration& c = oracle) {
    auto device = open_device(camera);
    CHECK(native_type(*device) == "ArvGvDevice");
    apply_and_prove(sim, *device, c);
    return device;
}

// Every pixel of a leased frame against the independent oracle.
bool pixels_match(const backend::FrameLease& frame) {
    const auto& d = frame.descriptor();
    for (std::uint32_t y = 0; y < d.height; ++y)
        for (std::uint32_t x = 0; x < d.width; ++x) {
            const auto actual = std::to_integer<unsigned>(frame.bytes()[std::size_t{y} * d.row_stride_bytes + x]);
            if (actual != expected_pixel(x, y, d.native_frame_id, d.configuration.exposure)) return false;
        }
    return true;
}
bool unqualified(const backend::FrameTimestamp& t) {
    return !t.valid && t.domain == backend::ClockDomain::unspecified && t.event == backend::TimestampEvent::unspecified &&
           t.value.count() == 0 && t.clock_generation == 0;
}
// A genuine, complete Mono8 network frame of configuration `c`.
void check_frame(const backend::FrameLease& frame, const backend::CameraConfiguration& c, std::uint64_t generation) {
    const auto& d = frame.descriptor();
    CHECK(d.width == c.width && d.height == c.height && d.row_stride_bytes == c.width);
    CHECK(d.storage_bits_per_pixel == 8 && d.significant_bits_per_pixel == 8 && d.bands == 1);
    CHECK(d.pixel_format == backend::PixelFormat::mono8 && d.byte_order == backend::ByteOrder::not_applicable);
    CHECK(d.status == backend::FrameStatus::complete);
    CHECK(d.configuration == c && d.configuration_generation == generation);
    CHECK(d.stream_generation != 0 && d.native_frame_id >= 1 && d.native_frame_id <= 65535);
    CHECK(frame.bytes().size() == std::size_t{c.width} * c.height);
    CHECK(unqualified(d.timestamp)); // production descriptors stay FrameTimestamp{}
    CHECK(pixels_match(frame));
}
backend::FrameLease receive_ok(backend::IFrameStream& stream, std::chrono::milliseconds timeout = 5s) {
    auto result = stream.receive(timeout);
    if (!result) throw std::runtime_error("network receive failed: " + result.error().message);
    return std::move(result).value();
}
std::vector<std::byte> copy_bytes(const backend::FrameLease& f) { return {f.bytes().begin(), f.bytes().end()}; }

// SDK-side GVSP counter of a live ArvGvStream (pinned src/arvgvstream.c:1680-1709),
// declared as a uint64 stream info (the type is checked before reading).
std::uint64_t stream_info(ArvStream* stream, const char* name) {
    bool declared = false;
    for (guint i = 0; i < arv_stream_get_n_infos(stream); ++i)
        if (std::string_view(arv_stream_get_info_name(stream, i)) == name)
            declared = arv_stream_get_info_type(stream, i) == G_TYPE_UINT64;
    CHECK(declared);
    return arv_stream_get_info_uint64_by_name(stream, name);
}

// ===========================================================================
// A. Discovery and device ownership through real GigE Vision discovery/GVCP.
void discovery_and_ownership() {
    lb::Simulator sim;
    {
        const auto started = audit_live(sim, "after simulator start");
        // Exactly the pinned simulator's three sockets exist, two of them on
        // 127.0.0.1 (GVCP 3956 and the GVSP output) and the documented
        // wildcard listener, which is NOT counted as loopback.
        CHECK(started.loopback.size() == 2);
        std::cout << "  socket audit " << started.summary() << '\n';
    }
    auto camera = aravis::make_backend(); // the production GigE factory, unchanged
    CHECK(camera->id() == aravis::backend_id);
    const auto devices = sim.discover(*camera);
    // Exactly the simulator: discovery is restricted to loopback, so no
    // physical camera can appear; the ID is what discovery reported.
    CHECK(devices.size() == 1);
    CHECK(devices.front().device_id == lb::expected_device_id);
    CHECK(devices.front().display_name == "Aravis Fake serial GVTest (GigEVision 127.0.0.1)");
    std::cout << "  discovered on '" << sim.discovery_interface() << "': " << devices.front().device_id << " ["
              << devices.front().display_name << "]\n";

    auto device = open_device(*camera);
    CHECK(native_type(*device) == "ArvGvDevice"); // not ArvFakeDevice
    native(*device, [](ArvDevice* d) {
        CHECK(ARV_IS_GV_DEVICE(d) && !ARV_IS_FAKE_DEVICE(d));
        CHECK(socket_text(arv_gv_device_get_device_address(ARV_GV_DEVICE(d))) == "127.0.0.1:3956");
        CHECK(socket_text(arv_gv_device_get_interface_address(ARV_GV_DEVICE(d))).rfind("127.0.0.1:", 0) == 0);
        CHECK(arv_gv_device_is_controller(ARV_GV_DEVICE(d)));
    });
    CHECK(sim.control_privilege() != 0); // the simulator granted GVCP control to this client
    CHECK(aravis::runtime_version() == "0.8.36" && aravis::compiled_version() == "0.8.36");
    CHECK(native_string(*device, "DeviceVendorName") == "Aravis");
    CHECK(native_string(*device, "DeviceModelName") == "Fake");
    CHECK(native_string(*device, "DeviceID") == "GVTest"); // pinned XML: DeviceID reads the serial register
    // Feature reads are genuine GVCP round trips: a nonce written into the
    // SERVER's memory is read back through the client feature, twice.
    sim.write_test_register(0xC0FFEE01u);
    CHECK(native_integer(*device, "TestRegister") == 0xC0FFEE01);
    sim.write_test_register(0xC0FFEE02u);
    CHECK(native_integer(*device, "TestRegister") == 0xC0FFEE02);

    // Ownership: a competing open of the same exact ID is refused by the
    // adapter's process-wide reservation, from this or another backend.
    auto rival = aravis::make_backend();
    for (auto* other : {camera.get(), rival.get()}) {
        const auto refused = other->open(device_id);
        CHECK(!refused && refused.error().code == backend::ErrorCode::busy);
        CHECK(contains(refused.error().message, "already open in this process"));
    }
    // Exact IDs only: aliases the SDK itself would accept, the serial alone,
    // the address and an absent device are all not_found.
    for (const char* other : {"Aravis-GVTest", "GVTest", "127.0.0.1", "Aravis-Fake-GV404"}) {
        const auto refused = rival->open(other);
        CHECK(!refused && refused.error().code == backend::ErrorCode::not_found);
        CHECK(contains(refused.error().message, "no other device or alias was tried"));
    }
    CHECK(sim.control_privilege() != 0);

    // Close drops the native device (GVCP control left) and then the
    // reservation; an explicit reopen through another backend succeeds.
    CHECK(device->close());
    CHECK(sim.control_privilege() == 0);
    CHECK(device->close()); // idempotent, no device I/O
    CHECK(device->configuration().error().code == backend::ErrorCode::disconnected);
    auto reopened = open_device(*rival);
    CHECK(native_type(*reopened) == "ArvGvDevice" && sim.control_privilege() != 0);
    CHECK(reopened->close());
    CHECK(sim.control_privilege() == 0);
    // After client close only the simulator's own sockets remain.
    CHECK(audit_live(sim, "after discovery and client close").loopback.size() == 2);
    reopened.reset();
    device.reset();
    sim.finish();
    const auto& released = sim.release_audit();
    CHECK(released && released->passed() && !released->exception_observed() && released->unexpected.empty());
    std::cout << "  socket audit " << released->summary() << '\n';
}

// ===========================================================================
// B. Configuration through real GVCP: initial readback, supported bounds,
// the existing transaction, no hidden trigger/format change, no write while
// active, reconfiguration after a completed stop.
void configuration_over_gvcp() {
    lb::Simulator sim;
    Observer seen;
    auto camera = seen.backend();
    (void)sim.discover(*camera);
    auto device = open_device(*camera);
    CHECK(native_type(*device) == "ArvGvDevice");
    // Initial native configuration, read over the network.
    const auto initial = device->configuration();
    CHECK(initial && initial.value() == fake_default);
    const auto caps = device->capabilities();
    CHECK(caps.sensor_width == 2048 && caps.sensor_height == 2048 && caps.mono8);
    CHECK(caps.width_increment == 1 && caps.height_increment == 1);
    CHECK(caps.offset_x_increment == 1 && caps.offset_y_increment == 1);
    // The requested profile lies inside the ACTUAL advertised bounds.
    CHECK(oracle.width + oracle.offset_x <= caps.sensor_width && oracle.height + oracle.offset_y <= caps.sensor_height);
    CHECK(oracle.frame_rate_hz >= caps.minimum_frame_rate_hz && oracle.frame_rate_hz <= caps.maximum_frame_rate_hz);
    CHECK(oracle.exposure >= caps.minimum_exposure && !caps.external_trigger);
    const auto trigger_before = native_string(*device, "TriggerMode");
    const auto format_before = native_string(*device, "PixelFormat");
    CHECK(trigger_before == "Off" && format_before == "Mono8");

    apply_and_prove(sim, *device, oracle); // client readback AND server registers
    CHECK(native_string(*device, "TriggerMode") == trigger_before); // no hidden trigger change
    CHECK(native_string(*device, "PixelFormat") == format_before);  // no hidden format change
    CHECK(sim.payload_bytes() == std::size_t{oracle.width} * oracle.height);
    CHECK(native_integer(*device, "PayloadSize") == std::int64_t{oracle.width} * oracle.height);

    // An unsupported request is rejected before any write; native state unchanged.
    const auto rejected = device->apply({4096, 48, 0, 0, 25, 10ms});
    CHECK(!rejected && device->configuration().value() == oracle);

    // Configuration generation is the caller's tag; the frozen run
    // configuration is the fresh readback at start.
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(31));
    CHECK(sim.acquisition_status() == 1);
    const auto blocked = device->apply(second);
    CHECK(!blocked && blocked.error().code == backend::ErrorCode::invalid_state);
    CHECK(contains(blocked.error().message, "nothing was written"));
    CHECK(sim.read_register(reg_width) == oracle.width && sim.read_register(reg_exposure_us) == 10000);
    {
        auto frame = receive_ok(*stream);
        check_frame(frame, oracle, 31);
    }
    stream->stop();
    CHECK(sim.acquisition_status() == 0);

    // Reconfigure after the completed stop; a new run carries the new
    // configuration and generation; pixels follow the new oracle (scale 2).
    apply_and_prove(sim, *device, second);
    CHECK(native_string(*device, "TriggerMode") == "Off" && native_string(*device, "PixelFormat") == "Mono8");
    CHECK(stream->start(32));
    for (int i = 0; i < 3; ++i) {
        auto frame = receive_ok(*stream);
        check_frame(frame, second, 32);
    }
    stream->stop();
    CHECK(device->close());
    CHECK(seen.live_streams() == 0 && seen.live_buffers() == 0);
    device.reset();
    stream.reset();
    sim.finish();
}

// Borrowed view of the run's live ArvGvStream through a GObject weak
// reference (never keeps it alive). Read only while the run is active on
// this thread, so the temporary strong reference is never the last one.
struct StreamView {
    GWeakRef ref;
    StreamView() { g_weak_ref_init(&ref, nullptr); }
    ~StreamView() { g_weak_ref_clear(&ref); }
    StreamView(const StreamView&) = delete;
    StreamView& operator=(const StreamView&) = delete;
    void set(ArvStream* s) { g_weak_ref_set(&ref, s); }
    std::uint64_t info(const char* name) {
        auto* s = static_cast<ArvStream*>(g_weak_ref_get(&ref));
        CHECK(s);
        const auto value = stream_info(s, name);
        g_object_unref(s);
        return value;
    }
    bool alive() {
        auto* s = g_weak_ref_get(&ref);
        if (s) g_object_unref(s);
        return s != nullptr;
    }
};

// ===========================================================================
// C. Real GVSP Mono8 frame transfer through ArvGvStream.
void gvsp_frame_transfer() {
    lb::Simulator sim;
    Observer seen;
    StreamView view;
    auto hooks = seen.hooks();
    auto created_hook = hooks.after_stream_created;
    hooks.after_stream_created = [&](ArvStream* s) { created_hook(s); view.set(s); };
    auto camera = aravis::make_backend_for_interface({"GigEVision", &arv_gv_interface_get_instance}, hooks);
    (void)sim.discover(*camera);
    auto device = open_configured(sim, *camera);
    auto stream = std::move(device->create_stream()).value();
    CHECK(seen.streams_created == 0 && sim.stream_port() == 0); // wrapper creation does no native work
    CHECK(stream->start(41));
    CHECK((seen.types() == std::vector<std::string>{"ArvGvStream"}));
    CHECK(seen.buffers_allocated == 4); // the adapter's fixed pool
    // The client programmed the simulator's stream channel to ITS OWN UDP
    // socket on 127.0.0.1 (server-side SCDA0/SCP0 registers).
    const auto port = sim.stream_port();
    CHECK(port != 0 && sim.stream_destination() == lb::loopback_address);
    CHECK(owns_udp_port(port)); // the GVSP destination is a socket of THIS process
    {
        const auto acquiring = audit_live(sim, "during GVCP/GVSP acquisition");
        bool client_stream_socket = false; // the client's GVSP socket is loopback-bound
        for (const auto& s : acquiring.loopback) client_stream_socket = client_stream_socket || s.port == port;
        CHECK(client_stream_socket);
        std::cout << "  socket audit " << acquiring.summary() << '\n';
    }
    const auto packet_size = sim.read_register(0x0d04) & 0xffffu; // GVBS SCPS0 packet size
    CHECK(packet_size > 36 && packet_size <= 9000);
    // The simulator advertises no packet-resend capability (GVBS 0x0934 bit 2),
    // so the pinned ArvGvDevice disables resend: no resend claim is made here.
    CHECK((sim.read_register(0x0934) & (1u << 2)) == 0);
    std::uint64_t generation = 0, previous = 0;
    constexpr int frames = 8;
    for (int i = 0; i < frames; ++i) {
        auto frame = receive_ok(*stream);
        check_frame(frame, oracle, 41); // geometry, Mono8, generations, size, EVERY pixel
        const auto& d = frame.descriptor();
        if (i == 0) generation = d.stream_generation;
        CHECK(d.stream_generation == generation);
        if (i) CHECK(id_distance(previous, d.native_frame_id) >= 1 && id_distance(previous, d.native_frame_id) <= 4);
        previous = d.native_frame_id;
    }
    // SDK GVSP accounting: every frame was reassembled from several UDP
    // datagrams (leader, >= 1 payload, trailer) received by ArvGvStream.
    const auto payload = std::size_t{oracle.width} * oracle.height;
    const std::uint64_t per_frame = (payload + packet_size - 1) / packet_size + 2;
    const auto completed = view.info("n_completed_buffers");
    const auto packets = view.info("n_received_packets"); // since this run's stream was created
    CHECK(completed >= frames && packets >= frames * per_frame);
    CHECK(view.info("n_failures") == 0 && view.info("n_missing_packets") == 0);
    const auto stats = stream->statistics();
    CHECK(stats.delivered_frames == frames && stats.incomplete_frames == 0 && stats.timeout_count == 0);
    std::cout << "  GVSP to 127.0.0.1:" << port << ", packet size " << packet_size << ": " << frames
              << " frames, last native ID " << previous << ", " << packets << " UDP packets received, "
              << completed << " SDK buffers completed; every pixel matches (x + y + id) % 255\n";
    stream->stop();
    CHECK(sim.acquisition_status() == 0 && sim.stream_port() == 0); // the SDK finalizer closed the channel
    CHECK(!view.alive() && !owns_udp_port(port));                   // stream and its socket released
    CHECK(device->close());
    CHECK(seen.live_streams() == 0 && seen.live_buffers() == 0 && seen.live_devices() == 0);
    stream.reset();
    device.reset();
    sim.finish();
}

// ===========================================================================
// D. GVCP AcquisitionStart/Stop observed on the SERVER, independently of
// frame arrival; stop/restart; lease lifetime; retirement order; reopen.
void acquisition_and_lifetime() {
    lb::Simulator sim;
    Observer seen; // outlives every backend, device and lease below
    std::optional<backend::FrameLease> kept;
    std::vector<std::byte> kept_bytes;
    backend::FrameDescriptor kept_descriptor;
    {
        auto camera = seen.backend();
        (void)sim.discover(*camera);
        auto device = open_configured(sim, *camera);
        auto stream = std::move(device->create_stream()).value();
        CHECK(sim.acquisition_status() == 0);
        CHECK(stream->start(51));
        CHECK(sim.acquisition_status() == 1); // the GVCP command reached the simulator
        CHECK((seen.command_log() == std::vector<std::string>{"AcquisitionStart"}));
        kept = receive_ok(*stream);
        check_frame(*kept, oracle, 51);
        kept_bytes = copy_bytes(*kept);
        kept_descriptor = kept->descriptor();
        { auto next = receive_ok(*stream); check_frame(next, oracle, 51); } // released -> requeued
        // The restart lease rule: refused while a lease of the run is held.
        stream->stop();
        CHECK(sim.acquisition_status() == 0); // GVCP AcquisitionStop, server-side
        CHECK((seen.command_log() == std::vector<std::string>{"AcquisitionStart", "AcquisitionStop"}));
        CHECK(seen.live_streams() == 0); // native stream finalized synchronously during stop()
        CHECK(seen.live_buffers() == 1); // only the leased buffer is still alive
        const auto refused = stream->start(52);
        CHECK(!refused && refused.error().code == backend::ErrorCode::resource_exhausted);
        CHECK(contains(refused.error().message, "release them before restarting"));
        CHECK(sim.acquisition_status() == 0 && seen.streams_created == 1);
        // Held bytes are unchanged after stop although the simulator keeps
        // running (and would reuse its own send buffer).
        std::this_thread::sleep_for(100ms);
        CHECK(copy_bytes(*kept) == kept_bytes && pixels_match(*kept));
        // Retirement order: the native stream was finalized (weak-reference
        // ordinal) before the native device, and close() returned only after
        // the device was finalized and GVCP control released on the server;
        // only then does a rival backend's open of the same ID succeed.
        CHECK(device->close());
        CHECK(seen.live_devices() == 0 && sim.control_privilege() == 0);
        CHECK(seen.streams.last.load() < seen.devices.last.load());
        auto rival = aravis::make_backend();
        auto reopened = rival->open(device_id);
        CHECK(reopened);
        CHECK(reopened.value()->close());
        stream.reset();
        device.reset();
        camera.reset();
    }
    CHECK(copy_bytes(*kept) == kept_bytes && pixels_match(*kept));
    CHECK(kept->descriptor().native_frame_id == kept_descriptor.native_frame_id);

    // Reopen and acquire while the OLD lease is held: the retired run's
    // buffer is freed, never requeued into the new ArvGvStream.
    auto camera = seen.backend();
    auto device = open_configured(sim, *camera);
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(53));
    CHECK(sim.acquisition_status() == 1);
    auto fresh = receive_ok(*stream);
    check_frame(fresh, oracle, 53);
    CHECK(fresh.descriptor().stream_generation != kept_descriptor.stream_generation);
    const int freed_before = seen.buffers.count.load();
    std::thread([lease = std::move(*kept)]() mutable { auto drop = std::move(lease); }).join();
    kept.reset();
    CHECK(seen.buffers.count.load() == freed_before + 1);
    CHECK(seen.live_buffers() == 4); // the new run's pool, unaffected
    for (int i = 0; i < 4; ++i) {
        auto next = receive_ok(*stream);
        check_frame(next, oracle, 53);
        CHECK(next.descriptor().stream_generation == fresh.descriptor().stream_generation);
    }
    // Explicit stop and restart of the same wrapper after the lease rule is met.
    const auto first_generation = fresh.descriptor().stream_generation;
    { auto drop = std::move(fresh); }
    stream->stop();
    CHECK(sim.acquisition_status() == 0);
    CHECK(stream->start(54));
    CHECK(sim.acquisition_status() == 1);
    auto restarted = receive_ok(*stream);
    check_frame(restarted, oracle, 54);
    CHECK(restarted.descriptor().stream_generation != first_generation);
    { auto drop = std::move(restarted); }
    stream->stop();
    CHECK(sim.acquisition_status() == 0);
    CHECK(device->close());
    CHECK(seen.live_buffers() == 0 && seen.live_streams() == 0 && seen.live_devices() == 0);
    stream.reset();
    device.reset();
    sim.finish();
}

// Joins the receiver on every path (including a failing CHECK), never from
// the receiving thread itself.
struct ReceiverCleanup {
    backend::IFrameStream& stream;
    std::thread& receiver;
    ~ReceiverCleanup() {
        stream.request_stop(); // releases a blocked receive within one polling slice
        if (receiver.joinable()) receiver.join();
        stream.stop();
    }
};

// D2. Prompt cancellation of a GENUINELY blocked receive during real network
// acquisition: every pool buffer is leased (GVSP frames keep arriving but
// have nowhere to go), and receive is observed inside native timed pops.
enum class Canceller { request_stop, stop, thrown, close };
const char* name_of(Canceller how) {
    switch (how) {
    case Canceller::request_stop: return "request_stop";
    case Canceller::stop: return "stop";
    case Canceller::thrown: return "exception unwind";
    case Canceller::close: return "close";
    }
    return "?";
}
void cancel_once(lb::Simulator& sim, Observer& seen, backend::ICameraDevice& device, Canceller how, std::uint64_t tag) {
    auto stream = std::move(device.create_stream()).value();
    std::vector<backend::FrameLease> held; // outlives the cleanup below
    std::atomic<bool> finished{false};
    std::optional<backend::Error> outcome;
    Clock::time_point returned{}, cancelled_at{};
    std::thread receiver;
    try {
        ReceiverCleanup cleanup{*stream, receiver}; // before any acquisition activity
        CHECK(stream->start(tag));
        CHECK(sim.acquisition_status() == 1);
        for (int i = 0; i < 4; ++i) held.push_back(receive_ok(*stream));
        const int waits_before = seen.native_waits.load();
        receiver = std::thread([&] {
            auto result = stream->receive(60s); // long public timeout
            returned = Clock::now();
            if (!result) outcome = result.error();
            finished = true;
        });
        wait_until([&] { return seen.native_waits.load() >= waits_before + 3; }, 5s, "receive to enter native waits");
        CHECK(!finished && sim.acquisition_status() == 1); // the simulator is still streaming
        cancelled_at = Clock::now();
        if (how == Canceller::thrown) throw std::logic_error("deliberate failure while receive is blocked");
        if (how == Canceller::request_stop) stream->request_stop();
        if (how == Canceller::stop) stream->stop();
        if (how == Canceller::close) CHECK(device.close());
        wait_until([&] { return finished.load(); }, 2s, "cancelled network receive to return");
    } catch (const std::logic_error& error) {
        CHECK(how == Canceller::thrown && contains(error.what(), "deliberate failure"));
    }
    CHECK(!receiver.joinable() && finished);
    CHECK(returned - cancelled_at < 1s && Clock::now() - cancelled_at < 3s); // never the 60 s timeout
    CHECK(outcome && outcome->code == (how == Canceller::close ? backend::ErrorCode::disconnected
                                                                : backend::ErrorCode::stopped));
    CHECK(stream->statistics().timeout_count == 0);
    CHECK(sim.acquisition_status() == 0); // GVCP AcquisitionStop reached the simulator
    for (const auto& lease : held) CHECK(pixels_match(lease)); // still valid after retirement
    held.clear();
    CHECK(seen.live_buffers() == 0 && seen.live_streams() == 0);
    std::cout << "  cancel by " << name_of(how) << ": returned in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(returned - cancelled_at).count() << " ms\n";
}
void cancellation() {
    lb::Simulator sim;
    Observer seen;
    auto camera = seen.backend();
    (void)sim.discover(*camera);
    auto device = open_configured(sim, *camera);
    std::uint64_t tag = 60;
    for (const auto how : {Canceller::request_stop, Canceller::stop, Canceller::thrown, Canceller::close})
        cancel_once(sim, seen, *device, how, ++tag);
    CHECK(sim.control_privilege() == 0); // close released GVCP control
    // A fresh open acquires again after the close-cancelled run.
    device = open_configured(sim, *camera);
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(70));
    { auto frame = receive_ok(*stream); check_frame(frame, oracle, 70); }
    stream->stop();
    CHECK(device->close());
    stream.reset();
    device.reset();
    sim.finish();
}

// A second SDK signal subscriber proves that the heartbeat really emitted the
// event. The closure owns its observation; no borrowed test-stack user data.
// This observer does not set the adapter's terminal state.
struct LossSignal {
    // TEST observer only: registration happens after heartbeat creation for
    // production-factory scenarios. Make its user-data publication visible to
    // TSan without adding SDK suppressions or changing the production callback.
    inline static std::mutex publication_mutex;
    std::atomic<std::int64_t> at_ns{0};
    std::atomic<int> count{0};
    std::atomic<bool> hold{false}, release{false}; // controlled close/signal overlap, test only
    static std::int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }
    static void notified(ArvDevice*, gpointer data) noexcept {
        std::shared_ptr<LossSignal> pinned;
        { std::lock_guard lock(publication_mutex); pinned = *static_cast<std::shared_ptr<LossSignal>*>(data); }
        auto& signal = *pinned;
        signal.at_ns.store(now_ns());
        signal.count.fetch_add(1);
        // TEST observer only: keep the SDK emission in flight while close
        // disconnects the production handler and reaches device finalization.
        const auto deadline = Clock::now() + 10s;
        while (signal.hold && !signal.release && Clock::now() < deadline) std::this_thread::yield();
    }
    static void destroyed(gpointer data, GClosure*) noexcept {
        std::lock_guard lock(publication_mutex);
        delete static_cast<std::shared_ptr<LossSignal>*>(data);
    }
};
std::shared_ptr<LossSignal> observe_loss(backend::ICameraDevice& device) {
    auto signal = std::make_shared<LossSignal>();
    native(device, [&](ArvDevice* d) {
        std::lock_guard lock(LossSignal::publication_mutex);
        g_signal_connect_data(d, "control-lost", G_CALLBACK(LossSignal::notified),
                              new std::shared_ptr<LossSignal>(signal), LossSignal::destroyed, G_CONNECT_DEFAULT);
    });
    return signal;
}
template <class T> void require_control_loss(const T& result) {
    CHECK(!result && result.error().code == backend::ErrorCode::disconnected);
    CHECK(contains(result.error().message, "Aravis reported camera control lost"));
}

// Q1. The primary control uses make_backend(), not an injected client flag.
// Pool exhaustion plus a recorded starvation episode proves the receiver has
// completed empty native waits, rather than merely having launched a thread.
void control_loss_active() {
    lb::Simulator sim;
    auto camera = aravis::make_backend();
    (void)sim.discover(*camera);
    auto device = open_configured(sim, *camera);
    auto signal = observe_loss(*device);
    auto stream = std::move(device->create_stream()).value();
    auto second_wrapper = std::move(device->create_stream()).value();
    CHECK(stream->start(120));
    std::vector<backend::FrameLease> held;
    for (std::size_t i = 0; i < gige_mel::backend::aravis_profile::pool_buffers; ++i)
        held.push_back(receive_ok(*stream));
    for (const auto& f : held) check_frame(f, oracle, 120);
    const auto saved = copy_bytes(held.front());
    const auto descriptor = held.front().descriptor();
    std::optional<backend::Error> error;
    std::atomic<bool> done{false};
    std::int64_t returned_ns = 0;
    std::thread receiver([&] {
        auto result = stream->receive(60s);
        if (!result) error = result.error();
        returned_ns = LossSignal::now_ns();
        done.store(true);
    });
    {
        ReceiverCleanup cleanup{*stream, receiver}; // cancel BEFORE joining, including assertion failures
        wait_until([&] { return stream->statistics().pool_starvation_events > 0; }, 5s, "empty native wait");
        CHECK(!done && sim.control_privilege() != 0 && sim.acquisition_status() == 1);
        const auto revoked_ns = LossSignal::now_ns();
        sim.revoke_control();
        CHECK(sim.control_privilege() == 0); // the heartbeat reads this very register over GVCP
        wait_until([&] { return signal->count.load() == 1; }, 10s, "actual SDK control-lost signal");
        std::cout << "  SDK control-lost emitted after server CCP readback zero; heartbeat detection "
                  << (signal->at_ns - revoked_ns) / 1000000 << " ms" << std::endl;
        wait_until([&] { return done.load(); }, 1s, "terminal receive after signal");
        CHECK(error && error->code == backend::ErrorCode::disconnected);
        CHECK(contains(error->message, "Aravis reported camera control lost"));
        CHECK(returned_ns - signal->at_ns.load() < 500000000); // event handling, NOT heartbeat detection delay
        CHECK(stream->statistics().timeout_count == 0);
        std::cout << "  actual control-lost: heartbeat detection " << (signal->at_ns - revoked_ns) / 1000000
                  << " ms, receive after signal " << (returned_ns - signal->at_ns) / 1000000 << " ms\n";
    }
    // A successful native read still works; it must NOT heal the terminal latch.
    CHECK(native_integer(*device, "Width") == oracle.width);
    const auto registers_before = sim.read_register(reg_width);
    require_control_loss(device->apply(second));
    CHECK(sim.read_register(reg_width) == registers_before);
    require_control_loss(device->configuration());
    require_control_loss(device->create_stream());
    require_control_loss(stream->start(121));
    require_control_loss(second_wrapper->start(122));
    require_control_loss(stream->receive(1ms));
    require_control_loss(device->close());
    CHECK(device->close()); // idempotent, no more SDK I/O
    stream.reset(); second_wrapper.reset(); device.reset(); camera.reset();
    CHECK(copy_bytes(held.front()) == saved && pixels_match(held.front()));
    CHECK(held.front().descriptor().native_frame_id == descriptor.native_frame_id);
    sim.reset_after_control_loss(); // explicit SERVER action, not automatic adapter recovery
    camera = aravis::make_backend();
    device = open_configured(sim, *camera);
    CHECK(device->configuration());
    stream = std::move(device->create_stream()).value();
    CHECK(stream->start(123));
    { auto f = receive_ok(*stream); check_frame(f, oracle, 123);
      CHECK(f.descriptor().stream_generation != descriptor.stream_generation); }
    std::thread release([leases = std::move(held)]() mutable { leases.clear(); });
    release.join();
    stream->stop(); CHECK(device->close());
    stream.reset(); device.reset(); camera.reset();
    sim.finish();
}

// Q2. Observation-only seams count native resources and writes. No signal is
// fabricated. The old source lease survives all owners and frees exactly once.
void control_loss_lifetime() {
    lb::Simulator sim;
    Observer seen;
    auto hooks = seen.hooks();
    std::atomic<int> writes{0};
    hooks.before_write = [&](std::string_view, ArvDevice*) { ++writes; };
    auto camera = aravis::make_backend_for_interface({"GigEVision", &arv_gv_interface_get_instance}, hooks);
    (void)sim.discover(*camera);
    auto device = open_configured(sim, *camera);
    auto signal = observe_loss(*device);
    auto stream = std::move(device->create_stream()).value();
    auto other = std::move(device->create_stream()).value();
    CHECK(stream->start(130));
    auto frame = receive_ok(*stream);
    const auto saved = copy_bytes(frame);
    sim.revoke_control();
    wait_until([&] { return signal->count.load() == 1; }, 10s, "SDK signal");
    const auto writes_before = writes.load();
    const auto created_before = seen.streams_created.load();
    require_control_loss(device->apply(second));
    require_control_loss(other->start(131));
    require_control_loss(stream->receive(0ms));
    CHECK(writes == writes_before && seen.streams_created == created_before);
    stream->stop(); stream->stop(); // no AcquisitionStop command after loss
    CHECK((seen.command_log() == std::vector<std::string>{"AcquisitionStart"}));
    CHECK(seen.live_streams() == 0 && seen.live_buffers() == 1);
    require_control_loss(device->close());
    CHECK(seen.live_devices() == 0 && seen.streams.last < seen.devices.last);
    stream.reset(); other.reset(); device.reset(); camera.reset();
    CHECK(copy_bytes(frame) == saved && pixels_match(frame));
    const int freed = seen.buffers.count;
    std::thread([lease = std::move(frame)]() mutable { auto drop = std::move(lease); }).join();
    CHECK(seen.buffers.count == freed + 1 && seen.live_buffers() == 0);
    sim.reset_after_control_loss(); sim.finish();
}

// Q3. An ACTUAL heartbeat emission overlaps close. Holding only the test
// subscriber in-flight makes native stream retirement and the device join
// observable without changing the production callback or adding a worker.
void control_loss_close_overlap() {
    lb::Simulator sim;
    Observer seen;
    auto camera = seen.backend();
    (void)sim.discover(*camera);
    for (int i = 0; i < 3; ++i) {
        auto device = open_configured(sim, *camera);
        auto signal = observe_loss(*device);
        signal->hold = true;
        auto stream = std::move(device->create_stream()).value();
        CHECK(stream->start(140 + static_cast<std::uint64_t>(i)));
        { auto f = receive_ok(*stream); CHECK(pixels_match(f)); }
        sim.revoke_control();
        wait_until([&] { return signal->count.load() == 1; }, 10s, "in-flight SDK emission");
        std::atomic<bool> closed{false};
        std::optional<backend::Error> error;
        const auto began = Clock::now();
        std::thread closer([&] { auto result = device->close(); if (!result) error = result.error(); closed = true; });
        // Always release the test signal BEFORE joining, including failure.
        const bool retired = eventually([&] { return seen.live_streams() == 0; }, 5s);
        const bool joined_early = closed.load();
        signal->release = true;
        closer.join();
        CHECK(retired && !joined_early && closed && Clock::now() - began < 5s);
        CHECK(error && contains(error->message, "Aravis reported camera control lost"));
        CHECK(seen.live_devices() == 0 && seen.live_buffers() == 0);
        CHECK(device->close());
        stream.reset(); device.reset();
        sim.reset_after_control_loss();
        // A fresh logical device cannot inherit the old signal/latch.
        auto fresh = open_configured(sim, *camera);
        auto next = std::move(fresh->create_stream()).value();
        CHECK(next->start(150 + static_cast<std::uint64_t>(i)));
        { auto f = receive_ok(*next); CHECK(pixels_match(f)); }
        next->stop(); CHECK(fresh->close());
        CHECK(signal->count == 1);
    }
    sim.finish();
}

// Q5. A real popped frame is paused in decoded-facts observation (no edited
// facts), then a real heartbeat signal arrives before frame admission. No
// complete frame, malformed frame or timeout may escape instead of the error.
void control_loss_admission() {
    lb::Simulator sim;
    std::atomic<bool> decoded{false}, resume{false};
    auto camera = aravis::make_backend_for_interface({"GigEVision", &arv_gv_interface_get_instance}, [&] {
        aravis::TestHooks h;
        h.decoded_facts = [&](gige_mel::backend::aravis_profile::NativeFrameFacts&) {
            decoded = true;
            const auto end = Clock::now() + 10s;
            while (!resume && Clock::now() < end) std::this_thread::yield();
        };
        return h;
    }());
    (void)sim.discover(*camera);
    auto device = open_configured(sim, *camera);
    auto signal = observe_loss(*device);
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(160));
    std::optional<backend::Error> error;
    std::thread receiver([&] { auto r = stream->receive(60s); if (!r) error = r.error(); });
    {
        ReceiverCleanup cleanup{*stream, receiver};
        // This guard runs before ReceiverCleanup on assertion failure.
        struct Resume { std::atomic<bool>& flag; ~Resume() { flag = true; } } guard{resume};
        wait_until([&] { return decoded.load(); }, 5s, "real popped frame decode");
        sim.revoke_control();
        wait_until([&] { return signal->count.load() == 1; }, 10s, "SDK signal before lease commit");
        resume = true;
    }
    CHECK(error && contains(error->message, "Aravis reported camera control lost"));
    CHECK(error->code == backend::ErrorCode::disconnected);
    CHECK(stream->statistics().delivered_frames == 0 && stream->statistics().timeout_count == 0);
    require_control_loss(device->close());
    sim.reset_after_control_loss(); sim.finish();
}

// Q6. The native heartbeat exists before profile validation or DeviceCore.
// A loss in that interval must not be missed or returned as a healthy open.
void control_loss_open() {
    lb::Simulator sim;
    Observer seen;
    auto signal = std::make_shared<LossSignal>();
    auto signal_data = std::make_unique<std::shared_ptr<LossSignal>>(signal); // before heartbeat thread exists
    auto hooks = seen.hooks();
    const auto opened = hooks.after_native_open;
    hooks.after_native_open = [&](ArvDevice* d) {
        opened(d);
        g_signal_connect_data(d, "control-lost", G_CALLBACK(LossSignal::notified),
                              signal_data.release(), LossSignal::destroyed, G_CONNECT_DEFAULT);
        sim.revoke_control();
        wait_until([&] { return signal->count.load() == 1; }, 10s, "SDK event during open");
    };
    auto camera = aravis::make_backend_for_interface({"GigEVision", &arv_gv_interface_get_instance}, hooks);
    (void)sim.discover(*camera);
    require_control_loss(camera->open(device_id));
    CHECK(seen.devices.count == 1 && seen.live_devices() == 0 && seen.streams_created == 0);
    CHECK(sim.control_privilege() == 0);
    // Failed-open cleanup released the reservation after native finalization.
    auto fresh_camera = aravis::make_backend();
    auto fresh = open_configured(sim, *fresh_camera);
    CHECK(fresh->close());
    sim.finish();
}

std::string temp_path(const char* stem, const char* extension) {
    const char* dir = std::getenv("TMPDIR");
    return std::string(dir && *dir ? dir : "/tmp") + "/" + stem + "-" + std::to_string(::getpid()) + extension;
}
bool exists(const std::string& path) { return std::ifstream(path).good(); }
std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
}
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
// The diagnostics write nothing to the camera: arrange `c` over GVCP in a
// separate session, then close it so the routine opens the device itself.
void arrange_configuration(lb::Simulator& sim, const backend::CameraConfiguration& c) {
    auto camera = aravis::make_backend();
    auto device = open_configured(sim, *camera, c);
    CHECK(device->close());
    CHECK(sim.control_privilege() == 0);
}

// E1. The PRODUCTION capture_one() routine over GVCP/GVSP (no test logic).
void direct_capture() {
    lb::Simulator sim;
    auto camera = aravis::make_backend();
    (void)sim.discover(*camera);
    arrange_configuration(sim, oracle);
    const auto path = temp_path("gige-mel-loopback-capture", ".pgm");
    std::remove(path.c_str());
    const auto outcome = aravis::capture_one(*camera, device_id, path);
    if (!outcome.ok())
        throw std::runtime_error("capture_one failed: " + (outcome.error ? outcome.error->message : std::string()) +
                                 (outcome.cleanup_error ? " / " + outcome.cleanup_error->message : std::string()));
    const auto& r = *outcome.report;
    CHECK(r.device_id == device_id && r.configuration == oracle && !r.timestamp_valid);
    CHECK(r.width == oracle.width && r.height == oracle.height && r.source_row_stride == oracle.width);
    CHECK(r.native_frame_id >= 1 && r.native_frame_id <= 65535 && r.stream_generation != 0);
    CHECK(r.statistics.delivered_frames == 1 && r.statistics.incomplete_frames == 0);
    // Independent PGM validation: exact P5 header, size and EVERY pixel.
    const auto bytes = read_file(path);
    const std::string header = "P5\n64 48\n255\n";
    CHECK(bytes.size() == header.size() + std::size_t{oracle.width} * oracle.height && r.pgm_bytes == bytes.size());
    CHECK(bytes.compare(0, header.size(), header) == 0);
    for (std::uint32_t y = 0; y < oracle.height; ++y)
        for (std::uint32_t x = 0; x < oracle.width; ++x)
            CHECK(static_cast<unsigned char>(bytes[header.size() + std::size_t{y} * oracle.width + x]) ==
                  expected_pixel(x, y, r.native_frame_id, oracle.exposure));
    // Clean retirement: stopped, closed, reservation released, reopenable.
    CHECK(sim.acquisition_status() == 0 && sim.control_privilege() == 0 && sim.stream_port() == 0);
    auto reopened = open_device(*camera);
    CHECK(reopened->close());
    std::cout << "  capture_one: " << path << " (" << bytes.size() << " bytes), native frame " << r.native_frame_id
              << ", every pixel matches the oracle\n";
    std::remove(path.c_str());
    reopened.reset();
    sim.finish();
}

// E2. The PRODUCTION time_probe() routine over GVCP/GVSP (no test logic).
void time_probe_gige() {
    lb::Simulator sim;
    auto camera = aravis::make_backend();
    (void)sim.discover(*camera);
    arrange_configuration(sim, oracle);
    const auto path = temp_path("gige-mel-loopback-time-probe", ".jsonl");
    std::remove(path.c_str());
    // Exclusive creation: an existing file is never overwritten, and no
    // device I/O happens (the simulator sees no controller).
    { std::ofstream(path) << "keep"; }
    const auto refused = aravis::time_probe(*camera, device_id, 4, path);
    CHECK(!refused.ok() && refused.error && refused.error->code == backend::ErrorCode::io_error);
    CHECK(read_file(path) == "keep" && sim.control_privilege() == 0);
    std::remove(path.c_str());

    constexpr std::uint32_t count = 6;
    const auto outcome = aravis::time_probe(*camera, device_id, count, path);
    if (!outcome.ok())
        throw std::runtime_error("time_probe failed: " + (outcome.error ? outcome.error->message : std::string()) +
                                 (outcome.cleanup_error ? " / " + outcome.cleanup_error->message : std::string()));
    CHECK(sim.acquisition_status() == 0 && sim.control_privilege() == 0 && sim.stream_port() == 0);
    const auto& r = *outcome.report;
    CHECK(r.frames.size() == count && r.output_lines == count + 2 && r.configuration == oracle);
    const auto text = read_file(path);
    const auto records = parse_jsonl(text);
    CHECK(records.size() == count + 2);
    const auto& h = records.front();
    CHECK(h.at("record") == "header" && h.at("schema") == "gige-mel.aravis-time-probe" && h.at("schema_version") == 2);
    CHECK(h.at("device_id") == device_id && h.at("interface") == "gige_vision");
    CHECK(h.at("native_device_type") == "ArvGvDevice");
    CHECK(h.at("compiled_sdk_version") == "0.8.36" && h.at("runtime_sdk_version") == "0.8.36");
    const auto& id = h.at("identity");
    CHECK(id.at("DeviceVendorName") == "Aravis" && id.at("DeviceModelName") == "Fake" && id.at("DeviceID") == "GVTest");
    // GenICam XML provenance: the client's description equals what the
    // simulator served over GVCP (byte count and SHA-256).
    CHECK(id.at("GenICamXmlBytes") == std::to_string(sim.genicam_xml_bytes()));
    CHECK(id.at("GenICamXmlSha256") == sim.genicam_xml_sha256());
    // The diagnostic's OWN frequency read and its stated source; equal to
    // the simulator's bootstrap value, which still does not prove what the
    // ArvGvStream constructor observed (GIGE-MEL-010-R1).
    CHECK(h.at("diagnostic_tick_frequency_hz") == sim.tick_frequency_hz() && sim.tick_frequency_hz() == 1000000000u);
    CHECK(contains(h.at("diagnostic_tick_frequency_read").get<std::string>(), "separate read by this diagnostic"));
    CHECK(h.at("actual_stream_timestamp_path") == "unverified");
    CHECK(contains(h.at("actual_stream_timestamp_path_basis").get<std::string>(), "not established"));
    CHECK(h.at("sdk_stream_timestamp_paths").is_object());
    CHECK(contains(h.at("sdk_buffer_timestamp_source").get<std::string>(), "unverified"));
    CHECK(contains(h.at("timing_origin").get<std::string>(), "unverified"));
    CHECK(h.at("timestamp_qualified") == false);
    CHECK(h.at("configuration").at("width") == oracle.width && h.at("configuration").at("exposure_ns") == 10000000);
    std::uint64_t previous = 0, generation = 0;
    for (std::size_t i = 1; i <= count; ++i) {
        const auto& f = records[i];
        CHECK(f.at("record") == "frame" && f.at("index") == i - 1 && f.at("interface") == "gige_vision");
        CHECK(f.at("timestamp_qualified") == false && f.at("descriptor_timestamp_valid") == false);
        CHECK(f.at("actual_stream_timestamp_path") == "unverified");
        CHECK(f.at("width") == oracle.width && f.at("height") == oracle.height);
        CHECK(f.at("offset_x") == oracle.offset_x && f.at("offset_y") == oracle.offset_y);
        // Raw unsigned SDK values, unconverted, equal to the routine's report.
        CHECK(f.at("sdk_buffer_timestamp_ns").is_number_unsigned() && f.at("sdk_system_timestamp_ns").is_number_unsigned());
        const auto fid = f.at("native_frame_id").get<std::uint64_t>();
        CHECK(fid == r.frames[i - 1].native.native_frame_id);
        CHECK(f.at("sdk_buffer_timestamp_ns").get<std::uint64_t>() == r.frames[i - 1].native.sdk_buffer_timestamp_ns);
        CHECK(f.at("sdk_system_timestamp_ns").get<std::uint64_t>() == r.frames[i - 1].native.sdk_system_timestamp_ns);
        CHECK(!r.frames[i - 1].descriptor_timestamp_valid);
        if (i == 1) generation = f.at("stream_generation").get<std::uint64_t>();
        CHECK(f.at("stream_generation") == generation && generation != 0);
        if (i > 1) CHECK(id_distance(previous, fid) >= 1 && id_distance(previous, fid) <= 4);
        previous = fid;
    }
    const auto& s = records.back();
    CHECK(s.at("record") == "summary" && s.at("frames") == count && s.at("delivered_frames") == count);
    CHECK(s.at("timestamp_qualified") == false);
    const auto first = text.find('\n');
    std::cout << "  time-probe frame:  " << text.substr(first + 1, text.find('\n', first + 1) - first - 1) << '\n';
    std::remove(path.c_str());
    CHECK(!exists(path));
    sim.finish();
}

// ===========================================================================
// M. MEL Image publication of NETWORK pixels through the unchanged provider.

template <class T> std::shared_ptr<T> response(mel::RequestFor<T> future) {
    CHECK(future.wait_for(0ms) == std::future_status::ready);
    auto result = future.get();
    CHECK(result && result.get());
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

// One MEL Image callback, checked INSIDE the callback while the destination
// is still exclusively the consumer's and the source SDK lease is held.
struct Delivery {
    ir::FrameHeader header;
    std::shared_ptr<ir::Buffer> buffer;
    std::int64_t context{0};
    void* address{nullptr};
    std::vector<std::byte> bytes;
    std::optional<vc::SourceFrame> source;
    bool registered{false}, matches_sdk{false}, matches_oracle{false}, source_lease_held{false}, stale{false};
};

class Listener final : public ir::ImageListener {
public:
    std::map<const void*, std::int64_t> registered; // set before enable; read-only afterwards
    std::shared_ptr<vc::Ledger> ledger;
    std::atomic<bool> auto_release{false};
    std::atomic<std::uint64_t> closed_run{0};

    void onImage(const ir::Channel&, const ir::FrameHeader& header, std::shared_ptr<ir::Buffer> buffer) override {
        Delivery d;
        d.header = header;
        d.buffer = buffer;
        if (buffer) inspect(d, header, *buffer);
        { std::lock_guard lock(mutex_); deliveries_.push_back(d); }
        changed_.notify_all();
        if (auto_release && buffer) (void)buffer->release();
    }
    std::size_t count() const { std::lock_guard lock(mutex_); return deliveries_.size(); }
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
        d.source = ledger->by_frame_id(header.getFrameID()); // 16-bit simulator IDs fit the header
        if (!d.source) return;
        const auto& s = *d.source;
        d.stale = s.run <= closed_run.load();
        const auto& od = s.original;
        d.source_lease_held = ledger->outstanding() >= 1;
        bool sdk = d.source_lease_held && s.original_data && od.width == header.getWidth() &&
                   od.height == header.getHeight() && s.original_size >= od.row_stride_bytes * od.height;
        bool good = sdk;
        for (std::uint32_t y = 0; y < od.height && (sdk || good); ++y)
            for (std::uint32_t x = 0; x < od.width; ++x) {
                const auto actual = d.bytes[std::size_t{y} * od.width + x];
                if (sdk && actual != s.original_data[std::size_t{y} * od.row_stride_bytes + x]) sdk = false;
                if (std::to_integer<unsigned>(actual) != expected_pixel(x, y, od.native_frame_id, od.configuration.exposure))
                    good = false;
            }
        d.matches_sdk = sdk;
        d.matches_oracle = good;
    }
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<Delivery> deliveries_;
};

// One MEL session whose backend is the TEST-ONLY virtual-clock decorator
// around the PRODUCTION aravis::make_backend() (GigE Vision). with_clock
// false: the same decorator as a pure pass-through (negative control).
// Members are declared so storage, listener and observers outlive the provider.
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
    std::vector<std::shared_ptr<ir::Buffer>> buffers;

    explicit Session(bool with_clock) {
        storage.reserve(16); // stable addresses: never reallocated after a registration
        if (with_clock) clock = std::make_shared<vc::Clock>();
        listener->ledger = ledger;
        decorated = std::make_shared<vc::Backend>(aravis::make_backend(), clock, ledger);
        settings.instance = "aravis-gige-loopback-test";
        settings.device_id = device_id; // the exact ID discovery reported
        settings.maximum_registered_buffers = 4;
        settings.horizontal_radians_per_pixel = h_rad_per_px;     // VIRTUAL test optics
        settings.vertical_radians_per_pixel = v_rad_per_px;       // VIRTUAL test optics
        settings.timestamp_domain = backend::ClockDomain::device; // the declared virtual domain
        manager = gige_mel::make_api_manager(settings, decorated);
        CHECK(manager);
        control = ir::getControl(settings.instance, manager);
        CHECK(control && control->init() == ir::Return::Success); // GVCP open of the simulator
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
    // detach, then drop the manager so the GigE device is closed before the
    // simulator is finished.
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
    void configure(lb::Simulator& sim, std::uint32_t id, const backend::CameraConfiguration& c) {
        const auto reply = response(image->send(camera_command(id, c)));
        CHECK(reply->getCommandSuccess());
        CHECK(inner()->configuration().value() == c); // fresh GVCP readback
        CHECK(native_type(*inner()) == "ArvGvDevice");
        CHECK(native_integer(*inner(), "GainRaw") == 0 && sim.read_register(reg_width) == c.width);
        CHECK(std::int64_t{sim.read_register(reg_exposure_us)} * 1000 == c.exposure.count());
    }
    void add_buffer(std::size_t bytes, std::int64_t context) {
        CHECK(storage.size() < 16);
        storage.emplace_back(bytes, sentinel);
        auto buffer = ir::getBuffer(settings.instance, manager);
        CHECK(buffer && buffer->init(storage.back().data(), bytes, context) == ir::Return::Success);
        CHECK(image->registerBuffer(buffer) == ir::Return::Success);
        listener->registered[storage.back().data()] = context;
        buffers.push_back(buffer);
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

// M1. Positive publication of NETWORK-ORIGINATED pixels under the TEST-ONLY
// virtual clock; held destinations, explicit release, disable, no stale
// publication. This proves the network-image publication path, not timing.
void positive_publication_in(lb::Simulator& sim);
void positive_publication() {
    lb::Simulator sim;
    (void)sim.discover(*aravis::make_backend());
    positive_publication_in(sim); // the MEL session (and its GigE device) ends here
    sim.finish();
}
void positive_publication_in(lb::Simulator& sim) {
    Session s(true);
    const vc::Model model{std::chrono::seconds{5000}, 40ms, 9}; // TEST MODEL values
    s.clock->set_model(model);
    s.configure(sim, 11001, oracle);
    const std::size_t bytes = std::size_t{oracle.width} * oracle.height;
    for (int i = 0; i < 4; ++i) s.add_buffer(bytes, 1101 + i);
    for (const auto& block : s.storage) CHECK(all_sentinel(block));
    CHECK(s.image->enable() == ir::Return::Success);
    CHECK(eventually([&] { return sim.acquisition_status() == 1; })); // GVCP AcquisitionStart
    s.listener->wait_count(4); // all four registered destinations now held by the consumer
    const auto held = s.listener->all();
    CHECK(held.size() == 4);
    std::set<const void*> addresses;
    std::uint64_t previous_sequence = 0;
    for (std::size_t i = 0; i < held.size(); ++i) {
        const auto& d = held[i];
        CHECK(d.buffer && d.registered && d.source && !d.stale);
        const auto& src = *d.source;
        CHECK(src.run == 1 && src.substituted);
        CHECK(unqualified(src.original.timestamp)); // the production network frame was unqualified
        CHECK(src.original.configuration == oracle && src.original.status == backend::FrameStatus::complete);
        CHECK(src.delivered_data == src.original_data && src.delivered_size == src.original_size);
        CHECK(d.source_lease_held && d.matches_sdk && d.matches_oracle); // every byte: SDK and oracle
        const auto declared = vc::nominal(model, src.sequence);
        CHECK(declared && src.delivered.timestamp.value == declared->value);
        const auto& h = d.header;
        CHECK(h.getWidth() == oracle.width && h.getHeight() == oracle.height);
        CHECK(h.getBitsPerPixel() == 8 && h.getNumBands() == 1 && h.getFormat() == ir::PixelFormat::Mono);
        CHECK(h.getColumnOffset() == oracle.offset_x && h.getRowOffset() == oracle.offset_y);
        CHECK(h.getIntegrationTime() == oracle.exposure);
        CHECK(h.getSystemTime() == declared->value); // the declared VIRTUAL value, nothing else
        CHECK(h.getFrameID() == static_cast<std::uint32_t>(src.original.native_frame_id));
        CHECK(h.getHorizontalFieldOfView() == oracle.width * h_rad_per_px);
        if (i) CHECK(src.sequence > previous_sequence);
        previous_sequence = src.sequence;
        addresses.insert(d.address);
    }
    CHECK(addresses.size() == 4);
    // More genuine network frames arrive and are dropped: the held
    // destinations are never overwritten before explicit release.
    const auto frames = s.ledger->frames();
    CHECK(s.ledger->wait_frames(frames + 4, bound));
    CHECK(eventually([&] { return s.stats().dropped_no_buffer >= 3; }));
    CHECK(s.listener->count() == 4);
    for (const auto& d : held) CHECK(region(d.address, bytes) == d.bytes);
    CHECK(s.ledger->wait_outstanding(1, bound) && s.ledger->peak_outstanding() == 1);
    // Explicit Buffer::release() of one destination resumes publication into it.
    for (const auto& b : s.buffers)
        if (b->getBufferAddress() == held.front().address) CHECK(b->release() == ir::Return::Success);
    s.listener->wait_count(5);
    const auto resumed = s.listener->all().at(4);
    CHECK(resumed.address == held.front().address && resumed.context == held.front().context);
    CHECK(resumed.matches_sdk && resumed.matches_oracle && resumed.source->sequence > previous_sequence);
    for (std::size_t i = 1; i < held.size(); ++i) CHECK(region(held[i].address, bytes) == held[i].bytes);
    // Normal disable: acquisition stopped over GVCP; no stale publication.
    CHECK(s.image->disable() == ir::Return::Success);
    s.listener->closed_run = 1;
    CHECK(sim.acquisition_status() == 0);
    const auto after = s.listener->count();
    std::this_thread::sleep_for(200ms);
    CHECK(s.listener->count() == after);
    for (const auto& d : s.listener->all()) CHECK(!d.stale && d.source && d.source->run == 1);
    const auto stats = s.stats();
    CHECK(stats.delivered_frames == after && stats.rejected_frames == 0 && stats.stream_failures == 0);
    CHECK(s.ledger->wait_outstanding(0, bound));
    std::cout << "  published " << after << " network frames (IDs " << held.front().header.getFrameID() << ".."
              << resumed.header.getFrameID() << ") with VIRTUAL systemTime " << held.front().header.getSystemTime().count()
              << " ns..; dropped " << stats.dropped_no_buffer << " while held\n";
}

// M2. NEGATIVE CONTROL: the same decorator as a pure pass-through. Genuine
// GVSP frames reach the provider with the production FrameTimestamp{} and
// are rejected; no callback, no destination write, no fabricated time.
void negative_publication_in(lb::Simulator& sim) {
    Session s(false);
    s.configure(sim, 12001, oracle);
    const std::size_t bytes = std::size_t{oracle.width} * oracle.height;
    s.add_buffer(bytes, 1201);
    s.add_buffer(bytes, 1202);
    CHECK(s.image->enable() == ir::Return::Success);
    CHECK(eventually([&] { return sim.acquisition_status() == 1; }));
    constexpr std::size_t frames = 6;
    CHECK(s.ledger->wait_frames(frames, bound)); // genuine GVSP frames received by the production backend
    CHECK(eventually([&] { return s.stats().rejected_frames >= frames; }));
    CHECK(s.image->enable() == ir::Return::Success); // still enabled: per-frame rejection, not a failure
    CHECK(s.image->disable() == ir::Return::Success);
    CHECK(sim.acquisition_status() == 0);
    const auto after = s.stats();
    CHECK(s.listener->count() == 0);
    CHECK(after.delivered_frames == 0 && after.dropped_no_buffer == 0);
    CHECK(after.stream_failures == 0 && after.callback_failures == 0 && after.rejected_frames >= frames);
    CHECK(contains(after.last_error, "timestamp contract"));
    for (const auto& block : s.storage) CHECK(all_sentinel(block)); // destinations untouched
    for (const auto& f : s.ledger->all()) {
        CHECK(!f.substituted && unqualified(f.original.timestamp) && unqualified(f.delivered.timestamp));
        CHECK(f.original.configuration == oracle && f.original.status == backend::FrameStatus::complete);
        CHECK(f.original_size == bytes);
    }
    CHECK(s.ledger->wait_outstanding(0, bound));
    std::cout << "  negative control: " << after.rejected_frames << " genuine GVSP frames rejected, 0 callbacks, "
              << "destinations unchanged; last_error: " << after.last_error << '\n';
}
void negative_publication() {
    lb::Simulator sim;
    (void)sim.discover(*aravis::make_backend());
    negative_publication_in(sim);
    sim.finish();
}

// Q4. The unchanged MEL worker already treats a terminal backend error as a
// stream failure. Positive network publication requires ONLY the existing
// test virtual clock; the production timestamp remains FrameTimestamp{}.
void control_loss_provider() {
    lb::Simulator sim;
    (void)sim.discover(*aravis::make_backend());
    {
        Session s(true);
        const vc::Model model{5000s, 40ms, 9};
        s.clock->set_model(model);
        s.configure(sim, 15001, oracle);
        const std::size_t bytes = std::size_t{oracle.width} * oracle.height;
        for (int i = 0; i < 4; ++i) s.add_buffer(bytes, 1501 + i);
        auto signal = observe_loss(*s.inner());
        CHECK(s.image->enable() == ir::Return::Success);
        s.listener->wait_count(4);
        const auto held = s.listener->all();
        for (const auto& d : held) {
            CHECK(d.registered && d.matches_sdk && d.matches_oracle && d.source_lease_held);
            CHECK(d.source && unqualified(d.source->original.timestamp));
            CHECK(d.header.getWidth() == oracle.width && d.header.getHeight() == oracle.height);
            CHECK(d.header.getBitsPerPixel() == 8 && d.header.getNumBands() == 1);
            CHECK(d.header.getColumnOffset() == oracle.offset_x && d.header.getRowOffset() == oracle.offset_y);
            CHECK(d.header.getIntegrationTime() == oracle.exposure);
            CHECK(d.header.getFrameID() == d.source->original.native_frame_id);
            CHECK(d.header.getSystemTime() == vc::nominal(model, d.source->sequence)->value);
        }
        sim.revoke_control();
        wait_until([&] { return signal->count.load() == 1; }, 10s, "provider SDK signal");
        wait_until([&] {
            const auto stats = s.stats();
            return stats.stream_failures == 1 && contains(stats.last_error, "Aravis reported camera control lost");
        }, 2s, "MEL terminal stream failure and diagnostic");
        CHECK(s.image->enable() == ir::Return::Fail); // faulted ownership, no implicit new stream
        auto refused = s.image->send(camera_command(15002, second));
        CHECK(refused.wait_for(bound) == std::future_status::ready);
        CHECK(!refused.get()); // command refused while faulted
        for (const auto& d : held) CHECK(region(d.address, bytes) == d.bytes);
        // Release one destination after failure: it must NOT invite publication.
        CHECK(held.front().buffer->release() == ir::Return::Success);
        CHECK(s.image->disable() == ir::Return::Success); // drains the worker/callbacks
        CHECK(s.listener->count() == held.size());
        for (const auto& d : held) CHECK(region(d.address, bytes) == d.bytes);
        CHECK(s.stats().stream_failures == 1 && s.stats().rejected_frames == 0);
        CHECK(s.ledger->wait_outstanding(0, bound));
        require_control_loss(s.inner()->configuration());
        require_control_loss(s.inner()->close());
    }
    sim.reset_after_control_loss();
    sim.finish();
}

// ===========================================================================
// L. SIMULATED packet loss: a fresh simulator with the pinned construct
// property gvsp-lost-ratio (0.0: normal delivery; 1.0: the simulator drops
// every outgoing GVSP packet before sendto). Not a switch or NIC loss test;
// a timeout here proves nothing about disconnection.
void simulated_loss(double ratio) {
    lb::Simulator sim(lb::Options{ratio});
    Observer seen;
    StreamView view;
    auto hooks = seen.hooks();
    auto created_hook = hooks.after_stream_created;
    hooks.after_stream_created = [&](ArvStream* st) { created_hook(st); view.set(st); };
    auto camera = aravis::make_backend_for_interface({"GigEVision", &arv_gv_interface_get_instance}, hooks);
    (void)sim.discover(*camera); // GVCP is unaffected by the GVSP loss ratio
    auto device = open_configured(sim, *camera);
    auto signal = observe_loss(*device);
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(90));
    CHECK(sim.acquisition_status() == 1 && sim.stream_port() != 0);
    if (ratio == 0.0) {
        for (int i = 0; i < 4; ++i) { auto frame = receive_ok(*stream); check_frame(frame, oracle, 90); }
        const auto packets = view.info("n_received_packets");
        CHECK(packets > 0);
        std::cout << "  gvsp-lost-ratio 0.0: 4 complete frames, " << packets << " GVSP packets received\n";
    } else {
        const auto begin = Clock::now();
        for (int i = 0; i < 3; ++i) { // the simulator keeps "sending" at 25 Hz
            const auto t0 = Clock::now();
            const auto result = stream->receive(400ms);
            const auto took = Clock::now() - t0;
            CHECK(!result && result.error().code == backend::ErrorCode::timeout);
            CHECK(took >= 400ms && took < 2s);
        }
        CHECK(sim.acquisition_status() == 1); // the server never stopped
        CHECK(view.info("n_received_packets") == 0 && view.info("n_completed_buffers") == 0);
        const auto stats = stream->statistics();
        CHECK(stats.delivered_frames == 0 && stats.incomplete_frames == 0 && stats.timeout_count == 3);
        CHECK(seen.buffers_allocated == 4 && seen.live_buffers() == 4); // no buffer growth
        // A blocked receive under complete loss is cancelled within the bound.
        std::atomic<bool> finished{false};
        std::optional<backend::Error> outcome;
        std::thread receiver;
        {
            ReceiverCleanup cleanup{*stream, receiver};
            const int waits = seen.native_waits.load();
            receiver = std::thread([&] {
                auto r = stream->receive(60s);
                if (!r) outcome = r.error();
                finished = true;
            });
            wait_until([&] { return seen.native_waits.load() >= waits + 3; }, 5s, "receive to wait under loss");
            const auto cancelled = Clock::now();
            stream->request_stop();
            wait_until([&] { return finished.load(); }, 2s, "cancelled receive under loss");
            CHECK(Clock::now() - cancelled < 2s);
        }
        CHECK(outcome && outcome->code == backend::ErrorCode::stopped);
        std::cout << "  gvsp-lost-ratio 1.0: 0 packets and 0 frames received; 3 bounded timeouts and a prompt "
                  << "cancel in " << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count()
                  << " ms; pool stayed at 4 buffers\n";
    }
    CHECK(signal->count == 0 && sim.control_privilege() != 0);
    CHECK(device->configuration()); // packet loss alone never latches terminal failure
    stream->stop();
    CHECK(sim.acquisition_status() == 0);
    CHECK(device->close());
    CHECK(seen.live_buffers() == 0 && seen.live_streams() == 0 && seen.live_devices() == 0);
    stream.reset();
    device.reset();
    sim.finish();
}
// Complete loss through the MEL provider: no publication, no fabricated frame.
void loss_through_provider(lb::Simulator& sim) {
    Session s(true); // even WITH the virtual clock nothing can be published
    s.configure(sim, 13001, oracle);
    s.add_buffer(std::size_t{oracle.width} * oracle.height, 1301);
    CHECK(s.image->enable() == ir::Return::Success);
    CHECK(eventually([&] { return sim.acquisition_status() == 1; }));
    std::this_thread::sleep_for(500ms); // ~12 transmitted (and dropped) frames at 25 Hz
    CHECK(s.image->disable() == ir::Return::Success);
    const auto stats = s.stats();
    CHECK(s.listener->count() == 0 && s.ledger->frames() == 0);
    CHECK(stats.delivered_frames == 0 && stats.rejected_frames == 0 && stats.stream_failures == 0);
    for (const auto& block : s.storage) CHECK(all_sentinel(block));
}
void packet_loss() {
    simulated_loss(0.0);
    simulated_loss(1.0);
    lb::Simulator sim(lb::Options{1.0});
    (void)sim.discover(*aravis::make_backend());
    loss_through_provider(sim);
    sim.finish();
}

// ===========================================================================
// S. Socket-audit controls (GIGE-MEL-011-R1). Not a transport scenario. The
// audit permits ONE socket that is not loopback-bound: the pinned
// simulator's 255.255.255.255:3956 global-discovery listener, identified by
// its recorded inode/fd as well as its address, port and options. Any other
// wildcard socket is rejected. No datagram is sent by this case and no host
// firewall or network setting is touched.
class TemporaryUdpSocket {
public:
    TemporaryUdpSocket(const char* address, std::uint16_t port, bool reuse) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) throw std::runtime_error("socket() failed");
        if (reuse) {
            const int one = 1;
            ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
        }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (::inet_pton(AF_INET, address, &a.sin_addr) != 1 ||
            ::bind(fd_, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
            const int error = errno;
            ::close(fd_);
            throw std::runtime_error(std::string("cannot bind temporary control socket ") + address + ": " +
                                     std::strerror(error));
        }
        struct stat st{};
        socklen_t length = sizeof a;
        CHECK(::fstat(fd_, &st) == 0 && ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &length) == 0);
        inode_ = st.st_ino;
        port_ = ntohs(a.sin_port);
    }
    ~TemporaryUdpSocket() { close(); }
    TemporaryUdpSocket(const TemporaryUdpSocket&) = delete;
    TemporaryUdpSocket& operator=(const TemporaryUdpSocket&) = delete;
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    std::uint64_t inode() const { return inode_; }
    std::uint16_t port() const { return port_; }

private:
    int fd_{-1};
    std::uint64_t inode_{0};
    std::uint16_t port_{0};
};

// The audit must reject exactly `extra` and still recognize the simulator's
// listener; the checked form must throw; closing `extra` restores a pass.
void require_rejected(const lb::Simulator& sim, TemporaryUdpSocket& extra, const char* label) {
    const auto rejected = sim.socket_audit(label);
    CHECK(!rejected.passed());
    CHECK(rejected.unexpected.size() == 1 && rejected.unexpected.front().inode == extra.inode());
    CHECK(rejected.discovery.size() == 1 && rejected.discovery.front().inode == sim.sockets().global_discovery.inode);
    bool threw = false;
    try {
        (void)sim.require_socket_audit(label);
    } catch (const std::runtime_error& error) {
        threw = contains(error.what(), "unexpected 1") && contains(error.what(), "FAIL");
    }
    CHECK(threw);
    std::cout << "  rejected: " << rejected.summary() << '\n';
    extra.close();
    CHECK(audit_live(sim, std::string(label) + ", after closing it").unexpected.empty());
}

void socket_audit_controls() {
    lb::Simulator sim;
    const auto& listener = sim.sockets().global_discovery;
    const auto normal = audit_live(sim, "audit control: simulator only");
    std::cout << "  pinned simulator sockets: GVCP " << sim.sockets().gvcp.text() << "; GVSP output "
              << sim.sockets().gvsp_output.text() << "; global discovery " << listener.text()
              << "; lo broadcast " << sim.sockets().loopback_broadcast << " (no subnet-discovery socket)\n";
    // Recognized by its documented identity, never as loopback and never
    // without that identity.
    CHECK(lb::classify_udp_socket(listener, &listener) == lb::SocketClass::simulator_global_discovery);
    CHECK(lb::classify_udp_socket(listener, nullptr) == lb::SocketClass::unexpected);
    for (const auto& s : normal.loopback) CHECK(s.inode != listener.inode);
    auto variant = [&](auto change) {
        auto forged = listener;
        change(forged);
        return lb::classify_udp_socket(forged, &listener);
    };
    CHECK(variant([](lb::UdpSocket& s) { s.address = "0.0.0.0"; }) == lb::SocketClass::unexpected);
    CHECK(variant([](lb::UdpSocket& s) { s.port = 3957; }) == lb::SocketClass::unexpected);
    CHECK(variant([](lb::UdpSocket& s) { s.inode += 1; }) == lb::SocketClass::unexpected);
    CHECK(variant([](lb::UdpSocket& s) { s.reuse_port = false; }) == lb::SocketClass::unexpected);
    CHECK(variant([](lb::UdpSocket& s) { s.bound_device = "lo"; }) == lb::SocketClass::unexpected);
    CHECK(variant([](lb::UdpSocket& s) { s.datagram_udp = false; }) == lb::SocketClass::unexpected);
    CHECK(variant([](lb::UdpSocket& s) { s.ipv6 = true; }) == lb::SocketClass::unexpected);

    // Negative controls: real, temporary, unrelated wildcard UDP sockets of
    // THIS process on kernel-assigned ephemeral ports, one at a time.
    {
        TemporaryUdpSocket any("0.0.0.0", 0, false);
        CHECK(any.port() != 0 && any.port() != lb::gvcp_port);
        require_rejected(sim, any, "audit control: extra 0.0.0.0 ephemeral socket");
    }
    {
        TemporaryUdpSocket broadcast("255.255.255.255", 0, true);
        CHECK(broadcast.port() != 0 && broadcast.port() != lb::gvcp_port);
        require_rejected(sim, broadcast, "audit control: extra 255.255.255.255 ephemeral socket");
    }
    // A lookalike with the listener's exact address, port and options (the
    // pinned reuse flags allow it to coexist) is still rejected: the
    // exception is the simulator's recorded socket, not a socket pattern.
    {
        TemporaryUdpSocket lookalike("255.255.255.255", lb::gvcp_port, true);
        require_rejected(sim, lookalike, "audit control: lookalike 255.255.255.255:3956 socket");
    }
    CHECK(audit_live(sim, "audit control: simulator only, again").loopback.size() == normal.loopback.size());
    sim.finish();
    const auto& released = sim.release_audit();
    CHECK(released && released->passed() && released->discovery.empty() && released->unexpected.empty());
    CHECK(lb::probe_udp_bind(lb::gvcp_port) == 0 && lb::probe_udp_bind("255.255.255.255", lb::gvcp_port) == 0);
    std::cout << "  socket audit " << released->summary() << '\n';
}

// Any GLib/Aravis warning or critical (e.g. a simulator "Failed to bind" or a
// g_return_if_fail reached by the adapter) aborts: failures, never noise.
void fatal_glib_log(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer) {
    std::cerr << "FAIL GLib " << ((level & G_LOG_LEVEL_CRITICAL) ? "critical" : "warning") << " ["
              << (domain ? domain : "") << "]: " << (message ? message : "") << '\n';
    std::abort();
}

// Bounded watchdog per scenario: a hang is a failure, never a pass. It runs on
// its own thread and never joins or cancels the test thread.
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
              << ") loaded from " << lb::loaded_sdk_library() << "; adapter make_backend() linked into "
              << lb::adapter_symbol_object() << " (static archive)\n"
              << "Server: in-process ArvGvFakeCamera on " << lb::loopback_address << ":" << lb::gvcp_port
              << " (simulated network camera; GVCP/GVSP over loopback UDP). Client: production aravis::make_backend() "
              << "(GigE Vision). TEST-ONLY virtual clock for MEL publication; VIRTUAL optics. No physical camera.\n";
    if (aravis::runtime_version() != "0.8.36" || aravis::compiled_version() != "0.8.36") {
        std::cerr << "FAIL: the qualified Aravis 0.8.36 library is not the one loaded\n";
        return 1;
    }
    g_log_set_always_fatal(static_cast<GLogLevelFlags>(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));
    for (const char* domain : {static_cast<const char*>(nullptr), "Aravis", "GLib", "GLib-GObject", "GLib-GIO"})
        g_log_set_handler(domain, static_cast<GLogLevelFlags>(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL),
                          fatal_glib_log, nullptr);
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"A  GigE discovery, exact identity and device ownership (GVCP)", discovery_and_ownership},
        {"B  configuration through real GVCP", configuration_over_gvcp},
        {"C  real GVSP Mono8 transfer through ArvGvStream (every pixel)", gvsp_frame_transfer},
        {"D1 GVCP AcquisitionStart/Stop, restart, lease lifetime and reopen", acquisition_and_lifetime},
        {"D2 bounded cancellation of a blocked network receive", cancellation},
        {"E1 production capture_one() over GigE", direct_capture},
        {"E2 production time_probe() over GigE (JSONL)", time_probe_gige},
        {"M1 positive MEL publication of network pixels (TEST-ONLY virtual time)", positive_publication},
        {"M2 negative control: production timestamps rejected", negative_publication},
        {"L  SIMULATED GVSP packet loss (gvsp-lost-ratio 0.0 / 1.0)", packet_loss},
        {"Q1 confirmed control loss: blocked receive, sticky refusal and explicit recovery", control_loss_active},
        {"Q2 confirmed control loss: no writes/streams, exact source lease cleanup", control_loss_lifetime},
        {"Q3 confirmed control loss: concurrent close and actual SDK emission", control_loss_close_overlap},
        {"Q4 confirmed control loss: MEL publication failure and destination ownership", control_loss_provider},
        {"Q5 confirmed control loss: popped frame refused at admission", control_loss_admission},
        {"Q6 confirmed control loss: validation-time open refusal and cleanup", control_loss_open},
        // Not a transport scenario: the R1 socket-audit controls.
        {"S  socket audit: documented wildcard discovery listener; unrelated wildcard rejected", socket_audit_controls},
    };
    // Usage: aravis_gige_loopback_contract_tests [FILTER [REPEAT]]
    const std::string filter = argc >= 2 ? argv[1] : "";
    const int repeat = argc >= 3 ? std::max(1, std::atoi(argv[2])) : 1;
    std::size_t passed = 0, audit_cases = 0;
    for (int r = 0; r < repeat; ++r)
        for (const auto& [name, run] : tests) {
            if (!filter.empty() && std::string(name).find(filter) == std::string::npos) continue;
            try {
                Watchdog watchdog(name, std::chrono::seconds{90});
                run();
                // Every simulator is destroyed: no wildcard UDP socket of any
                // kind may remain (no exception is permitted here).
                (void)lb::require_socket_audit(std::string(name) + ": after the scenario", nullptr);
                if (lb::probe_udp_bind(lb::gvcp_port) != 0)
                    throw std::runtime_error("UDP 127.0.0.1:3956 still bound after the scenario");
                if (lb::probe_udp_bind("255.255.255.255", lb::gvcp_port) != 0)
                    throw std::runtime_error("UDP 255.255.255.255:3956 still bound after the scenario");
            } catch (const std::exception& error) {
                std::cerr << "FAIL " << name << ": " << error.what() << '\n';
                return 1;
            }
            ++passed;
            if (run == socket_audit_controls) ++audit_cases;
            std::cout << "PASS " << name << std::endl;
        }
    std::cout << passed - audit_cases << " Aravis GigE loopback transport scenarios and " << audit_cases
              << " socket-audit control case(s) passed (simulated network camera; no physical camera, NIC, exposure "
              << "timing or timestamp qualification; the simulator's 255.255.255.255:3956 discovery listener is a "
              << "documented test-only wildcard exception, NOT loopback confinement)\n";
    return passed ? 0 : 1;
}
