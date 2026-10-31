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
    std::atomic<int> writes{0}; // configuration feature writes attempted by apply()
    std::mutex mutex;
    std::vector<std::string> commands;
    std::function<void(std::string_view, ArvDevice*)> on_command; // arrangement, set per test
    std::function<void()> on_wait;                                // observation, set per test
    // TEST-ONLY command-result boundary (see TestHooks::command_result).
    std::function<std::optional<backend::Error>(std::string_view, ArvDevice*)> on_result;
    aravis::TestHooks hooks() {
        aravis::TestHooks h;
        h.after_native_open = [this](ArvDevice* d) { devices_opened.fetch_add(1); devices.watch(d); };
        h.after_stream_created = [this](ArvStream* s) { streams_created.fetch_add(1); streams.watch(s); };
        h.after_buffer_allocated = [this](ArvBuffer* b) { buffers_allocated.fetch_add(1); buffers.watch(b); };
        h.before_command = [this](std::string_view c, ArvDevice* d) {
            { std::lock_guard lock(mutex); commands.emplace_back(c); }
            if (on_command) on_command(c, d);
        };
        h.before_write = [this](std::string_view, ArvDevice*) { writes.fetch_add(1); };
        h.command_result = [this](std::string_view c, ArvDevice* d) -> std::optional<backend::Error> {
            if (on_result) return on_result(c, d);
            return std::nullopt;
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

// Shared failure cleanup for a test thread blocked in receive() (R1 8A):
// request cancellation FIRST, then join, then retire the stream. Install it
// before start(), and declare observers and held leases before it so that
// they outlive the cleanup. (A join-only cleanup would wait for the long
// public receive timeout, because nothing else wakes the receiver.)
struct ReceiverCleanup {
    backend::IFrameStream& stream;
    std::thread& receiver;
    ~ReceiverCleanup() {
        stream.request_stop(); // releases the blocked receive within one slice
        if (receiver.joinable()) receiver.join();
        stream.stop();
    }
};

// D. Prompt cancellation of a GENUINELY blocked receive. Blocking is
// established by observing that receive entered a native timed pop while
// every pool buffer is leased (nothing can arrive), not by sleeping.
// Canceller::thrown exercises the shared failure cleanup itself: the test
// throws before its normal cancellation statement while receive is blocked.
enum class Canceller { request_stop, stop, close, thrown };
void run_canceller(Canceller how, backend::IFrameStream& stream, backend::ICameraDevice& device) {
    switch (how) {
    case Canceller::request_stop: stream.request_stop(); break;
    case Canceller::stop: stream.stop(); break;
    case Canceller::close: CHECK(device.close()); break;
    case Canceller::thrown: break;
    }
}
void cancel_blocked(Canceller how) {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    auto stream = std::move(device->create_stream()).value();
    std::vector<backend::FrameLease> held; // outlives the cleanup below
    std::atomic<bool> finished{false};
    std::optional<backend::Error> outcome;
    Clock::time_point returned{};
    Clock::time_point cancelled_at{};
    std::thread receiver;
    try {
        ReceiverCleanup cleanup{*stream, receiver}; // before any acquisition activity
        CHECK(stream->start(5));
        for (std::size_t i = 0; i < profile::pool_buffers; ++i) held.push_back(receive_ok(*stream));
        const int waits_before = seen.native_waits.load();
        receiver = std::thread([&] {
            auto result = stream->receive(60s); // long public timeout
            returned = Clock::now();
            if (!result) outcome = result.error();
            finished = true;
        });
        // Several native waits observed: receive is inside its polling wait.
        wait_until([&] { return seen.native_waits.load() >= waits_before + 3; }, 5s, "receive to enter native waits");
        CHECK(!finished);
        cancelled_at = Clock::now();
        if (how == Canceller::thrown) throw std::logic_error("deliberate failure before cancellation");
        run_canceller(how, *stream, *device);
        // Generous bound, far below the 60 s timeout; expiry fails the test.
        wait_until([&] { return finished.load(); }, 2s, "cancelled receive to return");
    } catch (const std::logic_error& error) {
        CHECK(how == Canceller::thrown && contains(error.what(), "deliberate failure"));
    }
    // The cleanup (or the normal path) has joined the receiver.
    CHECK(!receiver.joinable() && finished);
    CHECK(Clock::now() - cancelled_at < 2s); // no reliance on the 60 s timeout
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
        cancel_blocked(Canceller::thrown);
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
    // R1 (finding A): the SDK executor WAS called, so its error does not prove
    // the command was unapplied; production compensates with one stop. (The
    // old blanket "no stop for a failed start" assertion is not a contract.)
    CHECK((seen.command_log() == std::vector<std::string>{"AcquisitionStart", "AcquisitionStop"}));
    CHECK(contains(failed.error().message, "a compensating AcquisitionStop succeeded"));
    CHECK(stream->receive(20ms).error().code == backend::ErrorCode::stopped);
    CHECK(device->configuration().value() == oracle); // configuration unchanged and usable
    CHECK(device->apply(oracle).value() == oracle);   // the compensating stop resolved the outcome
    // Explicit recovery: the SDK accepts the command again.
    set_access(start_access, "RW");
    CHECK(stream->start(32));
    CHECK(acquisition_register(*device) == 1);
    auto frame = receive_ok(*stream);
    CHECK(pixels_match(frame));
    // B1. Stop-command failure: local resources are retired anyway, the
    // failure is preserved, and the camera is NOT claimed stopped.
    fail_stop = true;
    stream->stop();
    CHECK(seen.live_streams() == 0);
    CHECK(acquisition_register(*device) == 1); // the SDK rejected AcquisitionStop: still acquiring
    CHECK(pixels_match(frame));                // held lease still valid
    // No active receiver is not proof that reuse is safe (R1 finding B):
    // configuration is refused before writes...
    const int writes_before = seen.writes.load();
    const auto blocked = device->apply(oracle);
    CHECK(!blocked && blocked.error().code == backend::ErrorCode::invalid_state);
    CHECK(contains(blocked.error().message, "is uncertain") && contains(blocked.error().message, "nothing was written"));
    CHECK(seen.writes.load() == writes_before);
    // ...and, with the held lease released so that the lease rule cannot be
    // the reason, restart is refused before any native stream or command.
    { auto drop = std::move(frame); }
    CHECK(seen.live_buffers() == 0);
    const int streams_before = seen.streams_created.load();
    const auto commands_before = seen.command_log().size();
    const auto refused = stream->start(33);
    CHECK(!refused && refused.error().code == backend::ErrorCode::invalid_state);
    CHECK(contains(refused.error().message, "AcquisitionStop") && contains(refused.error().message, "read only"));
    CHECK(seen.streams_created.load() == streams_before && seen.command_log().size() == commands_before);
    CHECK(acquisition_register(*device) == 1);
    // Explicit recovery through the owning wrapper's stop(), with no native
    // stream left: one bounded AcquisitionStop, which the SDK now accepts.
    set_access(stop_access, "RW");
    stream->stop();
    CHECK(seen.command_log().size() == commands_before + 1 && seen.command_log().back() == "AcquisitionStop");
    CHECK(acquisition_register(*device) == 0);
    stream->stop(); // resolved: no further command
    CHECK(seen.command_log().size() == commands_before + 1);
    CHECK(device->apply(oracle).value() == oracle);
    CHECK(stream->start(34) && acquisition_register(*device) == 1);
    stream->stop();
    CHECK(acquisition_register(*device) == 0);
    // The historical failure is reported once by close(), as recovered.
    const auto closed = device->close();
    CHECK(!closed && contains(closed.error().message, "AcquisitionStop") &&
          contains(closed.error().message, "recovered: an explicit stop() retry succeeded"));
    CHECK(!contains(closed.error().message, "remains uncertain"));
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
        ReceiverCleanup cleanup{*stream, receiver}; // shared with scenario D
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

struct Unref { void operator()(gpointer object) const noexcept { if (object) g_object_unref(object); } };

// ===========================================================================
// GIGE-MEL-009-R1 regressions (findings A, B).
//
// Arrangement labels: "SDK-arranged" = the real SDK rejects a command because
// of an imposed GenICam access mode. "Injected command-result fault" = the
// SDK executed the command SUCCESSFULLY (the Fake acquisition register really
// changed), then the test-only TestHooks::command_result boundary returned an
// error in its place; production makes every recovery decision. That models
// an uncertain native command result after a genuine effect. It is not
// physical packet-loss or lost-acknowledgment qualification.

// Counts of every native acquisition side effect.
struct SideEffects {
    int streams, buffers, commands, writes;
    static SideEffects of(Observer& seen) {
        return {seen.streams_created.load(), seen.buffers_allocated.load(), static_cast<int>(seen.command_log().size()),
                seen.writes.load()};
    }
    bool operator==(const SideEffects&) const = default;
};

// K. Start outcomes: A1 known pre-execution failure, A2 applied start + error
// with a successful compensating stop, A3 applied start + error with a failed
// compensating stop, then explicit stop() recovery without a native stream.
void start_outcomes() {
    Observer seen;
    std::atomic<bool> inject_start{false}, start_register_seen{false};
    std::atomic<int> start_executions{0};
    seen.on_result = [&](std::string_view c, ArvDevice* d) -> std::optional<backend::Error> {
        if (c == "AcquisitionStart") start_executions.fetch_add(1);
        if (c != "AcquisitionStart" || !inject_start.exchange(false)) return std::nullopt;
        // The SDK command genuinely took effect before the fault is injected.
        start_register_seen = arv_fake_camera_get_acquisition_status(arv_fake_device_get_fake_camera(ARV_FAKE_DEVICE(d))) == 1;
        return backend::Error{backend::ErrorCode::timeout, "injected AcquisitionStart result: no acknowledgment"};
    };
    std::atomic<bool> throw_before_start{false};
    seen.on_command = [&](std::string_view c, ArvDevice*) {
        if (c == "AcquisitionStart" && throw_before_start.exchange(false))
            throw std::runtime_error("arranged failure before the start executor");
    };
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    auto stream = std::move(device->create_stream()).value();
    ArvDomNode* stop_access = nullptr;
    native(*device, [&](ArvDevice* d) { stop_access = impose_access(d, "AcquisitionStop", "RW"); });

    // A1. Known pre-execution failure after partial local setup: the hook
    // before the executor throws, so the SDK command is never called.
    throw_before_start = true;
    bool threw = false;
    try { (void)stream->start(60); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && start_executions == 0);
    CHECK((seen.command_log() == std::vector<std::string>{"AcquisitionStart"})); // reached the hook only
    CHECK(seen.streams_created == 1 && seen.live_streams() == 0);               // partial stream retired
    CHECK(seen.buffers_allocated == 4 && seen.live_buffers() == 0);             // partial pool retired
    CHECK(acquisition_register(*device) == 0);                                 // nothing to compensate
    CHECK(device->apply(oracle).value() == oracle);                            // no uncertainty invented
    CHECK(stream->start(61) && acquisition_register(*device) == 1);            // explicit retry works
    stream->stop();
    CHECK(acquisition_register(*device) == 0 && seen.live_streams() == 0);

    // A2. Applied start, then an injected error; the compensating stop works.
    auto before = SideEffects::of(seen);
    inject_start = true;
    const auto a2 = stream->start(62);
    CHECK(start_register_seen.exchange(false)); // the Fake acquisition really became active
    CHECK(!a2 && a2.error().code == backend::ErrorCode::timeout); // the ORIGINAL start failure, not Success
    CHECK(contains(a2.error().message, "injected AcquisitionStart result"));
    CHECK(contains(a2.error().message, "a compensating AcquisitionStop succeeded"));
    auto log = seen.command_log();
    CHECK(log.size() == static_cast<std::size_t>(before.commands) + 2 && log[log.size() - 2] == "AcquisitionStart" &&
          log.back() == "AcquisitionStop");
    CHECK(acquisition_register(*device) == 0);           // stopped by the compensating command
    CHECK(seen.live_streams() == 0 && seen.live_buffers() == 0); // stream and pool retired
    CHECK(stream->receive(10ms).error().code == backend::ErrorCode::stopped);
    CHECK(device->apply(oracle).value() == oracle);      // explicit reuse
    CHECK(stream->start(63) && acquisition_register(*device) == 1);
    stream->stop();

    // A3. Applied start, injected error, and the compensating stop is ALSO
    // rejected by the SDK (imposed read-only access).
    seen.on_command = [&](std::string_view c, ArvDevice*) {
        if (c == "AcquisitionStop") set_access(stop_access, "RO");
    };
    inject_start = true;
    const auto a3 = stream->start(64);
    CHECK(start_register_seen.exchange(false));
    CHECK(!a3 && a3.error().code == backend::ErrorCode::timeout); // primary start failure preserved
    CHECK(contains(a3.error().message, "injected AcquisitionStart result"));
    CHECK(contains(a3.error().message, "the compensating AcquisitionStop also failed") &&
          contains(a3.error().message, "read only") && contains(a3.error().message, "may still be acquiring"));
    CHECK(acquisition_register(*device) == 1);                   // the camera is still acquiring
    CHECK(seen.live_streams() == 0 && seen.live_buffers() == 0); // local retirement anyway
    seen.on_command = nullptr;
    // apply(): refused with zero configuration writes.
    before = SideEffects::of(seen);
    const auto blocked = device->apply({32, 32, 0, 0, 100, 10ms});
    CHECK(!blocked && blocked.error().code == backend::ErrorCode::invalid_state);
    CHECK(contains(blocked.error().message, "AcquisitionStart") && contains(blocked.error().message, "nothing was written"));
    // start(): refused with no stream, pool or command, from any wrapper.
    CHECK(stream->start(65).error().code == backend::ErrorCode::invalid_state);
    auto other = std::move(device->create_stream()).value();
    CHECK(other->start(65).error().code == backend::ErrorCode::invalid_state);
    // Another wrapper cannot take over this wrapper's unresolved recovery.
    other->stop();
    other->request_stop();
    CHECK(SideEffects::of(seen) == before && acquisition_register(*device) == 1);
    // Read-only diagnostics stay available.
    CHECK(device->configuration().value() == oracle);
    CHECK(device->capabilities().sensor_width == 2048);
    // request_stop() on the owner is prompt signalling only: no command.
    stream->request_stop();
    CHECK(SideEffects::of(seen) == before);
    // Restore stop capability; the OWNING wrapper's stop() recovers with no
    // native stream left (exactly one bounded command).
    set_access(stop_access, "RW");
    stream->stop();
    CHECK(seen.command_log().size() == static_cast<std::size_t>(before.commands) + 1);
    CHECK(seen.command_log().back() == "AcquisitionStop" && acquisition_register(*device) == 0);
    CHECK(seen.streams_created == before.streams); // recovery created no stream
    // Later explicit reuse.
    CHECK(device->apply(oracle).value() == oracle);
    CHECK(other->start(66) && acquisition_register(*device) == 1);
    auto frame = receive_ok(*other);
    CHECK(pixels_match(frame));
    other->stop();
    { auto drop = std::move(frame); }
    // close() reports the historical A3 failure once, as recovered, keeping
    // the primary start error (not only the cleanup error).
    const auto closed = device->close();
    CHECK(!closed && contains(closed.error().message, "injected AcquisitionStart result") &&
          contains(closed.error().message, "compensating AcquisitionStop also failed") &&
          contains(closed.error().message, "recovered: an explicit stop() retry succeeded"));
    CHECK(!contains(closed.error().message, "remains uncertain"));
    CHECK(device->close());
}

// Puts `device` into the uncertain state through a real SDK-rejected stop
// (B1 arrangement) while `stream` stays alive as the owner. A lease is held
// throughout to prove byte validity across retirement and close.
struct UncertainStop {
    Observer& seen;
    std::shared_ptr<backend::ICameraDevice> device;
    std::unique_ptr<backend::IFrameStream> stream;
    ArvDomNode* stop_access{nullptr};
    std::optional<backend::FrameLease> held;
    std::vector<std::byte> held_bytes;
    UncertainStop(Observer& observer, backend::ICameraBackend& camera) : seen(observer), device(open_oracle(camera)) {
        native(*device, [&](ArvDevice* d) { stop_access = impose_access(d, "AcquisitionStop", "RW"); });
        stream = std::move(device->create_stream()).value();
        CHECK(stream->start(70));
        held = receive_ok(*stream);
        held_bytes = copy_bytes(*held);
        seen.on_command = [this](std::string_view c, ArvDevice*) {
            if (c == "AcquisitionStop") set_access(stop_access, "RO");
        };
        stream->stop();
        seen.on_command = nullptr;
        CHECK(acquisition_register(*device) == 1 && seen.live_streams() == 0);
        CHECK(device->apply(oracle).error().code == backend::ErrorCode::invalid_state);
    }
};

// L. Uncertain outcome followed by close() with no active native stream:
// close still attempts the stop, closes the device regardless, reports
// truthfully, and stays idempotent without device I/O. The owner wrapper is
// kept alive during the assertions so its destructor cannot resolve anything.
void uncertain_close() {
    for (const bool stop_works : {true, false}) {
        Observer seen;
        auto camera = aravis_fake::make_backend(seen.hooks());
        UncertainStop u(seen, *camera);
        // Writable at the adapter's validation in both cases, so close()
        // reaches the SDK executor; when the final cleanup must fail, the SDK
        // itself rejects the execution (same arrangement as B1).
        set_access(u.stop_access, "RW");
        if (!stop_works)
            seen.on_command = [&](std::string_view c, ArvDevice*) {
                if (c == "AcquisitionStop") set_access(u.stop_access, "RO");
            };
        // Fake register read while the device is still open: the arranged
        // camera outcome at close time is "still acquiring".
        std::unique_ptr<ArvDevice, Unref> fake_device;
        native(*u.device, [&](ArvDevice* d) { fake_device.reset(ARV_DEVICE(g_object_ref(d))); });
        const auto commands = seen.command_log().size();
        const int writes = seen.writes.load();
        const auto closed = u.device->close();
        CHECK(seen.command_log().size() == commands + 1 && seen.command_log().back() == "AcquisitionStop");
        CHECK(seen.writes.load() == writes);
        const auto status = arv_fake_camera_get_acquisition_status(
            arv_fake_device_get_fake_camera(ARV_FAKE_DEVICE(fake_device.get())));
        CHECK(status == (stop_works ? 0u : 1u)); // the attempted stop's real effect
        CHECK(!closed && contains(closed.error().message, "AcquisitionStop") && contains(closed.error().message, "read only"));
        if (stop_works) {
            CHECK(contains(closed.error().message, "recovered: AcquisitionStop during close() succeeded"));
            CHECK(!contains(closed.error().message, "remains uncertain"));
        } else {
            CHECK(contains(closed.error().message, "close(): ") &&
                  contains(closed.error().message, "the camera acquisition outcome remains uncertain"));
        }
        fake_device.reset();
        // Closed-device behavior, both cases; the reservation was released.
        CHECK(seen.devices_opened - seen.devices.count == 0);
        CHECK(u.device->apply(oracle).error().code == backend::ErrorCode::disconnected);
        CHECK(u.stream->start(71).error().code == backend::ErrorCode::disconnected);
        CHECK(!aravis::with_native_device_for_testing(*u.device, [](ArvDevice*) {}));
        // Repeated close and owner stop/destruction: no device I/O.
        CHECK(u.device->close());
        u.stream->stop();
        u.stream.reset();
        CHECK(seen.command_log().size() == commands + 1);
        // Held bytes stayed valid throughout retirement and close.
        CHECK(copy_bytes(*u.held) == u.held_bytes && pixels_match(*u.held));
        u.held.reset();
        CHECK(seen.live_buffers() == 0);
        // A new session may open; close/reopen is NOT claimed to prove the
        // earlier camera stop (the Fake state is not shared across opens).
        auto reopened = open_fake(*camera);
        CHECK(reopened->close());
    }
}

// M. Recovery ownership across wrappers and generations, plus the lease rule.
void recovery_ownership() {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    ArvDomNode* stop_access = nullptr;
    native(*device, [&](ArvDevice* d) { stop_access = impose_access(d, "AcquisitionStop", "RW"); });
    std::atomic<bool> reject_stop{false};
    seen.on_command = [&](std::string_view c, ArvDevice*) {
        if (c == "AcquisitionStop" && reject_stop.load()) set_access(stop_access, "RO");
    };
    // Generation 1 by `old`, completed; then generation 2 by `owner`.
    auto old = std::move(device->create_stream()).value();
    CHECK(old->start(80));
    old->stop();
    auto owner = std::move(device->create_stream()).value();
    CHECK(owner->start(81));
    auto lease = receive_ok(*owner);
    const auto lease_bytes = copy_bytes(lease);
    // An obsolete wrapper (stop and destruction) cannot stop the newer run.
    auto commands = seen.command_log().size();
    old->stop();
    old.reset();
    CHECK(seen.command_log().size() == commands && acquisition_register(*device) == 1 && seen.live_streams() == 1);
    // The owner's stop is rejected by the SDK: the outcome is uncertain.
    reject_stop = true;
    owner->stop();
    reject_stop = false;
    CHECK(acquisition_register(*device) == 1 && seen.live_streams() == 0);
    // Never-started wrappers cannot recover another wrapper's fault (stop,
    // request_stop, destruction), even once the SDK would accept the command.
    set_access(stop_access, "RW");
    commands = seen.command_log().size();
    {
        auto bystander = std::move(device->create_stream()).value();
        bystander->request_stop();
        bystander->stop();
        CHECK(bystander->start(82).error().code == backend::ErrorCode::invalid_state);
    }
    CHECK(seen.command_log().size() == commands && acquisition_register(*device) == 1);
    CHECK(copy_bytes(lease) == lease_bytes && pixels_match(lease)); // held bytes valid
    // The owner recovers. Recovery does NOT bypass the held-lease rule.
    owner->stop();
    CHECK(seen.command_log().size() == commands + 1 && acquisition_register(*device) == 0);
    const int streams = seen.streams_created.load();
    const auto held = owner->start(83);
    CHECK(!held && held.error().code == backend::ErrorCode::resource_exhausted);
    CHECK(contains(held.error().message, "release them before restarting"));
    CHECK(seen.streams_created.load() == streams);
    CHECK(device->apply(oracle).value() == oracle); // configuration is permitted again
    CHECK(copy_bytes(lease) == lease_bytes && pixels_match(lease));
    { auto drop = std::move(lease); }
    // The unreported historical failure does not block reuse: a different
    // wrapper starts once the current uncertainty is cleared.
    auto next = std::move(device->create_stream()).value();
    CHECK(next->start(84) && acquisition_register(*device) == 1);
    next->stop();
    // Owner destruction cannot silently clear unresolved state: its single
    // destructor attempt is rejected, the outcome stays uncertain, no other
    // wrapper inherits it, and only close() resolves it.
    CHECK(owner->start(85));
    reject_stop = true;
    owner.reset(); // ~AravisStream: retire + one rejected AcquisitionStop
    reject_stop = false;
    CHECK(acquisition_register(*device) == 1 && seen.live_streams() == 0);
    set_access(stop_access, "RW");
    commands = seen.command_log().size();
    next->stop(); // next owns nothing now
    CHECK(seen.command_log().size() == commands);
    CHECK(next->start(86).error().code == backend::ErrorCode::invalid_state);
    CHECK(device->apply(oracle).error().code == backend::ErrorCode::invalid_state);
    const auto closed = device->close(); // logical close has authority
    CHECK(seen.command_log().size() == commands + 1);
    CHECK(!closed && contains(closed.error().message, "recovered: AcquisitionStop during close() succeeded"));
    CHECK(device->close());
}

// Increment child of a feature (explicit <Inc> property node), for arranging
// a fresh grid/increment through the SDK's own property setters.
ArvGcPropertyNode* increment_node(ArvDevice* d, const char* feature) {
    auto* node = ARV_DOM_NODE(arv_device_get_feature(d, feature));
    for (ArvDomNode* child = arv_dom_node_get_first_child(node); child; child = arv_dom_node_get_next_sibling(child))
        if (ARV_IS_GC_PROPERTY_NODE(child) &&
            arv_gc_property_node_get_node_type(ARV_GC_PROPERTY_NODE(child)) == ARV_GC_PROPERTY_NODE_TYPE_INCREMENT)
            return ARV_GC_PROPERTY_NODE(child);
    return nullptr;
}

// N. Fresh acquisition preflight through REAL SDK arrangements (finding C).
// Native facts are arranged and independently read back through the SDK
// BEFORE start(); each rejection has zero stream, buffer, command and write
// deltas (native READS are expected). One case follows a completed run.
void fresh_preflight() {
    Observer seen;
    auto camera = aravis_fake::make_backend(seen.hooks());
    auto device = open_oracle(*camera);
    auto stream = std::move(device->create_stream()).value();
    const auto expect_refused = [&](std::string_view part) {
        const auto before = SideEffects::of(seen);
        const auto refused = stream->start(90);
        CHECK(!refused && refused.error().code == backend::ErrorCode::unsupported);
        if (!contains(refused.error().message, part))
            throw std::runtime_error("expected '" + std::string(part) + "' in: " + refused.error().message);
        CHECK(contains(refused.error().message, "no native stream was created"));
        CHECK(SideEffects::of(seen) == before && acquisition_register(*device) == 0);
    };
    // A completed run, stop and lease release first: preflight is repeated on
    // every later start, not only on the first.
    CHECK(stream->start(89));
    { auto frame = receive_ok(*stream); CHECK(pixels_match(frame)); }
    stream->stop();

    // 1. Current ROI outside the sensor (sensor height 2048, Height 512,
    //    OffsetY 1600), written through the SDK and read back first.
    native(*device, [](ArvDevice* d) {
        GError* error = nullptr;
        arv_device_set_integer_feature_value(d, "Height", 512, &error);
        CHECK(!error);
        arv_device_set_integer_feature_value(d, "OffsetY", 1600, &error);
        CHECK(!error);
    });
    CHECK(native_integer(*device, "SensorHeight") == 2048 && native_integer(*device, "Height") == 512 &&
          native_integer(*device, "OffsetY") == 1600);
    expect_refused("OffsetY reads back 1600 with Height 512, outside the sensor extent 2048");
    native(*device, [](ArvDevice* d) { arv_device_set_integer_feature_value(d, "OffsetY", 4, nullptr); });
    native(*device, [](ArvDevice* d) { arv_device_set_integer_feature_value(d, "Height", 48, nullptr); });
    CHECK(device->configuration().value() == oracle);

    // 2. A malformed fresh limit after a valid open: an explicitly supplied
    //    invalid floating increment. The SDK returns exactly 0 with no GError.
    ArvGcPropertyNode* exposure_inc = nullptr;
    native(*device, [&](ArvDevice* d) {
        auto* real = ARV_GC_FLOAT(arv_device_get_feature(d, "ExposureTimeAbs"));
        auto* document = ARV_DOM_DOCUMENT(arv_device_get_genicam(d));
        auto* inc = ARV_DOM_NODE(arv_dom_document_create_element(document, "Inc"));
        CHECK(inc && arv_dom_node_append_child(ARV_DOM_NODE(real), inc));
        arv_dom_node_append_child(inc, ARV_DOM_NODE(arv_dom_document_create_text_node(document, "0")));
        GError* error = nullptr;
        CHECK(arv_gc_float_get_inc(real, &error) == 0.0);
        CHECK(!error);
        exposure_inc = increment_node(d, "ExposureTimeAbs");
        CHECK(exposure_inc);
    });
    expect_refused("ExposureTimeAbs advertises invalid increment 0 us");
    // Restore the fact through the SDK (a valid explicit increment) and prove it.
    native(*device, [&](ArvDevice* d) {
        GError* error = nullptr;
        arv_gc_property_node_set_double(exposure_inc, 1.0, &error);
        CHECK(!error);
        CHECK(arv_gc_float_get_inc(ARV_GC_FLOAT(arv_device_get_feature(d, "ExposureTimeAbs")), &error) == 1.0);
        CHECK(!error);
    });
    CHECK(stream->start(91) && acquisition_register(*device) == 1);
    stream->stop();

    // 3. A structurally valid profile change: Width increment 1 -> 2 with a
    //    current width (64) divisible by both. Envelope enforcement, not a
    //    malformed limit: the existing close/reopen diagnostic.
    native(*device, [](ArvDevice* d) {
        GError* error = nullptr;
        arv_gc_property_node_set_int64(increment_node(d, "Width"), 2, &error);
        CHECK(!error);
        arv_gc_integer_impose_min(ARV_GC_INTEGER(arv_device_get_feature(d, "Width")), 2, &error);
        CHECK(!error);
        CHECK(arv_gc_integer_get_inc(ARV_GC_INTEGER(arv_device_get_feature(d, "Width")), &error) == 2);
        CHECK(!error);
    });
    CHECK(native_integer(*device, "Width") == 64 && 64 % 2 == 0);
    CHECK(device->capabilities().width_increment == 1); // open-time envelope, immutable
    expect_refused("Width increment changed from 1 at open to 2");
    expect_refused("close and reopen the device");
    CHECK(device->close());
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
        {"D prompt cancellation of a blocked receive, and its failure cleanup (x3)", cancellation},
        {"E lease lifetime, retirement order and reopen", lifetime_and_reopen},
        {"F restart policy", restart_policy},
        {"G native command failure cleanup and recovery", failure_cleanup},
        {"H invalid native facts (SDK arrangements and limits)", invalid_native_facts},
        {"H decoded-facts seam through the production validator", seam_cases},
        {"I exception unwind while receive is blocked", exception_unwind},
        {"J direct capture helper and independent PGM check", direct_capture},
        {"K start outcomes: pre-execution, compensated, uncertain and recovered", start_outcomes},
        {"L uncertain outcome then close without a native stream", uncertain_close},
        {"M recovery ownership across wrappers and the lease rule", recovery_ownership},
        {"N fresh acquisition preflight before side effects", fresh_preflight},
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
