// Acquisition contract tests (GIGE-MEL-009) against the REAL Aravis 0.8.36
// library and its in-process Fake device: genuine ArvStream receiver threads,
// genuine SDK buffers and the SDK's own Fake pixel generator. Private hooks
// only observe native events or arrange native state through the same SDK.
// The frame-facts cases in invalid_native_facts() are explicitly labelled:
// they feed decoded facts to the SAME production validator, because the
// pinned Fake cannot emit those payloads. No project-generated frame replaces
// positive streaming. GLib warnings/criticals are fatal (see main()).
#include "aravis_fake_support.hpp"
#include "camera_profile.hpp"

#include <arv.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace backend = gige_mel::backend;
namespace aravis = gige_mel::backend::aravis;
namespace profile = gige_mel::backend::aravis_profile;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression)) {                                                                         \
            throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) +           \
                                     ": " #expression);                                              \
        }                                                                                            \
    } while (false)

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

// Oracle configuration: 10 000 us exposure and GainRaw 0 make the pinned
// Fake's diagonal ramp scale exactly 1.0, so pixel(x, y) = (x + y + id) % 255.
const backend::CameraConfiguration oracle{64, 48, 8, 4, 100, 10ms};

// Bounded wait for an observable condition; failure is a test failure.
template <class Predicate> void wait_until(Predicate predicate, std::chrono::milliseconds bound, const char* what) {
    const auto deadline = Clock::now() + bound;
    while (!predicate()) {
        if (Clock::now() >= deadline) throw std::runtime_error(std::string("timed out waiting for ") + what);
        std::this_thread::sleep_for(1ms);
    }
}

// GObject weak-reference counter; never keeps the observed object alive.
struct Finalized {
    std::atomic<int> count{0};
    static void notify(gpointer data, GObject*) { static_cast<Finalized*>(data)->count.fetch_add(1); }
    void watch(gpointer object) { g_object_weak_ref(G_OBJECT(object), &Finalized::notify, this); }
};

// Native observations shared by hooks. Declared before any backend/device so
// it outlives every activity it observes.
struct Observer {
    Finalized streams, buffers, devices;
    std::atomic<int> streams_created{0}, buffers_allocated{0}, devices_opened{0};
    std::atomic<int> native_waits{0};
    std::mutex mutex;
    std::vector<std::string> commands;
    std::function<void(std::string_view, ArvDevice*)> on_command; // arrangement, set per test
    std::function<void()> on_wait;                                // observation, set per test
    aravis::TestHooks hooks() {
        aravis::TestHooks h;
        h.after_native_open = [this](ArvDevice* d) { devices_opened.fetch_add(1); devices.watch(d); };
        h.after_stream_created = [this](ArvStream* s) { streams_created.fetch_add(1); streams.watch(s); };
        h.after_buffer_allocated = [this](ArvBuffer* b) { buffers_allocated.fetch_add(1); buffers.watch(b); };
        h.before_command = [this](std::string_view c, ArvDevice* d) {
            { std::lock_guard lock(mutex); commands.emplace_back(c); }
            if (on_command) on_command(c, d);
        };
        h.before_native_wait = [this] {
            native_waits.fetch_add(1);
            if (on_wait) on_wait();
        };
        return h;
    }
    std::vector<std::string> command_log() { std::lock_guard lock(mutex); return commands; }
    int live_streams() const { return streams_created.load() - streams.count.load(); }
    int live_buffers() const { return buffers_allocated.load() - buffers.count.load(); }
};

std::shared_ptr<backend::ICameraDevice> open_fake(backend::ICameraBackend& camera) {
    auto opened = camera.open(aravis_fake::device_id);
    if (!opened) throw std::runtime_error("open Fake_1 failed: " + opened.error().message);
    return std::move(opened).value();
}

void native(backend::ICameraDevice& device, const std::function<void(ArvDevice*)>& fn) {
    CHECK(aravis::with_native_device_for_testing(device, fn));
}

// The Fake's acquisition register (written only by AcquisitionStart/Stop),
// read through the SDK's public Fake-camera accessor: command evidence that
// is independent of frame delivery.
guint32 acquisition_register(backend::ICameraDevice& device) {
    guint32 value = 0xffffffffu;
    native(device, [&](ArvDevice* d) {
        value = arv_fake_camera_get_acquisition_status(arv_fake_device_get_fake_camera(ARV_FAKE_DEVICE(d)));
    });
    return value;
}

std::int64_t native_integer(backend::ICameraDevice& device, const char* feature) {
    std::int64_t value = -1;
    native(device, [&](ArvDevice* d) {
        GError* error = nullptr;
        value = arv_device_get_integer_feature_value(d, feature, &error);
        CHECK(!error);
    });
    return value;
}

// Applies the oracle configuration and proves it by actual readback, then
// proves the remaining oracle facts (GainRaw 0, Mono8) through the SDK.
std::shared_ptr<backend::ICameraDevice> open_oracle(backend::ICameraBackend& camera) {
    auto device = open_fake(camera);
    const auto applied = device->apply(oracle);
    CHECK(applied && applied.value() == oracle);
    CHECK(device->configuration().value() == oracle);
    CHECK(native_integer(*device, "GainRaw") == 0);
    native(*device, [](ArvDevice* d) {
        CHECK(std::string(arv_device_get_string_feature_value(d, "PixelFormat", nullptr)) == "Mono8");
    });
    return device;
}

// Independent oracle check of every pixel.
bool pixels_match(const backend::FrameLease& frame) {
    const auto& d = frame.descriptor();
    for (std::uint32_t y = 0; y < d.height; ++y)
        for (std::uint32_t x = 0; x < d.width; ++x) {
            const auto expected = static_cast<unsigned>((x + y + d.native_frame_id) % 255);
            const auto actual = std::to_integer<unsigned>(frame.bytes()[std::size_t{y} * d.row_stride_bytes + x]);
            if (actual != expected) return false;
        }
    return true;
}

void check_descriptor(const backend::FrameLease& frame, std::uint64_t configuration_generation) {
    const auto& d = frame.descriptor();
    CHECK(d.width == oracle.width && d.height == oracle.height && d.row_stride_bytes == oracle.width);
    CHECK(d.storage_bits_per_pixel == 8 && d.significant_bits_per_pixel == 8 && d.bands == 1);
    CHECK(d.pixel_format == backend::PixelFormat::mono8 && d.byte_order == backend::ByteOrder::not_applicable);
    CHECK(d.status == backend::FrameStatus::complete);
    CHECK(d.configuration == oracle && d.configuration_generation == configuration_generation);
    CHECK(d.stream_generation != 0 && d.native_frame_id >= 1 && d.native_frame_id <= 65535); // Fake: 16-bit, 0 invalid
    CHECK(frame.bytes().size() == std::size_t{d.row_stride_bytes} * d.height);
    // Deliberately unqualified timestamp: exactly FrameTimestamp{}.
    CHECK(!d.timestamp.valid && d.timestamp.domain == backend::ClockDomain::unspecified);
    CHECK(d.timestamp.event == backend::TimestampEvent::unspecified);
    CHECK(d.timestamp.value == std::chrono::nanoseconds{0} && d.timestamp.clock_generation == 0);
}

backend::FrameLease receive_ok(backend::IFrameStream& stream, std::chrono::milliseconds timeout = 3s) {
    auto result = stream.receive(timeout);
    if (!result) throw std::runtime_error("receive failed: " + result.error().message);
    return std::move(result).value();
}

std::vector<std::byte> copy_bytes(const backend::FrameLease& f) { return {f.bytes().begin(), f.bytes().end()}; }

// ===========================================================================
// A. Start, commands, single acquisition owner.
void start_and_commands() {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    CHECK(acquisition_register(*device) == 0);
    auto created = device->create_stream();
    CHECK(created);
    auto stream = std::move(created).value();
    // Wrapper creation: no native stream, buffer, command or acquisition.
    CHECK(seen.streams_created == 0 && seen.buffers_allocated == 0 && seen.command_log().empty());
    CHECK(acquisition_register(*device) == 0);
    // receive before start is a terminal stopped result, not a wait.
    CHECK(stream->receive(10ms).error().code == backend::ErrorCode::stopped);

    CHECK(stream->start(7));
    CHECK(seen.streams_created == 1 && seen.live_streams() == 1);
    CHECK(seen.buffers_allocated == static_cast<int>(profile::pool_buffers));
    CHECK((seen.command_log() == std::vector<std::string>{"AcquisitionStart"}));
    CHECK(acquisition_register(*device) == 1); // the SDK command reached the camera

    // Repeated and competing starts fail without another native receiver.
    CHECK(stream->start(8).error().code == backend::ErrorCode::busy);
    auto rival = std::move(device->create_stream()).value();
    const auto competing = rival->start(9);
    CHECK(!competing && competing.error().code == backend::ErrorCode::busy);
    CHECK(contains(competing.error().message, "no native stream was created"));
    CHECK(seen.streams_created == 1 && seen.command_log().size() == 1);
    // A rival wrapper never started cannot stop the owner's acquisition.
    rival->stop();
    rival->request_stop();
    CHECK(acquisition_register(*device) == 1 && seen.live_streams() == 1);

    // Configuration is rejected before any native write while active.
    const auto blocked = device->apply({32, 32, 0, 0, 100, 10ms});
    CHECK(!blocked && blocked.error().code == backend::ErrorCode::invalid_state);
    CHECK(contains(blocked.error().message, "nothing was written"));
    CHECK(device->configuration().value() == oracle);

    auto frame = receive_ok(*stream);
    check_descriptor(frame, 7);

    // Stop executes the SDK stop command and retires the native stream; the
    // register proves the command independently of any frame.
    stream->stop();
    CHECK((seen.command_log() == std::vector<std::string>{"AcquisitionStart", "AcquisitionStop"}));
    CHECK(acquisition_register(*device) == 0);
    CHECK(seen.live_streams() == 0); // finalized synchronously during stop()
    stream->stop();                  // repeated stop: no second command
    rival->stop();
    CHECK(seen.command_log().size() == 2);
    CHECK(stream->receive(50ms).error().code == backend::ErrorCode::stopped);
    CHECK(device->close());
}

// B. Genuine SDK pixels and descriptors through an actual ArvStream.
void real_pixels_and_descriptors() {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(41));
    std::uint64_t generation = 0;
    std::optional<std::uint64_t> previous_id;
    for (int i = 0; i < 6; ++i) {
        auto frame = receive_ok(*stream);
        check_descriptor(frame, 41);
        CHECK(pixels_match(frame));
        if (i == 0) generation = frame.descriptor().stream_generation;
        CHECK(frame.descriptor().stream_generation == generation);
        // The native ID is the SDK value; it may wrap (16-bit, 0 skipped).
        if (previous_id) CHECK(frame.descriptor().native_frame_id != *previous_id);
        previous_id = frame.descriptor().native_frame_id;
    }
    const auto stats = stream->statistics();
    CHECK(stats.delivered_frames == 6 && stats.incomplete_frames == 0);
    stream->stop();
    CHECK(device->close());
}

// C. Held pixels, bounded pool exhaustion, release restores delivery.
void pool_and_release() {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(3));
    std::vector<backend::FrameLease> held;
    std::vector<std::vector<std::byte>> copies;
    for (std::size_t i = 0; i < profile::pool_buffers; ++i) {
        held.push_back(receive_ok(*stream));
        copies.push_back(copy_bytes(held.back()));
        CHECK(pixels_match(held.back()));
    }
    // Every pool buffer is leased: a bounded timeout, no extra allocation.
    const auto stats_before = stream->statistics();
    CHECK(stats_before.delivered_frames == profile::pool_buffers && stats_before.pool_starvation_events == 0);
    const auto t0 = Clock::now();
    const auto starved = stream->receive(120ms);
    const auto waited = Clock::now() - t0;
    CHECK(!starved && starved.error().code == backend::ErrorCode::timeout);
    CHECK(waited >= 120ms && waited < 2s);
    CHECK(seen.buffers_allocated == static_cast<int>(profile::pool_buffers) && seen.live_buffers() == 4);
    auto stats = stream->statistics();
    CHECK(stats.timeout_count == 1);              // one public expiry, not one per slice
    CHECK(stats.pool_starvation_events == 1);     // one episode, despite many polls
    CHECK(stream->receive(60ms).error().code == backend::ErrorCode::timeout);
    stats = stream->statistics();
    CHECK(stats.timeout_count == 2 && stats.pool_starvation_events == 1); // same episode
    // Held bytes are unchanged while the receiver kept running.
    for (std::size_t i = 0; i < held.size(); ++i) CHECK(copy_bytes(held[i]) == copies[i] && pixels_match(held[i]));
    // Release one on another thread: it is requeued exactly once and delivery resumes.
    std::thread([lease = std::move(held.front())]() mutable { auto drop = std::move(lease); }).join();
    held.erase(held.begin());
    auto resumed = receive_ok(*stream);
    CHECK(pixels_match(resumed));
    CHECK(seen.buffers_allocated == static_cast<int>(profile::pool_buffers));
    held.push_back(std::move(resumed));
    // A new exhaustion after a release is a new episode.
    CHECK(stream->receive(60ms).error().code == backend::ErrorCode::timeout);
    stats = stream->statistics();
    CHECK(stats.delivered_frames == 5 && stats.timeout_count == 3 && stats.pool_starvation_events == 2);
    CHECK(stats.incomplete_frames == 0);
    // Zero timeout: one non-blocking poll; negative: rejected.
    held.clear();
    CHECK(stream->receive(-1ms).error().code == backend::ErrorCode::invalid_argument);
    // A zero-timeout poll never enters a native wait.
    const int waits = seen.native_waits.load();
    wait_until([&] { return stream->receive(0ms).has_value(); }, 2s, "a zero-timeout poll to find a queued frame");
    CHECK(seen.native_waits.load() == waits);
    stream->stop();
    CHECK(seen.live_buffers() == 0 && seen.live_streams() == 0);
    CHECK(device->close());
}

// D. Prompt cancellation of a GENUINELY blocked receive. Blocking is
// established by observing that receive entered a native timed pop while
// every pool buffer is leased (nothing can arrive), not by sleeping.
enum class Canceller { request_stop, stop, close };
void cancel_blocked(Canceller how) {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(5));
    std::vector<backend::FrameLease> held;
    for (std::size_t i = 0; i < profile::pool_buffers; ++i) held.push_back(receive_ok(*stream));
    std::atomic<bool> finished{false};
    std::optional<backend::Error> outcome;
    Clock::time_point returned{};
    const int waits_before = seen.native_waits.load();
    std::thread receiver([&] {
        auto result = stream->receive(60s); // long public timeout
        returned = Clock::now();
        if (!result) outcome = result.error();
        finished = true;
    });
    struct Join { std::thread& t; ~Join() { if (t.joinable()) t.join(); } } join{receiver};
    // Several native waits observed: receive is inside its polling wait.
    wait_until([&] { return seen.native_waits.load() >= waits_before + 3; }, 5s, "receive to enter native waits");
    CHECK(!finished);
    const auto cancelled_at = Clock::now();
    switch (how) {
    case Canceller::request_stop: stream->request_stop(); break;
    case Canceller::stop: stream->stop(); break;
    case Canceller::close: CHECK(device->close()); break;
    }
    // Generous bound, far below the 60 s timeout; expiry fails the test.
    wait_until([&] { return finished.load(); }, 2s, "cancelled receive to return");
    receiver.join();
    CHECK(returned - cancelled_at < 1s);
    CHECK(outcome && outcome->code == (how == Canceller::close ? backend::ErrorCode::disconnected
                                                                : backend::ErrorCode::stopped));
    CHECK(stream->statistics().timeout_count == 0);
    for (const auto& lease : held) CHECK(pixels_match(lease)); // still valid
    stream->stop();
    held.clear();
    CHECK(seen.live_buffers() == 0 && seen.live_streams() == 0);
    (void)device->close();
}
void cancellation() {
    for (int repeat = 0; repeat < 3; ++repeat) {
        cancel_blocked(Canceller::request_stop);
        cancel_blocked(Canceller::stop);
        cancel_blocked(Canceller::close);
    }
}

// E. A lease outlives stop, close and destruction of every external owner;
// release on another thread frees its buffer exactly once; native retirement
// precedes reservation release; an old release never touches a reopened run.
void lifetime_and_reopen() {
    Observer seen; // outlives every backend, device and lease below
    std::optional<backend::FrameLease> kept;
    std::vector<std::byte> kept_bytes;
    backend::FrameDescriptor kept_descriptor;
    {
        auto camera = aravis_fake::make_backend(seen.hooks());
        auto device = open_oracle(*camera);
        auto stream = std::move(device->create_stream()).value();
        CHECK(stream->start(11));
        kept = receive_ok(*stream);
        kept_bytes = copy_bytes(*kept);
        kept_descriptor = kept->descriptor();
        CHECK(pixels_match(*kept));
        (void)receive_ok(*stream); // another frame arrives and is requeued
        stream->stop();
        CHECK(seen.live_streams() == 0); // receiver joined, stream finalized
        CHECK(device->close());
        // Retirement order: native stream gone before the native device and
        // the reservation; only the leased buffer is still alive.
        CHECK(seen.live_buffers() == 1);
        CHECK(seen.devices_opened - seen.devices.count == 0);
        stream.reset();
        device.reset();
        camera.reset();
    }
    CHECK(copy_bytes(*kept) == kept_bytes && pixels_match(*kept));
    CHECK(kept->descriptor().native_frame_id == kept_descriptor.native_frame_id);
    CHECK(kept->descriptor().stream_generation == kept_descriptor.stream_generation);

    // Reopen (fresh backend and device) while the old lease is held; the old
    // run is retired, so the new device may acquire.
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(12));
    auto fresh = receive_ok(*stream);
    CHECK(fresh.descriptor().stream_generation != kept_descriptor.stream_generation);
    const int buffers_before_release = seen.buffers.count.load();
    // Release the OLD lease on another thread while the new run is active:
    // it is freed (not requeued into the new stream) exactly once.
    std::thread([lease = std::move(*kept)]() mutable { auto drop = std::move(lease); }).join();
    kept.reset();
    CHECK(seen.buffers.count.load() == buffers_before_release + 1);
    // The new run is unaffected: its pool still has exactly four buffers and
    // keeps delivering oracle frames of ITS generation only.
    CHECK(seen.live_buffers() == static_cast<int>(profile::pool_buffers));
    for (int i = 0; i < 4; ++i) {
        auto next = receive_ok(*stream);
        CHECK(pixels_match(next) && next.descriptor().stream_generation == fresh.descriptor().stream_generation);
    }
    stream->stop();
    CHECK(device->close());
    // `fresh` is still held after close; dropping it frees the last buffer.
    CHECK(seen.live_buffers() == 1);
    { auto drop = std::move(fresh); }
    CHECK(seen.live_buffers() == 0 && seen.live_streams() == 0);
}

// F. Restart policy on the same logical device.
void restart_policy() {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(21));
    auto old_lease = receive_ok(*stream);
    const auto old_bytes = copy_bytes(old_lease);
    const auto old_descriptor = old_lease.descriptor();
    stream->stop();
    // Refused while a lease of the previous run is held, also through a
    // replacement wrapper; no native stream is created.
    const int created = seen.streams_created.load();
    const auto refused = stream->start(22);
    CHECK(!refused && refused.error().code == backend::ErrorCode::resource_exhausted);
    CHECK(contains(refused.error().message, "release them before restarting"));
    auto replacement = std::move(device->create_stream()).value();
    CHECK(replacement->start(22).error().code == backend::ErrorCode::resource_exhausted);
    CHECK(seen.streams_created.load() == created);
    // Configuration may change after a completed stop; the old lease is unchanged.
    const backend::CameraConfiguration changed{32, 16, 0, 0, 100, 10ms};
    CHECK(device->apply(changed).value() == changed);
    CHECK(copy_bytes(old_lease) == old_bytes && old_lease.descriptor().configuration == oracle);
    CHECK(device->apply(oracle).value() == oracle);
    { auto drop = std::move(old_lease); }
    // Released: explicit restart works with a fresh native stream and a new
    // generation; no stale frame from the old run is delivered.
    CHECK(replacement->start(23));
    CHECK(seen.streams_created.load() == created + 1 && seen.live_streams() == 1);
    for (int i = 0; i < 5; ++i) {
        auto frame = receive_ok(*replacement);
        check_descriptor(frame, 23);
        CHECK(pixels_match(frame));
        CHECK(frame.descriptor().stream_generation > old_descriptor.stream_generation);
    }
    replacement->stop();
    // The original wrapper may also restart explicitly (new generation again).
    CHECK(stream->start(24));
    const auto again = receive_ok(*stream);
    CHECK(again.descriptor().configuration_generation == 24);
    stream->stop();
    // Statistics are per wrapper and persist across that wrapper's runs.
    CHECK(stream->statistics().delivered_frames == 2 && replacement->statistics().delivered_frames == 5);
    CHECK(device->close());
}

// Imposes a GenICam access mode on a command through the SDK's public DOM
// (an <ImposedAccessMode> child) so that the SDK ITSELF rejects or accepts
// later executions. Returns the property node so it can be changed.
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

// G. Native start-command failure after partial setup (stream created, pool
// queued), stop-command failure reporting, and explicit recovery.
void failure_cleanup() {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    ArvDomNode* start_access = nullptr;
    ArvDomNode* stop_access = nullptr;
    native(*device, [&](ArvDevice* d) {
        start_access = impose_access(d, "AcquisitionStart", "RW");
        stop_access = impose_access(d, "AcquisitionStop", "RW");
    });
    // After the adapter validated the command (writable), make it read-only
    // through the SDK, so the SDK's own execute rejects it.
    std::atomic<bool> fail_start{true}, fail_stop{false};
    seen.on_command = [&](std::string_view c, ArvDevice*) {
        if (c == "AcquisitionStart" && fail_start.exchange(false)) set_access(start_access, "RO");
        if (c == "AcquisitionStop" && fail_stop.exchange(false)) set_access(stop_access, "RO");
    };
    auto stream = std::move(device->create_stream()).value();
    const auto failed = stream->start(31);
    CHECK(!failed && failed.error().code == backend::ErrorCode::unsupported); // ARV_GC_ERROR_READ_ONLY
    CHECK(contains(failed.error().message, "AcquisitionStart") && contains(failed.error().message, "read only"));
    CHECK(contains(failed.error().message, "queued buffers were released") && contains(failed.error().message, "retry"));
    // Partial setup existed and is fully retired: no worker, no pool, no owner.
    CHECK(seen.streams_created == 1 && seen.live_streams() == 0);
    CHECK(seen.buffers_allocated == 4 && seen.live_buffers() == 0);
    CHECK(acquisition_register(*device) == 0);
    CHECK((seen.command_log() == std::vector<std::string>{"AcquisitionStart"})); // no stop for a failed start
    CHECK(stream->receive(20ms).error().code == backend::ErrorCode::stopped);
    CHECK(device->configuration().value() == oracle); // configuration unchanged and usable
    CHECK(device->apply(oracle).value() == oracle);   // no acquisition owner remains
    // Explicit recovery: the SDK accepts the command again.
    set_access(start_access, "RW");
    CHECK(stream->start(32));
    CHECK(acquisition_register(*device) == 1);
    auto frame = receive_ok(*stream);
    CHECK(pixels_match(frame));
    // Stop-command failure: local resources are retired anyway, the failure
    // is preserved and reported by close(); the camera is NOT claimed stopped.
    fail_stop = true;
    stream->stop();
    CHECK(seen.live_streams() == 0);
    CHECK(acquisition_register(*device) == 1); // the SDK rejected AcquisitionStop: still acquiring
    CHECK(pixels_match(frame));                // held lease still valid
    { auto drop = std::move(frame); }
    CHECK(seen.live_buffers() == 0);
    const auto closed = device->close();
    CHECK(!closed && contains(closed.error().message, "AcquisitionStop") &&
          contains(closed.error().message, "the camera may still be acquiring"));
    CHECK(device->close()); // reported once; close stays idempotent
}

// H. Invalid native facts.
// (1) Real SDK arrangements: unsupported acquisition mode and pixel format.
// (2) Allocation limits through the same planner start() uses.
// (3) DECODED-FACTS SEAM (labelled): facts decoded from REAL popped SDK
//     buffers are edited before the production validator; see seam_cases().
void invalid_native_facts() {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    native(*device, [](ArvDevice* d) {
        GError* error = nullptr;
        arv_device_set_string_feature_value(d, "AcquisitionMode", "SingleFrame", &error);
        CHECK(!error);
    });
    auto stream = std::move(device->create_stream()).value();
    const auto refused = stream->start(1);
    CHECK(!refused && refused.error().code == backend::ErrorCode::unsupported);
    CHECK(contains(refused.error().message, "AcquisitionMode is 'SingleFrame'"));
    CHECK(seen.streams_created == 0 && seen.buffers_allocated == 0 && seen.command_log().empty());
    native(*device, [](ArvDevice* d) {
        CHECK(std::string(arv_device_get_string_feature_value(d, "AcquisitionMode", nullptr)) == "SingleFrame");
        arv_device_set_string_feature_value(d, "AcquisitionMode", "Continuous", nullptr);
        arv_device_set_string_feature_value(d, "PixelFormat", "Mono16", nullptr);
    });
    const auto mono16 = stream->start(1);
    CHECK(!mono16 && contains(mono16.error().message, "PixelFormat is 'Mono16'"));
    CHECK(seen.streams_created == 0);
    native(*device, [](ArvDevice* d) { arv_device_set_string_feature_value(d, "PixelFormat", "Mono8", nullptr); });
    CHECK(stream->start(1));
    stream->stop();
    CHECK(device->close());

    const backend::CameraConfiguration c{64, 48, 0, 0, 25, 10ms};
    CHECK(profile::plan_pool(3072, c).value().total_bytes == 4 * 3072);
    CHECK(profile::plan_pool(0, c).error().code == backend::ErrorCode::unsupported);
    CHECK(profile::plan_pool(-1, c).error().code == backend::ErrorCode::unsupported);
    CHECK(contains(profile::plan_pool(3071, c).error().message, "smaller than the 64x48 Mono8 image"));
    CHECK(profile::plan_pool(std::int64_t{65} << 20, c).error().code == backend::ErrorCode::resource_exhausted);
    CHECK(profile::plan_pool(std::numeric_limits<std::int64_t>::max(), c).error().code ==
          backend::ErrorCode::resource_exhausted);
    CHECK(profile::plan_pool(std::int64_t{64} << 20, c, 5).error().code == backend::ErrorCode::resource_exhausted);
    CHECK(profile::plan_pool(std::int64_t{64} << 20, c, 4).value().total_bytes == std::size_t{256} << 20);
    const auto huge = std::numeric_limits<std::size_t>::max();
    CHECK(profile::plan_pool(4096, c, huge, huge, huge).error().code == backend::ErrorCode::resource_exhausted);
}

// DECODED-FACTS SEAM. While a case is armed, EVERY real popped buffer's facts
// are edited before the production validator. The receive must then expire
// at its one deadline (rejected/incomplete buffers are recycled and retried
// inside it), report the validator's reason, deliver nothing, and allocate
// nothing. Disarming restores genuine oracle frames from the same pool.
void seam_cases() {
    struct Case { const char* expect; void (*edit)(profile::NativeFrameFacts&); };
    const Case cases[] = {
        {nullptr, [](auto& f) { f.status = profile::NativeStatus::incomplete; f.raw_status = 4; }},
        {"is not a completed image", [](auto& f) { f.status = profile::NativeStatus::other; f.raw_status = 6; }},
        {"is not a plain image", [](auto& f) { f.payload = profile::NativePayload::multipart; f.raw_payload = 10; }},
        {"chunk data", [](auto& f) { f.has_chunks = true; }},
        {"exactly one image part", [](auto& f) { f.parts = 2; }},
        {"is not Mono8", [](auto& f) { f.pixel_format = 0x010c0006u; }}, // packed 12-bit
        {"differs from the frozen configuration", [](auto& f) { f.width -= 1; }},
        {"Y padding", [](auto& f) { f.padding_y = 64; }},
        {"no padding is fabricated", [](auto& f) { f.padding_x = 1; }}, // stride 65 needs 3120 bytes
        {"no padding is fabricated", [](auto& f) { f.image_bytes -= 1; }},
        {"is not inside the received size", [](auto& f) { f.received_bytes = f.capacity_bytes + 1; }},
        {"is not inside the received size", [](auto& f) { f.part_offset = 1; }},
        {"image span overflows", [](auto& f) { f.padding_x = std::numeric_limits<std::int64_t>::max(); }},
    };
    Observer seen;
    std::atomic<const Case*> armed{nullptr};
    std::atomic<int> edited{0};
    aravis::TestHooks hooks = seen.hooks();
    hooks.decoded_facts = [&](profile::NativeFrameFacts& f) {
        if (const Case* c = armed.load()) { c->edit(f); edited.fetch_add(1); }
    };
    auto camera = aravis_fake::make_backend(hooks);
    auto device = open_oracle(*camera);
    auto stream = std::move(device->create_stream()).value();
    CHECK(stream->start(2));
    std::uint64_t timeouts = 0;
    for (const auto& c : cases) {
        armed = &c;
        const int before = edited.load();
        const auto t0 = Clock::now();
        const auto result = stream->receive(150ms);
        const auto elapsed = Clock::now() - t0;
        CHECK(!result && result.error().code == backend::ErrorCode::timeout);
        CHECK(elapsed >= 150ms && elapsed < 1s); // one deadline, not restarted per buffer
        CHECK(edited.load() > before);           // real buffers were popped and judged
        if (c.expect && !contains(result.error().message, c.expect))
            throw std::runtime_error(std::string("expected \"") + c.expect + "\" in: " + result.error().message);
        CHECK(stream->statistics().timeout_count == ++timeouts);
    }
    armed = nullptr;
    const auto stats = stream->statistics();
    CHECK(stats.delivered_frames == 0 && stats.incomplete_frames >= 1); // only the incomplete case counts
    CHECK(seen.buffers_allocated == static_cast<int>(profile::pool_buffers)); // recycled, never reallocated
    auto good = receive_ok(*stream);
    CHECK(pixels_match(good));
    stream->stop();
    CHECK(seen.live_buffers() == 1);
    { auto drop = std::move(good); }
    CHECK(seen.live_buffers() == 0);
    CHECK(device->close());
}

// I. Exception unwind while a receive is blocked. The cleanup guard is
// installed before start; the observer and gate outlive the activity; the
// gate is released before joining; no watchdog provides the release.
void exception_unwind() {
    Observer seen; // outlives everything below
    struct Gate {
        std::mutex m;
        std::condition_variable cv;
        bool blocked{false};
    } gate;
    seen.on_wait = [&] {
        { std::lock_guard lock(gate.m); gate.blocked = true; }
        gate.cv.notify_all();
    };
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    auto stream = std::move(device->create_stream()).value();
    std::vector<backend::FrameLease> held;
    std::thread receiver;
    std::optional<backend::ErrorCode> receiver_result;
    bool thrown = false;
    const auto unwind_started = std::make_shared<Clock::time_point>();
    try {
        struct Cleanup {
            backend::IFrameStream& stream;
            std::thread& receiver;
            ~Cleanup() {
                stream.request_stop(); // releases the blocked receive
                if (receiver.joinable()) receiver.join();
                stream.stop();
            }
        } cleanup{*stream, receiver};
        CHECK(stream->start(51));
        for (std::size_t i = 0; i < profile::pool_buffers; ++i) held.push_back(receive_ok(*stream));
        { std::lock_guard lock(gate.m); gate.blocked = false; }
        receiver = std::thread([&] {
            auto result = stream->receive(60s);
            receiver_result = result ? std::optional<backend::ErrorCode>{} : result.error().code;
        });
        {
            std::unique_lock lock(gate.m);
            CHECK(gate.cv.wait_for(lock, 5s, [&] { return gate.blocked; })); // genuinely waiting natively
        }
        *unwind_started = Clock::now();
        throw std::runtime_error("deliberate failure while receive is blocked");
    } catch (const std::runtime_error& error) {
        thrown = contains(error.what(), "deliberate failure");
    }
    CHECK(thrown);
    CHECK(Clock::now() - *unwind_started < 2s); // bounded cleanup
    CHECK(!receiver.joinable() && receiver_result == backend::ErrorCode::stopped);
    CHECK(seen.live_streams() == 0);
    for (const auto& lease : held) CHECK(pixels_match(lease));
    held.clear();
    CHECK(seen.live_buffers() == 0);
    CHECK(device->close());
}

// J. The diagnostic's capture routine against the private Fake backend; the
// PGM is inspected independently of the routine.
void direct_capture() {
    const std::string path = "aravis_stream_contract_capture.pgm";
    std::remove(path.c_str());
    {
        // Argument validation precedes device I/O.
        Observer seen;
        auto camera = aravis_fake::make_backend(seen.hooks());
        CHECK(aravis::capture_one(*camera, "", path).error->code == backend::ErrorCode::invalid_argument);
        CHECK(aravis::capture_one(*camera, aravis_fake::device_id, "").error->code == backend::ErrorCode::invalid_argument);
        CHECK(aravis::capture_one(*camera, aravis_fake::device_id, path, 0ms).error->code ==
              backend::ErrorCode::invalid_argument);
        CHECK(seen.devices_opened == 0);
        // Output failure: cleanup still stops and closes (reopen succeeds).
        const auto bad = aravis::capture_one(*camera, aravis_fake::device_id, "/nonexistent-dir/x.pgm");
        CHECK(!bad.ok() && bad.error && bad.error->code == backend::ErrorCode::io_error && !bad.cleanup_error);
        CHECK(seen.live_streams() == 0 && seen.live_buffers() == 0 && seen.devices_opened - seen.devices.count == 0);
    }
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    // Each Fake open creates a fresh ArvFakeCamera, so capture uses the Fake
    // defaults as its current configuration; prove them by readback first.
    {
        auto device = open_fake(*camera);
        CHECK(device->configuration().value() == (backend::CameraConfiguration{512, 512, 0, 0, 25, 10ms}));
        CHECK(native_integer(*device, "GainRaw") == 0);
        CHECK(device->close());
    }
    const auto outcome = aravis::capture_one(*camera, aravis_fake::device_id, path);
    CHECK(outcome.ok());
    const auto& r = *outcome.report;
    CHECK(r.width == 512 && r.height == 512 && r.source_row_stride == 512 && !r.timestamp_valid);
    CHECK(r.configuration.exposure == 10ms && r.statistics.delivered_frames == 1);
    CHECK(seen.live_streams() == 0 && seen.live_buffers() == 0); // stopped, closed, lease released
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string header = "P5\n512 512\n255\n";
    CHECK(file.size() == header.size() + 512u * 512u && r.pgm_bytes == file.size());
    CHECK(std::string(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(header.size())) == header);
    for (std::uint32_t y = 0; y < 512; ++y)
        for (std::uint32_t x = 0; x < 512; ++x)
            CHECK(static_cast<unsigned char>(file[header.size() + y * 512u + x]) == (x + y + r.native_frame_id) % 255);
    std::remove(path.c_str());
}

// Any GLib warning or critical (e.g. a g_return_if_fail precondition reached
// by the adapter) aborts the test: they are failures, not noise.
void fatal_glib_log(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer) {
    std::cerr << "FAIL GLib " << ((level & G_LOG_LEVEL_CRITICAL) ? "critical" : "warning") << " ["
              << (domain ? domain : "") << "]: " << (message ? message : "") << '\n';
    std::abort();
}

} // namespace

int main(int argc, char** argv) {
    std::cout << "Aravis runtime " << aravis::runtime_version() << " (compiled " << aravis::compiled_version()
              << ") loaded from " << aravis_fake::loaded_library_path() << "; Fake device only\n";
    if (aravis::runtime_version() != "0.8.36" || aravis::compiled_version() != "0.8.36") {
        std::cerr << "FAIL: the qualified Aravis 0.8.36 library is not the one loaded\n";
        return 1;
    }
    g_log_set_always_fatal(static_cast<GLogLevelFlags>(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING));
    for (const char* domain : {static_cast<const char*>(nullptr), "Aravis", "GLib", "GLib-GObject"})
        g_log_set_handler(domain, static_cast<GLogLevelFlags>(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL),
                          fatal_glib_log, nullptr);
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"A start, commands and single acquisition owner", start_and_commands},
        {"B real SDK pixels and descriptors", real_pixels_and_descriptors},
        {"C bounded pool, held pixels and release", pool_and_release},
        {"D prompt cancellation of a blocked receive (x3)", cancellation},
        {"E lease lifetime, retirement order and reopen", lifetime_and_reopen},
        {"F restart policy", restart_policy},
        {"G native command failure cleanup and recovery", failure_cleanup},
        {"H invalid native facts (SDK arrangements and limits)", invalid_native_facts},
        {"H decoded-facts seam through the production validator", seam_cases},
        {"I exception unwind while receive is blocked", exception_unwind},
        {"J direct capture helper and independent PGM check", direct_capture},
    };
    std::size_t passed = 0;
    for (const auto& [name, run] : tests) {
        if (argc == 2 && std::string(name).find(argv[1]) == std::string::npos) continue;
        try {
            run();
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
        ++passed;
        std::cout << "PASS " << name << std::endl;
    }
    std::cout << passed << " Aravis stream contract scenarios passed\n";
    return 0;
}
