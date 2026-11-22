#include "aravis_stream.hpp"
#include "acquisition_policy.hpp"
#include "aravis_features.hpp"
#include "aravis_private.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <utility>

namespace gige_mel::backend::aravis {
namespace detail {

namespace {

namespace profile = profiles;

// ---------------------------------------------------------------------------
// Acquisition helpers (GIGE-MEL-009). Callers hold the device lock.

// A command must be a GenICam Command node that is implemented, available
// and not read-only. Nothing is executed here.
Result<ArvGcCommand*> command_node(ArvDevice* device, std::string_view name) {
    const std::string feature(name);
    ArvGcNode* node = arv_device_get_feature(device, feature.c_str());
    if (!node || !ARV_IS_GC_COMMAND(node))
        return fail(ErrorCode::unsupported, "required command '" + feature + "' is not present as a GenICam command");
    auto* feature_node = ARV_GC_FEATURE_NODE(node);
    ErrorSlot error;
    const bool implemented = arv_gc_feature_node_is_implemented(feature_node, error.out());
    if (error.failed()) return translate("query implementation of", feature, *error.take());
    const bool available = implemented && arv_gc_feature_node_is_available(feature_node, error.out());
    if (error.failed()) return translate("query availability of", feature, *error.take());
    if (!implemented || !available)
        return fail(ErrorCode::unsupported, "required command '" + feature + "' is not implemented/available");
    const auto mode = arv_gc_feature_node_get_actual_access_mode(feature_node);
    if (mode != ARV_GC_ACCESS_MODE_WO && mode != ARV_GC_ACCESS_MODE_RW)
        return fail(ErrorCode::unsupported, "command '" + feature + "' has access mode " +
                                                std::to_string(static_cast<int>(mode)) + "; write access is required");
    return ARV_GC_COMMAND(node);
}

// `outcome` becomes `uncertain` immediately BEFORE the SDK call that may have
// a camera side effect. It is a plain store (no allocation), so the state
// survives any exception while the error diagnostic is built. A failure
// before that point (command missing, unavailable or not writable; a
// throwing test hook) leaves `outcome` unchanged: no command was attempted.
// No SDK error is classified as side-effect-free: once the executor was
// called, the caller decides the outcome only on success.
Status execute_command(ArvDevice* device, std::string_view name, const TestHooks* hooks, Acquisition& outcome) {
    auto node = command_node(device, name);
    if (!node) return node.error();
    if (hooks && hooks->before_command) hooks->before_command(name, device);
    ErrorSlot error;
    outcome = Acquisition::uncertain;
    arv_gc_command_execute(node.value(), error.out());
    if (error.failed()) return translate("execute", name, *error.take());
    if (hooks && hooks->command_result)
        if (auto substituted = hooks->command_result(name, device)) return std::move(*substituted);
    return {};
}

// Public-accessor decoding of one popped buffer. Image accessors are called
// only when their documented precondition (successful 2-D image part) holds,
// so no GLib critical is provoked by an unexpected payload.
profile::NativeFrameFacts decode(ArvBuffer* buffer, std::size_t capacity) {
    profile::NativeFrameFacts f;
    f.capacity_bytes = capacity;
    f.frame_id = arv_buffer_get_frame_id(buffer);
    const ArvBufferStatus status = arv_buffer_get_status(buffer);
    f.raw_status = static_cast<int>(status);
    switch (status) {
    case ARV_BUFFER_STATUS_SUCCESS: f.status = profile::NativeStatus::success; break;
    case ARV_BUFFER_STATUS_TIMEOUT:
    case ARV_BUFFER_STATUS_MISSING_PACKETS:
    case ARV_BUFFER_STATUS_WRONG_PACKET_ID:
    case ARV_BUFFER_STATUS_ABORTED: f.status = profile::NativeStatus::incomplete; break;
    default: f.status = profile::NativeStatus::other; break; // size mismatch, unsupported, cleared, ...
    }
    if (f.status != profile::NativeStatus::success) return f;
    const ArvBufferPayloadType payload = arv_buffer_get_payload_type(buffer);
    f.raw_payload = static_cast<int>(payload);
    f.payload = payload == ARV_BUFFER_PAYLOAD_TYPE_IMAGE ? profile::NativePayload::image
              : payload == ARV_BUFFER_PAYLOAD_TYPE_MULTIPART ? profile::NativePayload::multipart
              : (payload == ARV_BUFFER_PAYLOAD_TYPE_CHUNK_DATA || payload == ARV_BUFFER_PAYLOAD_TYPE_EXTENDED_CHUNK_DATA)
                  ? profile::NativePayload::chunk_or_extended
                  : profile::NativePayload::other;
    f.has_chunks = arv_buffer_has_chunks(buffer);
    f.parts = arv_buffer_get_n_parts(buffer);
    if (f.payload != profile::NativePayload::image || f.parts < 1) return f;
    f.part_is_2d_image = arv_buffer_get_part_data_type(buffer, 0) == ARV_BUFFER_PART_DATA_TYPE_2D_IMAGE;
    if (!f.part_is_2d_image) return f;
    f.pixel_format = static_cast<std::uint32_t>(arv_buffer_get_image_pixel_format(buffer));
    if (f.pixel_format == profile::pfnc_mono12) {
        f.storage_bits = 16;
        f.significant_bits = 12;
        // Native DeviceStreamChannelEndianness=Little was required at
        // open/start; PFNC supplies alignment, NOT endianness. SDK copies bytes.
        f.byte_order = ByteOrder::little_endian;
    }
    gint x = 0, y = 0, w = 0, h = 0, px = 0, py = 0;
    arv_buffer_get_image_region(buffer, &x, &y, &w, &h);
    arv_buffer_get_image_padding(buffer, &px, &py);
    f.offset_x = x; f.offset_y = y; f.width = w; f.height = h; f.padding_x = px; f.padding_y = py;
    std::size_t received = 0, image = 0;
    const auto* base = static_cast<const unsigned char*>(arv_buffer_get_data(buffer, &received));
    const auto* data = static_cast<const unsigned char*>(arv_buffer_get_image_data(buffer, &image));
    f.received_bytes = received;
    f.image_bytes = image;
    if (base && data && data >= base) {
        f.data_present = true;
        f.part_offset = static_cast<std::size_t>(data - base);
    }
    return f;
}

// ---------------------------------------------------------------------------
// GVSP socket receive buffer (GIGE-MEL-019); docs/gvsp-receive-buffer.md.
// Only public SDK accessors and ordinary per-process OS facts are used: the
// stream's own port, the device's reported interface address, this
// process's /proc/self/fd entries, getsockname and getsockopt(SO_RCVBUF).

// What a GigE run requests and what the receive path later measures. The
// identification facts are captured under the device lock in start(), so the
// receiving thread never touches the ArvDevice.
std::optional<std::int64_t> read_rmem_max() {
    std::FILE* file = std::fopen("/proc/sys/net/core/rmem_max", "r");
    if (!file) return std::nullopt;
    long long value = -1;
    const bool ok = std::fscanf(file, "%lld", &value) == 1 && value >= 0;
    std::fclose(file);
    return ok ? std::optional<std::int64_t>(value) : std::nullopt;
}

struct SocketLookup {
    std::optional<std::int64_t> kernel_reported_bytes;
    std::string problem; // set when kernel_reported_bytes is absent
};

// Exactly one UDP IPv4 socket of this process bound to address:port.
// Ambiguity, absence or a failed query is a problem, never a guess.
SocketLookup lookup_receive_buffer(std::uint32_t address, std::uint16_t port) {
    SocketLookup result;
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) {
        result.problem = std::string("cannot list /proc/self/fd: ") + std::strerror(errno);
        return result;
    }
    const int own = dirfd(directory);
    int matches = 0;
    int value = 0;
    while (const dirent* entry = readdir(directory)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char* end = nullptr;
        const long parsed = std::strtol(entry->d_name, &end, 10);
        if (!end || *end != '\0' || parsed < 0 || parsed > std::numeric_limits<int>::max()) continue;
        const int fd = static_cast<int>(parsed);
        if (fd == own) continue;
        int type = 0, protocol = 0;
        socklen_t length = sizeof type;
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 || type != SOCK_DGRAM) continue;
        length = sizeof protocol;
        if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &length) != 0 || protocol != IPPROTO_UDP) continue;
        sockaddr_in local{};
        socklen_t local_length = sizeof local;
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length) != 0 || local.sin_family != AF_INET)
            continue;
        if (ntohs(local.sin_port) != port || local.sin_addr.s_addr != address) continue;
        int reported = 0;
        length = sizeof reported;
        if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &reported, &length) != 0) {
            result.problem = std::string("getsockopt(SO_RCVBUF) failed: ") + std::strerror(errno);
            closedir(directory);
            return result;
        }
        ++matches;
        value = reported;
    }
    closedir(directory);
    char text[INET_ADDRSTRLEN] = "?";
    in_addr in{};
    in.s_addr = address;
    inet_ntop(AF_INET, &in, text, sizeof text);
    const std::string where = std::string(text) + ":" + std::to_string(port);
    if (matches == 1) result.kernel_reported_bytes = value;
    else if (matches == 0) result.problem = "no UDP IPv4 socket of this process is bound to " + where;
    else result.problem = std::to_string(matches) + " UDP sockets of this process are bound to " + where + " (ambiguous)";
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Acquisition ownership (GIGE-MEL-009); docs/aravis-integration.md,
// "Acquisition ownership and retirement", is the normative description.
//
// Strong references (no cycles):
//   AravisDevice ──┐
//   AravisStream ──┴─> DeviceCore ─> Run (active / most recent) ─> ArvStream ─> ArvDevice
//   AravisStream ────> Run (its own most recent)                  (until retirement)
//   lease owner (LeasedBuffer) ─> Run, and owns exactly one popped ArvBuffer
// A Run references no DeviceCore, wrapper or lease. Retirement drops
// Run::stream, so outstanding leases keep only their own SDK buffer storage
// and the small Run record, never the native stream, receiver or device.
//
// Locks: DeviceCore::mutex (D) serializes native feature I/O and every
// acquisition transition (start, stop/retirement, close). Run::mutex (R)
// guards the run's stream pointer, retirement, in-flight native waits and
// lease accounting. AravisStream::mutex_ (W) guards only the wrapper's run
// pointer and is held briefly, never while acquiring D or R. Order: D -> R.
// receive() and lease release take only R and never wait for D, so stop() and
// close(), which hold D while waiting (at most one polling slice) for in-flight
// native waits and while the SDK finalizer joins its receiver, cannot deadlock
// with them. No native wait happens while R is held.

struct ReceiveBufferProbe {
    std::int64_t requested_bytes{0};
    std::optional<std::int64_t> sdk_property_bytes;
    std::uint16_t port{0};
    std::optional<std::uint32_t> interface_ipv4; // network byte order
    std::string setup_problem;                   // non-empty: unobservable from the start
};

struct Run {
    Run(std::uint64_t stream_gen, std::uint64_t config_gen, const CameraConfiguration& frozen, std::size_t buffers,
        std::size_t bytes)
        : stream_generation(stream_gen), configuration_generation(config_gen), configuration(frozen),
          buffer_count(buffers), buffer_bytes(bytes) {}
    const std::uint64_t stream_generation;
    const std::uint64_t configuration_generation;
    const CameraConfiguration configuration; // frozen from start()'s fresh readback
    PixelFormat pixel_format{PixelFormat::mono8}; // fixed before run publication
    const std::size_t buffer_count;
    const std::size_t buffer_bytes;

    std::atomic<bool> cancel{false}; // request_stop/stop/close: receive returns promptly
    std::mutex mutex;                // R
    std::condition_variable idle;    // waiters dropped to zero
    GObjectPtr<ArvStream> stream;    // null after retirement
    bool retired{false};
    std::atomic<bool> closed{false}; // retired by logical device close
    unsigned waiters{0};             // receive slices inside a native pop
    std::size_t outstanding{0};      // pool buffers held by leases
    bool starved{false};             // current pool-starvation episode already counted

    // GIGE-MEL-019. Written by start() under D before publication, then only
    // by the receiving thread, which claims the single measurement with
    // `receive_buffer_claimed`. Readers copy `receive_buffer` under R.
    ReceiveBufferProbe receive_buffer_probe;              // immutable after start()
    ReceiveBufferObservation receive_buffer;              // R
    std::atomic<bool> receive_buffer_claimed{false};      // at most one measurement per native stream
};

std::uint64_t active_generation_locked(const DeviceCore& core) noexcept {
    return core.active->stream_generation;
}

// One bounded AcquisitionStop attempt (device lock held, device present): a
// single SDK command, no internal retry loop. Success clears the current
// uncertainty and marks an unreported historical failure recovered. Any
// failure - before or after execution - leaves the outcome `uncertain`,
// because the caller is retiring (or has retired) the receiver while the
// camera may still be acquiring.
Status stop_command_locked(DeviceCore& core, const char* recovered_by) {
    if (core.lost()) return control_loss_error(); // no command/reclaim after confirmed loss
    auto stopped = execute_command(core.device.get(), "AcquisitionStop", core.hooks.get(), core.acquisition);
    if (!stopped) {
        core.acquisition = Acquisition::uncertain;
        return stopped;
    }
    core.acquisition = Acquisition::none;
    core.owner = 0;
    if (core.fault && !core.fault->recovered_by) core.fault->recovered_by = recovered_by;
    return {};
}

namespace {

// Keeps the first unrecovered failure of the void stop() interface for one
// report by close(). A new failure after a recovered one is chained onto it,
// so neither episode is lost and the report reflects the latest outcome.
void record_fault(DeviceCore& core, Error error) {
    if (core.fault && !core.fault->recovered_by) return; // same unresolved episode
    if (core.fault)
        error.message = core.fault->error.message + "; recovered by " + core.fault->recovered_by + "; later: " +
                        error.message;
    core.fault = DeviceCore::Fault{std::move(error), nullptr};
}

} // namespace

// Retires core.active (device lock held, device reference still present when
// `closing` is false or close() has not yet dropped it). Order:
//   1. cancel: receive returns within one polling slice;
//   2. AcquisitionStop through the SDK; a failure leaves the outcome
//      `uncertain` (never hidden) and is recorded for close(), or handed to
//      close() itself through `close_failure`;
//   3. mark retired, take the stream out, and wait (at most one slice) until
//      no receive is inside a native pop; lease releases now free storage;
//   4. drop the last native-stream reference: the SDK finalizer joins its own
//      receiver (Fake) or stops the receiver and writes the stream-channel
//      register (GigE) while this device's lock and reservation are held;
//      queued buffers are freed by that finalizer, leased ones by their lease.
// No second explicit thread stop is issued; arv_shutdown() is never called.
// The receiver is retired whatever the command outcome. Until AcquisitionStop
// succeeds the outcome is `uncertain` (plain stores made before the command,
// so an exception while describing a failure cannot lose them); the run's
// wrapper stays the recovery owner.
void retire_locked(DeviceCore& core, bool closing, std::optional<Error>* close_failure) noexcept {
    const std::shared_ptr<Run> run = std::move(core.active);
    if (!run) return;
    if (closing) run->closed.store(true); // before cancel: receive reports disconnected
    run->cancel.store(true);
    core.acquisition = Acquisition::uncertain;
    core.attempted = "AcquisitionStop";
    core.attempted_generation = run->stream_generation;
    if (core.device && !core.lost()) {
        try {
            if (auto stopped = stop_command_locked(core, "a later successful AcquisitionStop"); !stopped) {
                Error failure{stopped.error().code, stopped.error().message + " (stream generation " +
                                                        std::to_string(run->stream_generation) +
                                                        "); local stream resources were retired anyway"};
                if (close_failure) *close_failure = std::move(failure);
                else record_fault(core, std::move(failure));
            }
        } catch (...) {
            // Allocation failure while describing the error: the outcome
            // stays `uncertain`; keep retiring.
        }
    }
    GObjectPtr<ArvStream> stream;
    {
        std::unique_lock lock(run->mutex);
        run->retired = true;
        run->idle.wait(lock, [&] { return run->waiters == 0; });
        stream = std::move(run->stream);
    }
    stream.reset(); // synchronous SDK finalization (sole strong reference)
}

// Admission refusal while an acquisition-command outcome is uncertain (device
// lock held). Built only on the refusal path; `what` says what was refused.
Error uncertain_refusal(const DeviceCore& core, const std::string& what) {
    std::string message = "the outcome of " + std::string(core.attempted ? core.attempted : "an acquisition command") +
                          " (stream generation " + std::to_string(core.attempted_generation) +
                          ") is uncertain, so the camera may still be acquiring; configuration and acquisition are "
                          "refused until stop() on the stream that owns this outcome succeeds in AcquisitionStop, or "
                          "the device is closed; " + what;
    if (core.fault && !core.fault->recovered_by) message += "; last failure: " + core.fault->error.message;
    return fail(ErrorCode::invalid_state, std::move(message));
}

namespace {

using Clock = std::chrono::steady_clock;
// Longest single native timed pop; bounds cancellation latency.
constexpr std::chrono::milliseconds receive_polling_slice{5};
// Process-wide, so generations are distinct across wrappers and devices.
std::atomic<std::uint64_t> stream_generations{0};

// The FrameLease owner: the Run record plus exactly one popped SDK buffer
// (transfer full from arv_stream_*pop_buffer). No wrapper, device or stream
// pointer is captured. Release is nonthrowing and runs on any thread:
//   run still active -> arv_stream_push_buffer() requeues it exactly once;
//   run retired      -> g_object_unref() frees it exactly once.
// A retired run has no stream, so the buffer can never enter a later run.
struct LeasedBuffer {
    LeasedBuffer(std::shared_ptr<Run> owner, ArvBuffer* leased) noexcept : run(std::move(owner)), buffer(leased) {}
    ~LeasedBuffer() {
        std::lock_guard lock(run->mutex);
        if (!run->retired && run->stream) arv_stream_push_buffer(run->stream.get(), buffer);
        else g_object_unref(buffer);
        --run->outstanding;
        run->starved = false;
    }
    LeasedBuffer(const LeasedBuffer&) = delete;
    LeasedBuffer& operator=(const LeasedBuffer&) = delete;
    std::shared_ptr<Run> run;
    ArvBuffer* buffer;
};

// GIGE-MEL-019. Device lock held; the native stream exists and no buffer is
// queued yet. A non-GigE stream is left completely untouched. For an
// ArvGvStream: FIXED policy with the request, then the public facts the
// measurement needs. Nothing here can fail acquisition: a fact that cannot be
// read makes the later observation `unknown`.
void configure_receive_buffer(Run& run, ArvDevice* device, const TestHooks& hooks) {
    ArvStream* stream = run.stream.get();
    ReceiveBufferObservation& seen = run.receive_buffer;
    seen.stream_generation = run.stream_generation;
    if (!ARV_IS_GV_STREAM(stream) || !ARV_IS_GV_DEVICE(device)) {
        seen.state = ReceiveBufferState::not_applicable;
        seen.detail = std::string("not a GigE Vision stream (") + G_OBJECT_TYPE_NAME(stream) +
                      "); no receive-buffer property was set";
        return;
    }
    auto& probe = run.receive_buffer_probe;
    probe.requested_bytes = hooks.gvsp_receive_buffer_request_bytes.value_or(gvsp_receive_buffer_request_bytes);
    probe.requested_bytes = std::clamp<std::int64_t>(probe.requested_bytes, 1, std::numeric_limits<gint>::max());
    g_object_set(stream, "socket-buffer", ARV_GV_STREAM_SOCKET_BUFFER_FIXED, "socket-buffer-size",
                 static_cast<gint>(probe.requested_bytes), nullptr);
    gint policy = -1, size = -1;
    g_object_get(stream, "socket-buffer", &policy, "socket-buffer-size", &size, nullptr);
    probe.sdk_property_bytes = size;
    if (policy != ARV_GV_STREAM_SOCKET_BUFFER_FIXED || size != probe.requested_bytes)
        probe.setup_problem = "the SDK property readback (policy " + std::to_string(policy) + ", size " +
                              std::to_string(size) + ") differs from the request";
    probe.port = arv_gv_stream_get_port(ARV_GV_STREAM(stream));
    GSocketAddress* local = arv_gv_device_get_interface_address(ARV_GV_DEVICE(device)); // transfer none
    if (local && G_IS_INET_SOCKET_ADDRESS(local)) {
        GInetAddress* address = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(local));
        if (address && g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4 &&
            g_inet_address_get_native_size(address) == sizeof(std::uint32_t)) {
            std::uint32_t raw = 0;
            std::memcpy(&raw, g_inet_address_to_bytes(address), sizeof raw);
            probe.interface_ipv4 = raw;
        }
    }
    if (probe.setup_problem.empty() && !probe.interface_ipv4)
        probe.setup_problem = "the device reported no IPv4 interface address for the stream socket";
    if (probe.setup_problem.empty() && probe.port == 0) probe.setup_problem = "the SDK reported stream port 0";
    seen.state = ReceiveBufferState::pending;
    seen.requested_bytes = probe.requested_bytes;
    seen.sdk_property_bytes = probe.sdk_property_bytes;
    seen.detail = "requested; measured after the first received buffer of this stream";
}

// Receiving thread, NO adapter lock held, at most once per native stream
// (after the first popped buffer, when the pinned SDK has applied the size).
// The stream is pinned for the call by the caller's waiter count.
void measure_receive_buffer(Run& run, const TestHooks& hooks) {
    if (run.receive_buffer_claimed.exchange(true)) return;
    const auto& probe = run.receive_buffer_probe;
    {
        std::lock_guard lock(run.mutex);
        if (run.receive_buffer.state != ReceiveBufferState::pending) return; // not GigE
    }
    std::optional<std::int64_t> reported;
    std::string problem = probe.setup_problem;
    if (problem.empty()) {
        std::uint16_t port = probe.port;
        if (hooks.receive_buffer_lookup_port) port = hooks.receive_buffer_lookup_port(port);
        auto found = lookup_receive_buffer(*probe.interface_ipv4, port);
        reported = found.kernel_reported_bytes;
        problem = found.problem;
    }
    auto observation = assess_receive_buffer(run.stream_generation, probe.requested_bytes, probe.sdk_property_bytes,
                                             reported, read_rmem_max(), problem);
    const auto warning = receive_buffer_warning(observation);
    observation.warned = !warning.empty();
    {
        std::lock_guard lock(run.mutex);
        run.receive_buffer = observation;
    }
    if (!warning.empty()) {
        std::cerr << warning << std::endl;
        if (hooks.receive_buffer_warning) hooks.receive_buffer_warning(warning);
    }
}

// Process-wide, never reused, never 0: a stable private recovery identity for
// each wrapper, compared under the device lock. It is a number, not a
// pointer, so no dangling reference or ownership cycle can arise.
std::atomic<std::uint64_t> stream_tokens{0};

// Lightweight wrapper. create_stream() performs no native work; start()
// creates a FRESH native stream and pool for every run. Several wrappers may
// exist for one device, but DeviceCore::active admits one acquisition at a
// time and the restart lease rule is per device, so no wrapper bypasses them.
// Statistics are per wrapper and accumulate across that wrapper's runs.
class AravisStream final : public IFrameStream {
public:
    explicit AravisStream(std::shared_ptr<DeviceCore> core)
        : core_(std::move(core)), token_(stream_tokens.fetch_add(1) + 1) {}
    // The caller has joined its receiving thread. Destruction makes the same
    // bounded attempt as stop(); if it fails, the outcome stays uncertain and
    // is NOT handed to another wrapper: only close() can then resolve it.
    ~AravisStream() override { stop(); }
    AravisStream(const AravisStream&) = delete;
    AravisStream& operator=(const AravisStream&) = delete;

    Status start(std::uint64_t configuration_generation) override;
    Result<FrameLease> receive(std::chrono::milliseconds timeout) override { return receive_observed(timeout, nullptr); }
    // GIGE-MEL-010 read-only observation: the SAME receive; when `observed`
    // is non-null and a lease is returned, it holds SDK facts read from that
    // lease's buffer on this (receiving) thread. Never touches the timestamp
    // in the descriptor, the buffer, the pool or the release order.
    Result<FrameLease> receive_observed(std::chrono::milliseconds timeout, NativeTimestampObservation* observed);

    void request_stop() noexcept override {
        if (const auto run = current()) run->cancel.store(true);
    }

    // Retires this wrapper's active run (receiver always retired; a failed
    // AcquisitionStop leaves the outcome uncertain). Explicit recovery: when
    // the outcome THIS wrapper owns is uncertain and no native stream remains,
    // one bounded AcquisitionStop attempt is made; success clears the
    // uncertainty, failure keeps it. A wrapper that does not own the current
    // outcome (never started, obsolete, or another wrapper's) does nothing.
    // The restart lease rule is unaffected.
    void stop() noexcept override {
        std::lock_guard lock(core_->mutex);
        DeviceCore& core = *core_;
        if (const auto run = current()) {
            run->cancel.store(true);
            if (core.active == run) {
                retire_locked(core, false);
                return;
            }
        }
        if (core.lost() || !core.device || core.active || core.acquisition != Acquisition::uncertain || core.owner != token_) return;
        try {
            if (auto stopped = stop_command_locked(core, "an explicit stop() retry"); !stopped)
                record_fault(core, Error{stopped.error().code,
                                         stopped.error().message + " (explicit stop() retry; generation " +
                                             std::to_string(core.attempted_generation) + ")"});
        } catch (...) {
            // Allocation failure while describing the error: still uncertain.
        }
    }

    // GIGE-MEL-019: copy of the most recent run's observation (no I/O).
    Result<ReceiveBufferObservation> receive_buffer() const {
        const auto run = current();
        if (!run) return fail(ErrorCode::stopped, "Aravis stream has not been started");
        std::lock_guard lock(run->mutex);
        return run->receive_buffer;
    }

    StreamStatistics statistics() const override {
        StreamStatistics s;
        s.delivered_frames = delivered_.load();
        s.incomplete_frames = incomplete_.load();
        s.timeout_count = timeouts_.load();
        s.pool_starvation_events = starvation_.load();
        return s;
    }

private:
    std::shared_ptr<Run> current() const noexcept {
        std::lock_guard lock(mutex_);
        return run_;
    }
    static Error ended(const Run& run) {
        return run.closed ? fail(ErrorCode::disconnected, "Aravis device was logically closed during acquisition")
                          : fail(ErrorCode::stopped, "Aravis stream generation " + std::to_string(run.stream_generation) +
                                                         " is stopped");
    }
    // Returns a popped buffer to its run (requeue while active, else free).
    static void recycle(Run& run, ArvBuffer* buffer) noexcept {
        std::lock_guard lock(run.mutex);
        if (!run.retired && run.stream) arv_stream_push_buffer(run.stream.get(), buffer);
        else g_object_unref(buffer);
    }

    const std::shared_ptr<DeviceCore> core_;
    const std::uint64_t token_; // private recovery identity (see stream_tokens)
    mutable std::mutex mutex_;  // W: guards run_ only
    std::shared_ptr<Run> run_;
    std::atomic<std::uint64_t> delivered_{0}, incomplete_{0}, timeouts_{0}, starvation_{0};
};

// Start order (device lock held throughout, so apply()/close()/other starts
// are serialized and see either no run or a fully published one):
//   reject closed / active acquisition / uncertain acquisition outcome /
//   outstanding leases of the previous run -> READ-ONLY PREFLIGHT of the
//   fresh snapshot (profile preconditions, structural limits, current ROI
//   geometry, the open-envelope comparison, representability) ->
//   AcquisitionMode Continuous, commands present and writable, PayloadSize ->
//   checked pool plan -> native stream (receiver starts) -> allocate & queue
//   the fixed pool -> AcquisitionStart -> publish.
//
// Failure handling (GIGE-MEL-009-R1):
//  - Before the SDK command executor is called (preflight, preparation,
//    allocation, a missing/unwritable command): no command was attempted.
//    The partial native stream/pool is retired, the outcome stays `none`, and
//    an explicit retry/configuration remains available.
//  - After AcquisitionStart was attempted: an SDK error does not prove the
//    command was unapplied. The ORIGINAL start error is preserved; one
//    compensating AcquisitionStop is attempted while the device and its
//    reservation are held; the partial stream/pool is retired whatever that
//    yields. If the stop succeeds, the result is still the start failure and
//    later explicit use is permitted. If it fails, the outcome stays
//    `uncertain`, owned by this wrapper for explicit stop() recovery, and the
//    diagnostic carries both failures. AcquisitionStart is never retried.
Status AravisStream::start(std::uint64_t configuration_generation) {
    std::lock_guard device_lock(core_->mutex);
    DeviceCore& core = *core_;
    if (core.lost()) return control_loss_error();
    if (!core.device) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
    if (core.active)
        return fail(ErrorCode::busy, "an acquisition is already active on this Aravis device (stream generation " +
                                         std::to_string(core.active->stream_generation) +
                                         "); stop it before starting another; no native stream was created");
    if (core.acquisition == Acquisition::uncertain)
        return uncertain_refusal(core, "no native stream was created and no command was sent");
    if (core.latest) {
        std::lock_guard run_lock(core.latest->mutex);
        if (core.latest->outstanding != 0)
            return fail(ErrorCode::resource_exhausted,
                        std::to_string(core.latest->outstanding) + " source lease(s) from stream generation " +
                            std::to_string(core.latest->stream_generation) +
                            " are still held; release them before restarting; no native stream was created");
    }
    ArvDevice* device = core.device.get();
    AravisDeviceFeatures features(device);
    auto frozen = core.policy->preflight(features);
    if (!frozen) return fail(frozen.error().code, frozen.error().message + "; no native stream was created");
    for (const char* command : {"AcquisitionStart", "AcquisitionStop"})
        if (auto node = command_node(device, command); !node) return node.error();
    auto payload = features.read_integer("PayloadSize");
    if (!payload) return payload.error();
    auto pool = profiles::plan_pool(payload.value(), frozen.value(), pool_buffers,
                                   maximum_frame_bytes, maximum_pool_bytes, core.caps.pixel_format);
    if (!pool) return pool.error();
    if (core.lost()) return control_loss_error(); // last admission check before native stream creation

    auto run = std::make_shared<Run>(stream_generations.fetch_add(1) + 1, configuration_generation, frozen.value(),
                                     pool.value().buffers, pool.value().buffer_bytes);
    run->pixel_format = core.caps.pixel_format;
    {
        ErrorSlot error;
        run->stream.reset(arv_device_create_stream(device, nullptr, nullptr, error.out()));
        if (error.failed()) return translate("create stream on", "", *error.take());
        if (!run->stream) return fail(ErrorCode::io_error, "Aravis returned no stream object; nothing was started");
    }
    // Local partial-setup retirement: cancel, retire, and drop the sole
    // native-stream reference; the SDK finalizer stops its receiver and frees
    // every queued pool buffer. Nothing was published; nothing was written.
    const auto retire_partial = [&]() noexcept {
        run->cancel.store(true);
        GObjectPtr<ArvStream> stream;
        {
            std::lock_guard lock(run->mutex);
            run->retired = true;
            stream = std::move(run->stream);
        }
        stream.reset();
    };
    // After an ATTEMPTED AcquisitionStart: compensate, then retire. The
    // outcome was set to `uncertain` by execute_command() before the SDK
    // call; this wrapper is recorded as the recovery owner before anything
    // that allocates, so an exception cannot leave an unowned uncertainty.
    bool compensated = false; // at most ONE compensating attempt per start()
    const auto compensate = [&]() noexcept -> std::optional<Error> {
        compensated = true;
        core.owner = token_;
        core.attempted = "AcquisitionStart";
        core.attempted_generation = run->stream_generation;
        std::optional<Error> cleanup;
        try {
            if (auto stopped = stop_command_locked(core, "a compensating AcquisitionStop"); !stopped)
                cleanup = stopped.error();
        } catch (...) {
            core.acquisition = Acquisition::uncertain; // the diagnostic failed, not the state
        }
        retire_partial();
        return cleanup;
    };
    const auto attempted_failure = [&](const Error& primary) -> Status {
        const auto cleanup = compensate();
        const std::string retired = "; the native stream and " + std::to_string(run->buffer_count) +
                                    " queued buffers were released; configuration is unchanged";
        if (core.acquisition != Acquisition::uncertain)
            return fail(primary.code, primary.message + retired +
                                          "; a compensating AcquisitionStop succeeded; retry start() explicitly");
        // History keeps both failures but no current-state claim, so a later
        // report can truthfully say "recovered"; the returned error adds it.
        Error history{primary.code, primary.message + retired + "; the compensating AcquisitionStop also failed: " +
                                        (cleanup ? cleanup->message : std::string("(no diagnostic)"))};
        record_fault(core, history);
        history.message += "; the camera may still be acquiring; configuration and start() are refused until stop() "
                           "on this stream succeeds, or the device is closed";
        return history;
    };
    try {
        if (core.hooks->after_stream_created) core.hooks->after_stream_created(run->stream.get());
        // GIGE-MEL-019: receive-buffer policy, after creation and before any
        // buffer is queued, on a GigE Vision stream only.
        configure_receive_buffer(*run, device, *core.hooks);
        for (std::size_t i = 0; i < run->buffer_count; ++i) {
            GObjectPtr<ArvBuffer> buffer(arv_buffer_new_allocate(run->buffer_bytes));
            if (!buffer) {
                retire_partial();
                return fail(ErrorCode::resource_exhausted, "Aravis buffer allocation failed; no command was sent");
            }
            if (core.hooks->after_buffer_allocated) core.hooks->after_buffer_allocated(buffer.get());
            arv_stream_push_buffer(run->stream.get(), buffer.release()); // transfer full
        }
        if (core.lost()) {
            retire_partial();
            return control_loss_error();
        }
        if (auto ok = execute_command(device, "AcquisitionStart", core.hooks.get(), core.acquisition); !ok) {
            if (core.acquisition != Acquisition::uncertain) {
                // The executor was never called: nothing to compensate.
                retire_partial();
                return fail(ok.error().code, ok.error().message +
                                                 "; AcquisitionStart was not executed; the native stream and queued "
                                                 "buffers were released; retry start() explicitly");
            }
            return attempted_failure(ok.error());
        }
        {
            std::lock_guard lock(mutex_);
            run_ = run;
        }
        core.active = run;
        core.latest = run;
        core.acquisition = Acquisition::owned;
        core.owner = token_;
        core.attempted = "AcquisitionStart";
        core.attempted_generation = run->stream_generation;
        if (core.lost()) {
            retire_locked(core, false);
            return control_loss_error();
        }
        return {};
    } catch (...) {
        // Unexpected exception (test hook or allocation). If the executor was
        // reached, compensate first; otherwise only retire locally.
        if (!compensated && core.acquisition == Acquisition::uncertain && core.active != run) (void)compensate();
        else retire_partial();
        throw;
    }
}

// One overall steady-clock deadline. Negative timeouts are invalid_argument;
// zero performs one non-blocking poll (no native wait). Each native wait is a
// timed pop of at most receive_polling_slice (the SDK unit is microseconds),
// so cancellation and close are observed within one slice. A null pop is a
// transient wait result, never evidence of disconnection. Incomplete and
// malformed buffers are recycled and the loop continues within the SAME
// deadline. timeout_count counts expired public calls, not slices.
Result<FrameLease> AravisStream::receive_observed(std::chrono::milliseconds timeout,
                                                 NativeTimestampObservation* observed) {
    if (timeout < std::chrono::milliseconds::zero())
        return fail(ErrorCode::invalid_argument, "receive timeout must not be negative");
    if (core_->lost()) return control_loss_error();
    const auto run = current();
    if (!run) return fail(ErrorCode::stopped, "Aravis stream has not been started");
    const auto entry = Clock::now();
    // Saturating deadline: a huge timeout cannot overflow the time point.
    const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - entry);
    const auto deadline = timeout >= room ? Clock::time_point::max() : entry + timeout;
    const auto& hooks = core_->hooks;
    std::optional<Error> malformed; // last rejected complete buffer, reported on expiry
    for (;;) {
        if (core_->lost()) return control_loss_error();
        if (run->cancel.load()) return ended(*run);
        // Borrowed: retirement waits for waiters == 0 before it drops the
        // run's reference, so the stream outlives this slice. No reference is
        // taken here, so this thread can never run the SDK finalizer.
        ArvStream* stream = nullptr;
        std::size_t outstanding = 0;
        {
            std::lock_guard lock(run->mutex);
            if (run->retired || !run->stream) return ended(*run);
            stream = run->stream.get();
            ++run->waiters;
            outstanding = run->outstanding;
        }
        const auto now = Clock::now();
        const auto remaining = now >= deadline ? Clock::duration::zero() : deadline - now;
        const auto slice = std::min<Clock::duration>(remaining, receive_polling_slice);
        ArvBuffer* popped = nullptr;
        if (slice <= Clock::duration::zero()) {
            popped = arv_stream_try_pop_buffer(stream);
        } else {
            try {
                if (hooks->before_native_wait) hooks->before_native_wait();
            } catch (...) {
                { std::lock_guard lock(run->mutex); --run->waiters; }
                run->idle.notify_all();
                throw;
            }
            if (core_->lost()) {
                { std::lock_guard lock(run->mutex); --run->waiters; }
                run->idle.notify_all();
                return control_loss_error();
            }
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(slice).count();
            popped = arv_stream_timeout_pop_buffer(stream, static_cast<guint64>(std::max<std::int64_t>(micros, 1)));
        }
        // GIGE-MEL-019: one bounded measurement per native stream, while this
        // slice's waiter count still pins the stream (and so its socket)
        // against retirement. No adapter lock is held across the lookup. It
        // can throw only on allocation; the popped buffer is then returned to
        // its run exactly like a decoded_facts exception (requeued while
        // active, freed after retirement), so the pool never shrinks.
        if (popped && !run->receive_buffer_claimed.load()) {
            try {
                measure_receive_buffer(*run, *hooks);
            } catch (...) {
                { std::lock_guard lock(run->mutex); --run->waiters; }
                run->idle.notify_all();
                recycle(*run, popped);
                throw;
            }
        }
        bool starved_now = false;
        {
            std::lock_guard lock(run->mutex);
            --run->waiters;
            // Pool starvation: nothing arrived while every pool buffer was held
            // by a lease for the whole slice. Counted once per episode; a
            // release clears the episode.
            if (!popped && outstanding == run->buffer_count && run->outstanding == run->buffer_count &&
                !run->starved) {
                run->starved = true;
                starved_now = true;
            }
        }
        run->idle.notify_all();
        if (core_->lost()) {
            if (popped) g_object_unref(popped); // transfer-full, never deliver/requeue this result
            return control_loss_error();
        }
        if (starved_now) starvation_.fetch_add(1);
        if (popped) {
            GObjectPtr<ArvBuffer> buffer(popped); // transfer full from the pop
            auto facts = decode(buffer.get(), run->buffer_bytes);
            if (hooks->decoded_facts) {
                try {
                    hooks->decoded_facts(facts);
                } catch (...) {
                    recycle(*run, buffer.release());
                    throw;
                }
            }
            const auto decision = core_->policy->validate_frame(facts, run->configuration, run->pixel_format);
            if (decision.verdict == profile::FrameVerdict::accept) {
                const auto* data = static_cast<const std::byte*>(arv_buffer_get_image_data(buffer.get(), nullptr));
                FrameDescriptor d;
                d.width = run->configuration.width;
                d.height = run->configuration.height;
                d.row_stride_bytes = decision.row_stride;
                d.storage_bits_per_pixel = facts.storage_bits;
                d.significant_bits_per_pixel = facts.significant_bits;
                d.bands = 1;
                d.pixel_format = run->pixel_format;
                d.byte_order = facts.byte_order;
                d.status = FrameStatus::complete;
                d.native_frame_id = facts.frame_id; // SDK value, unmodified
                d.stream_generation = run->stream_generation;
                d.configuration_generation = run->configuration_generation;
                d.configuration = run->configuration;
                d.timestamp = FrameTimestamp{}; // deliberately unqualified (GIGE-MEL-009); observation never changes it
                // GIGE-MEL-010 observation: constant-time public getters on the
                // still-owned buffer, read directly from the SDK (not from the
                // possibly test-edited decoded facts). Unsigned values are
                // copied unconverted; nothing is written to the buffer.
                NativeTimestampObservation native;
                if (observed) {
                    gint x = 0, y = 0, w = 0, h = 0;
                    arv_buffer_get_image_region(buffer.get(), &x, &y, &w, &h);
                    native.stream_generation = run->stream_generation;
                    native.native_frame_id = arv_buffer_get_frame_id(buffer.get());
                    // Accepted geometry equals the frozen unsigned ROI, so the
                    // SDK's gint values are nonnegative here.
                    native.width = static_cast<std::uint32_t>(w);
                    native.height = static_cast<std::uint32_t>(h);
                    native.offset_x = static_cast<std::uint32_t>(x);
                    native.offset_y = static_cast<std::uint32_t>(y);
                    native.sdk_buffer_timestamp_ns = arv_buffer_get_timestamp(buffer.get());
                    native.sdk_system_timestamp_ns = arv_buffer_get_system_timestamp(buffer.get());
                }
                {
                    std::lock_guard lock(run->mutex);
                    // Admission linearizes at this atomic observation. A loss
                    // observed later cannot retroactively revoke this lease.
                    if (core_->lost()) return control_loss_error();
                    // Retired after the pop: the guard frees the buffer.
                    if (run->retired) return ended(*run);
                    ++run->outstanding;
                }
                std::shared_ptr<const LeasedBuffer> owner;
                try {
                    owner = std::make_shared<const LeasedBuffer>(run, buffer.get());
                } catch (...) {
                    { std::lock_guard lock(run->mutex); --run->outstanding; }
                    throw; // the guard requeues nothing; it frees the buffer
                }
                (void)buffer.release(); // owned by the lease from here on
                delivered_.fetch_add(1);
                if (observed) *observed = native;
                return FrameLease{d, std::span<const std::byte>(data, decision.span_bytes), std::move(owner)};
            }
            if (decision.verdict == profile::FrameVerdict::incomplete) incomplete_.fetch_add(1);
            else malformed = Error{decision.code, decision.reason};
            recycle(*run, buffer.release());
        }
        if (Clock::now() >= deadline) break;
    }
    if (core_->lost()) return control_loss_error();
    timeouts_.fetch_add(1);
    if (malformed)
        return fail(ErrorCode::timeout, "no valid frame before the receive deadline; last rejected buffer: " +
                                            malformed->message);
    return fail(ErrorCode::timeout, "no valid frame before the receive deadline (stream generation " +
                                        std::to_string(run->stream_generation) + ")");
}

} // namespace

std::unique_ptr<IFrameStream> make_stream(std::shared_ptr<DeviceCore> core) {
    return std::make_unique<AravisStream>(std::move(core));
}

} // namespace detail

Result<ObservedFrame> receive_with_native_timestamps(IFrameStream& stream, std::chrono::milliseconds timeout) {
    auto* concrete = dynamic_cast<detail::AravisStream*>(&stream);
    if (!concrete) return detail::fail(ErrorCode::invalid_argument, "timestamp observation requires a stream created by the Aravis adapter");
    NativeTimestampObservation native;
    auto received = concrete->receive_observed(timeout, &native);
    if (!received) return received.error();
    return ObservedFrame{std::move(received).value(), native};
}

std::string_view receive_buffer_state_name(ReceiveBufferState state) {
    switch (state) {
    case ReceiveBufferState::not_applicable: return "not_applicable";
    case ReceiveBufferState::pending: return "pending";
    case ReceiveBufferState::sufficient: return "sufficient";
    case ReceiveBufferState::insufficient: return "insufficient";
    case ReceiveBufferState::unknown: return "unknown";
    }
    return "unknown";
}

ReceiveBufferObservation assess_receive_buffer(std::uint64_t stream_generation, std::int64_t requested_bytes,
                                               std::optional<std::int64_t> sdk_property_bytes,
                                               std::optional<std::int64_t> kernel_reported_bytes,
                                               std::optional<std::int64_t> rmem_max_bytes,
                                               const std::string& lookup_problem) {
    ReceiveBufferObservation o;
    o.stream_generation = stream_generation;
    o.requested_bytes = requested_bytes;
    o.sdk_property_bytes = sdk_property_bytes;
    o.kernel_reported_bytes = kernel_reported_bytes;
    o.rmem_max_bytes = rmem_max_bytes;
    if (requested_bytes <= 0) {
        o.state = ReceiveBufferState::unknown;
        o.kernel_reported_bytes.reset();
        o.detail = "no positive receive-buffer request was recorded";
        return o;
    }
    if (!kernel_reported_bytes || *kernel_reported_bytes <= 0) {
        o.state = ReceiveBufferState::unknown;
        o.kernel_reported_bytes.reset();
        o.detail = lookup_problem.empty() ? std::string("the GVSP socket's SO_RCVBUF could not be observed")
                                          : "the GVSP socket's SO_RCVBUF could not be observed: " + lookup_problem;
        return o;
    }
    // Linux doubles the requested value for its own bookkeeping (socket(7)).
    o.granted_bytes = *kernel_reported_bytes / 2;
    const bool enough = *kernel_reported_bytes / 2 >= requested_bytes;
    o.state = enough ? ReceiveBufferState::sufficient : ReceiveBufferState::insufficient;
    o.detail = "kernel SO_RCVBUF " + std::to_string(*kernel_reported_bytes) + " (" +
               std::to_string(*o.granted_bytes) + " bytes granted, Linux reports twice the grant) " +
               (enough ? ">= " : "< ") + std::to_string(requested_bytes) + " bytes requested";
    if (!enough && rmem_max_bytes && *rmem_max_bytes < requested_bytes)
        o.detail += "; net.core.rmem_max " + std::to_string(*rmem_max_bytes) + " is below the request";
    return o;
}

std::string receive_buffer_warning(const ReceiveBufferObservation& o) {
    if (o.state != ReceiveBufferState::insufficient && o.state != ReceiveBufferState::unknown) return {};
    const auto bytes = [](const std::optional<std::int64_t>& v) {
        return v ? std::to_string(*v) : std::string("unknown");
    };
    std::string text = "gige_mel aravis WARNING: GVSP receive buffer " +
                       std::string(receive_buffer_state_name(o.state)) + " for stream generation " +
                       std::to_string(o.stream_generation) + ": requested " + std::to_string(o.requested_bytes) +
                       " bytes, granted " + bytes(o.granted_bytes) + " bytes (kernel SO_RCVBUF " +
                       bytes(o.kernel_reported_bytes) + "), net.core.rmem_max " + bytes(o.rmem_max_bytes) + "; " +
                       o.detail + ". Acquisition continues; packet loss under host load is more likely";
    if (o.state == ReceiveBufferState::insufficient)
        text += ". See docs/gvsp-receive-buffer.md for the host prerequisite";
    return text;
}

Result<ReceiveBufferObservation> receive_buffer_observation(IFrameStream& stream) {
    auto* concrete = dynamic_cast<detail::AravisStream*>(&stream);
    if (!concrete)
        return detail::fail(ErrorCode::invalid_argument, "receive-buffer observation requires a stream created by the Aravis adapter");
    return concrete->receive_buffer();
}

} // namespace gige_mel::backend::aravis
