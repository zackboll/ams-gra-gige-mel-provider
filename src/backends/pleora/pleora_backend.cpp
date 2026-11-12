#include "pleora_backend.hpp"
#include "pleora_features.hpp"
#include "pleora_private.hpp"
#include "acquisition_policy.hpp"
#include "profiles/mono_free_running_device.hpp"

#include <PvBaseLib.h>
#include <PvBuffer.h>
#include <PvDeviceEventSink.h>
#include <PvDeviceGEV.h>
#include <PvDeviceInfoGEV.h>
#include <PvGenCommand.h>
#include <PvGenBoolean.h>
#include <PvImage.h>
#include <PvStreamGEV.h>
#include <PvSystem.h>
#include <PvVersion.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace gige_mel::backend::pleora {
namespace {

namespace profile = profiles::mono_free_running;
using Clock = std::chrono::steady_clock;

Error fail(ErrorCode code, std::string message) { return {code, std::move(message)}; }
std::string copy(const PvString& text) {
    const char* value = text.GetAscii();
    return value ? std::string(value) : std::string();
}

std::mutex& discovery_mutex() { static std::mutex mutex; return mutex; }

Status check_sdk_version() {
    const auto compiled = compiled_version();
    const auto runtime = runtime_version();
    if (runtime != compiled)
        return fail(ErrorCode::unsupported, "Pleora runtime version " + runtime + " differs from compiled SDK " +
                                                compiled + "; no discovery or device I/O was attempted");
    return {};
}

// MAC ownership is process-wide across adapter instances. Reservation release
// follows native disconnection/destruction, including all failed-open paths.
class Reservation {
public:
    static Result<std::unique_ptr<Reservation>> acquire(const std::string& id) {
        auto reservation = std::unique_ptr<Reservation>(new Reservation(id));
        std::lock_guard lock(mutex());
        if (ids().count(id)) return fail(ErrorCode::busy, "Pleora device '" + id + "' is already open in this process");
        ids().insert(id);
        reservation->held_ = true;
        return reservation;
    }
    ~Reservation() {
        if (!held_) return;
        std::lock_guard lock(mutex());
        ids().erase(id_);
    }
private:
    explicit Reservation(std::string id) : id_(std::move(id)) {}
    static std::mutex& mutex() { static std::mutex value; return value; }
    static std::set<std::string>& ids() { static std::set<std::string> value; return value; }
    std::string id_;
    bool held_{false};
};

Result<std::string> stable_id(const PvDeviceInfoGEV& info) {
    auto mac = copy(info.GetMACAddress());
    if (mac.size() != 17) return fail(ErrorCode::io_error, "Pleora discovery returned an invalid MAC address: '" + mac + "'");
    for (std::size_t i = 0; i < mac.size(); ++i) {
        char& c = mac[i];
        if (i % 3 == 2) {
            if (c != ':') return fail(ErrorCode::io_error, "Pleora discovery returned a noncanonical MAC address: '" + mac + "'");
        } else if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return fail(ErrorCode::io_error, "Pleora discovery returned an invalid MAC address: '" + mac + "'");
        }
    }
    return "mac:" + mac;
}

// The SDK callback only records a terminal observation. It performs no SDK or
// provider calls and takes no adapter lock. Its owner outlives the native device
// and its callback threads, even if unregister overlaps a selected callback.
struct LinkState final : PvDeviceEventSink {
    std::atomic<bool> lost{false};
    std::atomic<bool> closed{false};
    void OnLinkDisconnected(PvDevice*) override { lost.store(true); }
};

Error connection_lost() {
    return fail(ErrorCode::disconnected,
                "Pleora reported device connection lost; this logical device is terminal; close and reopen explicitly; "
                "camera acquisition state was not independently measured");
}

struct NativeOpen {
    std::unique_ptr<Reservation> reservation;
    std::shared_ptr<LinkState> link{std::make_shared<LinkState>()};
    std::unique_ptr<PvDeviceGEV> device{std::make_unique<PvDeviceGEV>()};
    std::string device_id;
    std::string ip_address;
    std::string display_name;
    std::vector<std::pair<std::string, std::string>> discovery_identity;
    bool registered{false};

    ~NativeOpen() { disconnect(); }
    void disconnect() noexcept {
        if (!device) return;
        try {
            if (registered) (void)device->UnregisterEventSink(link.get());
            registered = false;
            (void)device->Disconnect();
        } catch (...) { /* Native destruction still joins SDK-owned threads. */ }
        device.reset(); // link and reservation remain alive through SDK teardown
    }

    Status connection() {
        if (link->closed.load() || !device) return fail(ErrorCode::disconnected, "Pleora device is logically closed");
        if (link->lost.load()) return connection_lost();
        if (!device->IsConnected()) {
            link->lost.store(true);
            return connection_lost();
        }
        return {};
    }
};

Result<std::unique_ptr<NativeOpen>> open_native(std::string_view requested_id) {
    if (requested_id.empty()) return fail(ErrorCode::invalid_argument, "an explicit, nonempty Pleora MAC device ID is required");
    if (auto version = check_sdk_version(); !version) return version.error();
    std::lock_guard lock(discovery_mutex());
    PvSystem system;
    system.SetDetectionTimeout(discovery_timeout_ms);
    const auto found = system.Find();
    if (!found.IsOK()) return translate_result(found, "discover devices");
    const PvDeviceInfoGEV* selected = nullptr;
    unsigned matches = 0;
    for (std::uint32_t i = 0; i < system.GetDeviceCount(); ++i) {
        const auto* info = dynamic_cast<const PvDeviceInfoGEV*>(system.GetDeviceInfo(i));
        if (!info) continue;
        const auto id = stable_id(*info);
        if (!id) return id.error();
        if (id.value() == requested_id) { selected = info; ++matches; }
    }
    if (matches != 1)
        return fail(ErrorCode::not_found, "Pleora device ID '" + std::string(requested_id) + "' was " +
                                             (matches ? "ambiguous across discovery paths" : "not found") +
                                             "; no IP address, alias or different camera was tried");
    auto native = std::make_unique<NativeOpen>();
    native->device_id = std::string(requested_id);
    native->ip_address = copy(selected->GetIPAddress());
    native->display_name = copy(selected->GetDisplayID());
    native->discovery_identity = {{"DiscoveryVendorName", copy(selected->GetVendorName())},
                                  {"DiscoveryModelName", copy(selected->GetModelName())},
                                  {"DiscoverySerialNumber", copy(selected->GetSerialNumber())},
                                  {"DiscoveryVersion", copy(selected->GetVersion())}};
    auto reservation = Reservation::acquire(native->device_id);
    if (!reservation) return reservation.error();
    native->reservation = std::move(reservation).value();
    if (auto policy = configure_device_policy(*native->device); !policy) return policy.error();
    const auto subscribed = native->device->RegisterEventSink(native->link.get());
    if (!subscribed.IsOK()) return translate_result(subscribed, "register connection-loss observer", native->device_id);
    native->registered = true;
    const auto connected = native->device->Connect(selected, PvAccessControl);
    if (!connected.IsOK()) return translate_result(connected, "connect", native->device_id);
    if (auto status = native->connection(); !status) return status.error();
    return native;
}

enum class SlotState { idle, queued, popped, leased };
struct Slot {
    std::unique_ptr<PvBuffer> buffer;
    SlotState state{SlotState::idle};
};

struct Run {
    Run(std::uint64_t stream_gen, std::uint64_t config_gen, CameraConfiguration config, PixelFormat format,
        std::size_t bytes, std::shared_ptr<LinkState> connection)
        : stream_generation(stream_gen), configuration_generation(config_gen), configuration(config), pixel_format(format),
          buffer_bytes(bytes), link(std::move(connection)) {}

    const std::uint64_t stream_generation;
    const std::uint64_t configuration_generation;
    const CameraConfiguration configuration;
    const PixelFormat pixel_format;
    const std::size_t buffer_bytes;
    const std::shared_ptr<LinkState> link;
    std::atomic<bool> cancel{false};
    std::atomic<bool> closed{false};
    std::mutex mutex; // R: never acquired before the device lock
    std::condition_variable idle;
    std::unique_ptr<PvStreamGEV> stream;
    std::array<Slot, pool_buffers> slots;
    bool retired{false};
    unsigned waiters{0}; // pins native stream and popped buffers through admission
    std::size_t outstanding{0};
    bool starved{false};
    std::optional<Error> queue_error;
    bool queue_failed{false}; // survives allocation failure while translating

    ~Run() {
        // Normal retirement closes the receiver first. This also protects a
        // partially constructed start if allocation throws before publication.
        if (stream) {
            (void)stream->AbortQueuedBuffers();
            (void)stream->Close();
            stream.reset();
        }
    }
};

Error ended(const Run& run) {
    if (run.closed.load() || run.link->closed.load()) return fail(ErrorCode::disconnected, "Pleora device is logically closed");
    if (run.link->lost.load()) return connection_lost();
    return fail(ErrorCode::stopped, "Pleora acquisition is stopped (stream generation " + std::to_string(run.stream_generation) + ")");
}

// R held. Only the slot owner may requeue it. QueueBuffer's PENDING result is
// the documented successful asynchronous queue operation, unlike control I/O.
void recycle_locked(Run& run, std::size_t index) noexcept {
    auto& slot = run.slots[index];
    if (!slot.buffer) return;
    slot.state = SlotState::idle;
    if (run.retired || !run.stream) {
        slot.buffer.reset();
        return;
    }
    if (run.cancel.load() || run.link->lost.load() || run.link->closed.load()) return;
    try {
        const auto queued = run.stream->QueueBuffer(slot.buffer.get());
        if (queued.IsSuccess()) { slot.state = SlotState::queued; return; }
        run.queue_failed = true;
        run.queue_error = translate_result(queued, "requeue buffer");
    } catch (...) { run.queue_failed = true; }
}

struct LeasedBuffer {
    LeasedBuffer(std::shared_ptr<Run> owner, std::size_t slot) : run(std::move(owner)), index(slot) {}
    ~LeasedBuffer() {
        if (!armed) return;
        {
            std::lock_guard lock(run->mutex);
            --run->outstanding;
            run->starved = false;
            recycle_locked(*run, index);
        }
        run->idle.notify_all();
    }
    std::shared_ptr<Run> run;
    std::size_t index;
    bool armed{false};
};

// A receive pins the receiver through decoding/admission, not merely through
// RetrieveBuffer. Retirement can never free a popped buffer being inspected.
class ReceivePin {
public:
    explicit ReceivePin(std::shared_ptr<Run> run) : run_(std::move(run)) {}
    ~ReceivePin() {
        {
            std::lock_guard lock(run_->mutex);
            if (popped_) recycle_locked(*run_, *popped_);
            --run_->waiters;
        }
        run_->idle.notify_all();
    }
    void popped(std::size_t index) { popped_ = index; }
    void leased() { popped_.reset(); }
private:
    std::shared_ptr<Run> run_;
    std::optional<std::size_t> popped_;
};

// Device lock held. Set cancellation before any camera command can block.
// Native stream teardown completes before unleased storage is freed. Leased
// buffers stay owned by the run and are freed by their last explicit release.
std::optional<Error> retire_receiver(const std::shared_ptr<Run>& run) {
    if (!run) return std::nullopt;
    run->cancel.store(true);
    std::unique_lock lock(run->mutex);
    run->retired = true;
    run->idle.wait(lock, [&] { return run->waiters == 0; });
    std::optional<Error> error;
    auto stream = std::move(run->stream);
    if (stream) {
        // An error diagnostic must never prevent destruction of the receiver
        // before its buffers. Retain at least the failure code on allocation
        // failure and continue local teardown in all cases.
        const auto remember = [&](const PvResult& result, std::string_view operation) noexcept {
            if (error || result.IsOK() || result.GetCode() == PvResult::Code::NOT_CONNECTED) return;
            try { error = translate_result(result, operation); }
            catch (...) { error.emplace(Error{ErrorCode::io_error, {}}); }
        };
        try {
            remember(stream->AbortQueuedBuffers(), "abort queued buffers");
            // At most our four buffers can be queued. Never spin indefinitely
            // on a broken SDK count; Close/destruction ends remaining I/O.
            for (std::size_t i = 0; i < pool_buffers && stream->GetQueuedBufferCount() != 0; ++i) {
                PvBuffer* buffer = nullptr;
                PvResult operation;
                const auto retrieved = stream->RetrieveBuffer(&buffer, &operation, 0);
                if (!retrieved.IsOK()) {
                    remember(retrieved, "drain aborted buffers");
                    break;
                }
            }
            remember(stream->Close(), "close native receiver");
        } catch (...) { if (!error) error.emplace(Error{ErrorCode::io_error, {}}); }
        stream.reset();
    }
    for (auto& slot : run->slots) {
        if (slot.state != SlotState::leased) slot.buffer.reset();
    }
    return error;
}

struct DeviceCore {
    DeviceCore(std::unique_ptr<NativeOpen> opened, CameraCapabilities capabilities, profile::NativeSnapshot snapshot)
        : link(opened->link), native(std::move(opened)), caps(capabilities), envelope(std::move(snapshot)) {}
    std::mutex mutex; // D: complete configuration and lifecycle transactions
    const std::shared_ptr<LinkState> link;
    std::unique_ptr<NativeOpen> native;
    const CameraCapabilities caps;
    const profile::NativeSnapshot envelope;
    std::shared_ptr<Run> active;
    std::shared_ptr<Run> latest;
    std::uint64_t owner{0};
    // Stores precede the potentially effective SDK call. Only an explicitly
    // successful stop/unlock clears them; local receiver teardown is no proof.
    bool stop_pending{false};
    bool unlock_pending{false};
    std::optional<Error> fault;

    Status connection() {
        if (!native || link->closed.load()) return fail(ErrorCode::disconnected, "Pleora device is logically closed");
        return native->connection();
    }
};

Status execute_command(DeviceCore& core, std::string_view name, bool starting) {
    if (auto status = core.connection(); !status) return status;
    PleoraFeatures features(*core.native->device->GetParameters());
    const auto command = features.command(name);
    if (!command) return command.error();
    // Command validation itself has no acquisition side effect.
    if (auto status = core.connection(); !status) return status;
    core.stop_pending = true;
    const auto result = command.value()->Execute();
    if (!result.IsOK()) return translate_result(result, "execute command", name);
    if (!starting) core.stop_pending = false;
    return {};
}

// Device lock held; exactly one compensating/explicit stop and unlock attempt.
// The caller retains pending flags even if diagnostic allocation throws.
Status stop_locked(DeviceCore& core) {
    if (core.active) core.active->cancel.store(true);
    std::optional<Error> error;
    if (core.stop_pending) {
        try {
            const auto stopped = execute_command(core, "AcquisitionStop", false);
            if (!stopped) error = stopped.error();
        } catch (...) { error.emplace(Error{ErrorCode::io_error, {}}); }
    }
    if (const auto receiver_error = retire_receiver(core.active); receiver_error && !error) error = receiver_error;
    core.active.reset();
    if (core.unlock_pending) {
        try {
            if (auto connected = core.connection(); !connected) {
                if (!error) error = connected.error();
            } else {
                const auto disabled = core.native->device->StreamDisable();
                if (disabled.IsOK()) core.unlock_pending = false;
                else if (!error) error = translate_result(disabled, "disable stream transport");
            }
        } catch (...) { if (!error) error.emplace(Error{ErrorCode::io_error, {}}); }
    }
    if (!core.stop_pending && !core.unlock_pending) core.owner = 0;
    if (error) return std::move(*error);
    return {};
}

Error uncertain(const DeviceCore& core) {
    return fail(ErrorCode::invalid_state,
                "Pleora acquisition/transport outcome is uncertain (AcquisitionStop pending=" +
                    std::string(core.stop_pending ? "true" : "false") + ", StreamDisable pending=" +
                    (core.unlock_pending ? "true" : "false") + "); explicitly stop or close before reuse; nothing was written");
}

std::atomic<std::uint64_t> stream_generations{0};
std::atomic<std::uint64_t> stream_tokens{0};

void record_fault(DeviceCore& core, const Error& error) noexcept {
    if (core.fault) return;
    try { core.fault = error; }
    catch (...) { core.fault.emplace(Error{error.code, {}}); }
}

// Installed before any streaming side effect. Exceptions cannot lose command
// uncertainty or leave an unpublished native receiver operating indefinitely.
struct StartCleanup {
    DeviceCore& core;
    bool armed{true};
    ~StartCleanup() {
        if (!armed) return;
        try {
            const auto stopped = stop_locked(core);
            if (!stopped) record_fault(core, stopped.error());
        } catch (...) { /* Pending flags and native run ownership survive. */ }
    }
};

profile::NativeFrameFacts decode(PvBuffer& buffer, const Run& run) {
    profile::NativeFrameFacts f;
    if (!buffer.IsHeaderValid() || !buffer.IsTrailerValid()) {
        // Image geometry and payload type are not trustworthy without both
        // native GVSP leader/trailer facts, even if retrieval itself succeeds.
        f.status = profile::NativeStatus::incomplete;
        return f;
    }
    f.status = profile::NativeStatus::success;
    f.raw_status = static_cast<int>(PvResult::Code::OK);
    f.raw_payload = static_cast<int>(buffer.GetPayloadType());
    f.payload = buffer.GetPayloadType() == PvPayloadTypeImage ? profile::NativePayload::image
                : buffer.GetPayloadType() == PvPayloadTypeMultiPart ? profile::NativePayload::multipart
                : buffer.GetPayloadType() == PvPayloadTypeChunkData || buffer.GetPayloadType() == PvPayloadTypeExtendedChunkData
                    ? profile::NativePayload::chunk_or_extended : profile::NativePayload::other;
    f.has_chunks = buffer.HasChunks();
    f.capacity_bytes = buffer.GetSize();
    f.received_bytes = buffer.GetAcquiredSize();
    f.frame_id = buffer.GetBlockID();
    if (f.payload != profile::NativePayload::image) return f;
    const auto* image = buffer.GetImage();
    if (!image) return f;
    f.parts = 1;
    f.part_is_2d_image = true;
    f.pixel_format = static_cast<std::uint32_t>(image->GetPixelType());
    f.width = image->GetWidth();
    f.height = image->GetHeight();
    f.offset_x = image->GetOffsetX();
    f.offset_y = image->GetOffsetY();
    f.padding_x = image->GetPaddingX();
    f.padding_y = image->GetPaddingY();
    f.image_bytes = image->GetImageSize();
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.GetDataPointer());
    const auto data = reinterpret_cast<std::uintptr_t>(image->GetDataPointer());
    f.data_present = base != 0 && data >= base && data - base <= f.capacity_bytes;
    if (f.data_present) f.part_offset = data - base;
    if (f.pixel_format == profile::pfnc_mono12) {
        f.storage_bits = 16;
        f.significant_bits = 12;
        // The unchanged profile admitted this only after explicit native
        // DeviceStreamChannelEndianness=Little readback. No conversion occurs.
        f.byte_order = run.pixel_format == PixelFormat::mono12 ? ByteOrder::little_endian : ByteOrder::not_applicable;
    }
    return f;
}

class PleoraStream final : public IFrameStream {
public:
    explicit PleoraStream(std::shared_ptr<DeviceCore> core)
        : core_(std::move(core)), token_(stream_tokens.fetch_add(1) + 1) {}
    ~PleoraStream() override { stop(); }

    Status start(std::uint64_t configuration_generation) override {
        std::lock_guard lock(core_->mutex);
        auto& core = *core_;
        if (auto connected = core.connection(); !connected) return connected;
        if (core.active) return fail(ErrorCode::invalid_state, "a Pleora acquisition is already active; stop it before starting another");
        if (core.stop_pending || core.unlock_pending) return uncertain(core);
        if (core.latest) {
            std::lock_guard pool_lock(core.latest->mutex);
            if (core.latest->outstanding)
                return fail(ErrorCode::resource_exhausted, "Pleora source leases from the preceding acquisition remain held; release them before restart");
        }
        PleoraFeatures features(*core.native->device->GetParameters());
        const auto snapshot = profile::read_snapshot(features);
        if (!snapshot) return snapshot.error();
        const auto frozen = profile::acquisition_preflight(snapshot.value(), core.envelope);
        if (!frozen) return frozen.error();
        const auto mode = features.read_string_or_enum("AcquisitionMode");
        if (!mode) return mode.error();
        if (mode.value() != profile::acquisition_mode)
            return fail(ErrorCode::unsupported, "AcquisitionMode is '" + mode.value() + "'; the profile requires Continuous, which the adapter does not set");
        for (const auto name : {"AcquisitionStart", "AcquisitionStop"})
            if (auto command = features.command(name); !command) return command.error();
        const auto payload = features.read_integer("PayloadSize");
        if (!payload) return payload.error();
        const auto pool = profile::plan_pool(payload.value(), frozen.value(), pool_buffers,
                                            maximum_frame_bytes, maximum_pool_bytes, core.caps.pixel_format);
        if (!pool) return pool.error();
        if (auto connected = core.connection(); !connected) return connected;

        auto run = std::make_shared<Run>(stream_generations.fetch_add(1) + 1, configuration_generation,
                                         frozen.value(), core.caps.pixel_format, pool.value().buffer_bytes, core.link);
        run->stream = std::make_unique<PvStreamGEV>();
        const auto opened = run->stream->Open(core.native->ip_address.c_str(), 0, 0, PvString(),
                                              static_cast<std::uint32_t>(pool_buffers));
        if (!opened.IsOK()) return translate_result(opened, "open GigE receiver", core.native->device_id);
        if (run->stream->GetQueuedBufferMaximum() < pool_buffers)
            return fail(ErrorCode::resource_exhausted, "Pleora receiver cannot hold the required four-buffer pool");
        for (auto& slot : run->slots) {
            slot.buffer = std::make_unique<PvBuffer>();
            const auto allocated = slot.buffer->Alloc(static_cast<std::uint32_t>(run->buffer_bytes));
            if (!allocated.IsOK()) return translate_result(allocated, "allocate native buffer");
            const auto queued = run->stream->QueueBuffer(slot.buffer.get());
            if (!queued.IsSuccess()) return translate_result(queued, "queue native buffer");
            slot.state = SlotState::queued;
        }
        core.owner = token_;
        core.active = run;
        core.latest = run;
        { std::lock_guard current_lock(current_mutex_); current_ = run; }
        StartCleanup cleanup{core};
        const auto begin = [&]() -> Status {
            if (auto connected = core.connection(); !connected) return connected;
            if (run->cancel.load()) return ended(*run);
            const auto packet = core.native->device->NegotiatePacketSize();
            if (!packet.IsOK()) return translate_result(packet, "negotiate GigE packet size");
            if (auto connected = core.connection(); !connected) return connected;
            if (run->cancel.load()) return ended(*run);
            const auto destination = core.native->device->SetStreamDestination(run->stream->GetLocalIPAddress(),
                                                                               run->stream->GetLocalPort());
            if (!destination.IsOK()) return translate_result(destination, "set GigE stream destination");
            if (auto connected = core.connection(); !connected) return connected;
            if (run->cancel.load()) return ended(*run);
            core.unlock_pending = true; // preserve a possibly effective StreamEnable
            const auto enabled = core.native->device->StreamEnable();
            if (!enabled.IsOK()) return translate_result(enabled, "enable stream transport");
            if (run->cancel.load()) return ended(*run);
            if (auto started = execute_command(core, "AcquisitionStart", true); !started) return started;
            if (auto connected = core.connection(); !connected) return connected;
            if (run->cancel.load()) return ended(*run);
            return {};
        };
        const auto started = begin();
        if (!started) {
            const auto stopped = stop_locked(core);
            cleanup.armed = false;
            auto message = started.error().message;
            if (!stopped) {
                record_fault(core, stopped.error());
                message += "; cleanup failed: " + stopped.error().message;
            }
            if (core.stop_pending || core.unlock_pending) message += "; camera acquisition/transport outcome remains uncertain";
            return fail(started.error().code, std::move(message));
        }
        cleanup.armed = false;
        return {};
    }

    Result<FrameLease> receive(std::chrono::milliseconds timeout) override {
        if (timeout < std::chrono::milliseconds::zero()) return fail(ErrorCode::invalid_argument, "receive timeout must not be negative");
        const auto run = current();
        if (!run) return fail(ErrorCode::stopped, "Pleora stream has not been started");
        const auto entry = Clock::now();
        const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - entry);
        const auto deadline = timeout >= room ? Clock::time_point::max() : entry + timeout;
        std::optional<Error> malformed;
        std::optional<Error> incomplete;
        for (;;) {
            if (run->link->lost.load() || run->link->closed.load() || run->cancel.load()) return ended(*run);
            const auto now = Clock::now();
            const auto left = now >= deadline ? Clock::duration::zero() : deadline - now;
            const auto slice = std::min<Clock::duration>(left, receive_polling_slice);
            PvStreamGEV* stream = nullptr;
            {
                std::unique_lock lock(run->mutex);
                if (run->retired || !run->stream) return ended(*run);
                if (run->queue_failed)
                    return run->queue_error.value_or(fail(ErrorCode::io_error, "Pleora buffer requeue failed"));
                if (run->outstanding == pool_buffers) {
                    if (!run->starved) { run->starved = true; starvation_.fetch_add(1); }
                    if (now >= deadline) break;
                    run->idle.wait_for(lock, slice, [&] { return run->cancel.load() || run->link->lost.load() || run->outstanding < pool_buffers; });
                    continue;
                }
                stream = run->stream.get();
                ++run->waiters;
            }
            ReceivePin pin(run);
            PvBuffer* buffer = nullptr;
            PvResult operation;
            const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(slice).count();
            const auto retrieved = stream->RetrieveBuffer(&buffer, &operation,
                                                          static_cast<std::uint32_t>(slice > Clock::duration::zero() ? std::max<std::int64_t>(millis, 1) : 0));
            if (retrieved.IsOK()) {
                std::size_t index = pool_buffers;
                {
                    std::lock_guard lock(run->mutex);
                    for (std::size_t i = 0; i < run->slots.size(); ++i)
                        if (run->slots[i].buffer.get() == buffer && buffer) { index = i; break; }
                    if (index == pool_buffers || run->slots[index].state != SlotState::queued)
                        return fail(ErrorCode::io_error, "Pleora returned a buffer outside the queued pool");
                    run->slots[index].state = SlotState::popped;
                    pin.popped(index);
                }
                if (run->link->lost.load() || run->link->closed.load() || run->cancel.load()) return ended(*run);
                if (operation.GetCode() == PvResult::Code::NOT_CONNECTED) {
                    run->link->lost.store(true);
                    return connection_lost();
                }
                if (operation.GetCode() == PvResult::Code::NO_LICENSE)
                    return translate_result(operation, "receive licensed image");
                if (operation.GetCode() == PvResult::Code::ABORTED || operation.GetCode() == PvResult::Code::AUTO_ABORTED)
                    return fail(ErrorCode::stopped, "Pleora native receiver aborted the acquisition");
                if (!operation.IsOK()) {
                    switch (operation.GetCode()) {
                    case PvResult::Code::TIMEOUT:
                    case PvResult::Code::MISSING_PACKETS:
                    case PvResult::Code::IMAGE_ERROR:
                    case PvResult::Code::RESENDS_FAILURE:
                    case PvResult::Code::TOO_MANY_RESENDS:
                    case PvResult::Code::TOO_MANY_CONSECUTIVE_RESENDS:
                        incomplete_.fetch_add(1);
                        incomplete = translate_result(operation, "receive incomplete buffer");
                        break;
                    default:
                        return translate_result(operation, "receive buffer");
                    }
                } else {
                    // SDK evaluation mode may return OK after modifying image
                    // pixels with a watermark. Such bytes are not raw camera
                    // output and cannot satisfy this profile's image contract.
                    if (buffer->IsHeaderValid() && buffer->GetPayloadType() == PvPayloadTypeImage &&
                        buffer->GetImage() && buffer->GetImage()->HasWatermark())
                        return fail(ErrorCode::unsupported, "Pleora SDK marked this image with a license watermark; a valid receiver license is required for raw camera frames");
                    const auto facts = decode(*buffer, *run);
                    const auto decision = profile::validate_frame(facts, run->configuration, run->pixel_format);
                    if (decision.verdict == profile::FrameVerdict::accept) {
                        FrameDescriptor descriptor;
                        descriptor.width = run->configuration.width;
                        descriptor.height = run->configuration.height;
                        descriptor.row_stride_bytes = decision.row_stride;
                        descriptor.storage_bits_per_pixel = facts.storage_bits;
                        descriptor.significant_bits_per_pixel = facts.significant_bits;
                        descriptor.pixel_format = run->pixel_format;
                        descriptor.byte_order = facts.byte_order;
                        descriptor.native_frame_id = facts.frame_id;
                        descriptor.stream_generation = run->stream_generation;
                        descriptor.configuration_generation = run->configuration_generation;
                        descriptor.configuration = run->configuration;
                        descriptor.timestamp = FrameTimestamp{}; // raw SDK time is not qualified MEL time
                        const auto* bytes = reinterpret_cast<const std::byte*>(buffer->GetImage()->GetDataPointer());
                        auto owner = std::make_shared<LeasedBuffer>(run, index);
                        {
                            std::lock_guard lock(run->mutex);
                            if (run->retired || run->cancel.load() || run->link->closed.load() || run->link->lost.load())
                                return ended(*run);
                            run->slots[index].state = SlotState::leased;
                            ++run->outstanding;
                            owner->armed = true;
                            pin.leased();
                        }
                        delivered_.fetch_add(1);
                        return FrameLease{descriptor, {bytes, decision.span_bytes}, std::move(owner)};
                    }
                    if (decision.verdict == profile::FrameVerdict::incomplete) {
                        incomplete_.fetch_add(1);
                        incomplete = Error{ErrorCode::io_error, "Pleora buffer lacks valid GVSP header/trailer information"};
                    } else malformed = Error{decision.code, decision.reason};
                }
            } else if (retrieved.GetCode() != PvResult::Code::TIMEOUT) {
                if (retrieved.GetCode() == PvResult::Code::NOT_CONNECTED) run->link->lost.store(true);
                if (run->link->lost.load() || run->cancel.load() || run->link->closed.load()) return ended(*run);
                return translate_result(retrieved, "retrieve buffer");
            }
            if (Clock::now() >= deadline) break;
        }
        if (run->link->lost.load() || run->cancel.load() || run->link->closed.load()) return ended(*run);
        timeouts_.fetch_add(1);
        if (malformed)
            return fail(malformed->code, "no valid Pleora frame before the receive deadline; last rejected complete buffer: " + malformed->message);
        return fail(ErrorCode::timeout, "no valid Pleora frame before the receive deadline" +
                                           (incomplete ? "; last incomplete buffer: " + incomplete->message : std::string()));
    }

    void request_stop() noexcept override {
        const auto run = current();
        if (run) { run->cancel.store(true); run->idle.notify_all(); }
    }

    void stop() noexcept override {
        request_stop();
        std::lock_guard lock(core_->mutex);
        if (!core_->native || core_->owner != token_) return;
        try {
            const auto stopped = stop_locked(*core_);
            if (!stopped) record_fault(*core_, stopped.error());
        } catch (...) {
            record_fault(*core_, Error{ErrorCode::io_error, {}});
        }
    }

    StreamStatistics statistics() const override {
        return {delivered_.load(), incomplete_.load(), timeouts_.load(), starvation_.load()};
    }

private:
    std::shared_ptr<Run> current() const {
        std::lock_guard lock(current_mutex_);
        return current_;
    }
    const std::shared_ptr<DeviceCore> core_;
    const std::uint64_t token_;
    mutable std::mutex current_mutex_;
    std::shared_ptr<Run> current_;
    std::atomic<std::uint64_t> delivered_{0}, incomplete_{0}, timeouts_{0}, starvation_{0};
};

class PleoraDevice final : public ICameraDevice {
public:
    PleoraDevice(std::unique_ptr<NativeOpen> native, CameraCapabilities caps, profile::NativeSnapshot envelope)
        : core_(std::make_shared<DeviceCore>(std::move(native), caps, std::move(envelope))) {}
    ~PleoraDevice() override { try { (void)close(); } catch (...) {} }
    CameraCapabilities capabilities() const override { return core_->caps; }

    Result<CameraConfiguration> configuration() const override {
        std::lock_guard lock(core_->mutex);
        if (auto connected = core_->connection(); !connected) return connected.error();
        PleoraFeatures features(*core_->native->device->GetParameters());
        return profile::read_configuration(features, core_->envelope, [&]() { return core_->connection(); });
    }

    Result<CameraConfiguration> apply(const CameraConfiguration& request) override {
        std::lock_guard lock(core_->mutex);
        if (auto connected = core_->connection(); !connected) return connected.error();
        if (core_->active) return fail(ErrorCode::invalid_state, "a Pleora acquisition is active; stop it before applying configuration; nothing was written");
        if (core_->stop_pending || core_->unlock_pending) return uncertain(*core_);
        PleoraFeatures features(*core_->native->device->GetParameters());
        return profile::apply_configuration(features, request, core_->envelope, [&]() { return core_->connection(); },
                                             [&](std::string_view) { return core_->connection(); });
    }

    Result<std::unique_ptr<IFrameStream>> create_stream() override {
        std::lock_guard lock(core_->mutex);
        if (auto connected = core_->connection(); !connected) return connected.error();
        return std::unique_ptr<IFrameStream>(std::make_unique<PleoraStream>(core_));
    }

    Status close() override {
        std::unique_ptr<NativeOpen> retired;
        std::optional<Error> error;
        bool uncertain_outcome = false;
        {
            std::lock_guard lock(core_->mutex);
            if (!core_->native) return {};
            if (core_->active) { core_->active->closed.store(true); core_->active->cancel.store(true); }
            const auto stopped = stop_locked(*core_);
            if (!stopped) error = stopped.error();
            uncertain_outcome = core_->stop_pending || core_->unlock_pending;
            if (core_->fault) {
                if (error) error->message += "; earlier stop failure: " + core_->fault->message;
                else error = core_->fault;
            }
            core_->link->closed.store(true);
            retired = std::move(core_->native);
            core_->owner = 0;
            core_->fault.reset();
        }
        if (retired->registered) {
            const auto unsubscribed = retired->device->UnregisterEventSink(retired->link.get());
            retired->registered = false;
            if (!unsubscribed.IsOK() && !error) error = translate_result(unsubscribed, "unregister connection-loss observer");
        }
        const auto disconnected = retired->device->Disconnect();
        if (!disconnected.IsOK() && !error) error = translate_result(disconnected, "disconnect device");
        retired->device.reset(); // joins native threads while the callback target still lives
        retired.reset(); // release reservation only after all native device I/O
        if (uncertain_outcome) {
            if (!error) error = fail(ErrorCode::io_error, "Pleora acquisition/transport cleanup did not establish a successful stop");
            error->message += "; the camera may still be acquiring; native resources were released and the device was logically closed";
        }
        if (error) return *error;
        return {};
    }

private:
    const std::shared_ptr<DeviceCore> core_;
};

class PleoraBackend final : public ICameraBackend {
public:
    std::string_view id() const noexcept override { return backend_id; }
    Result<std::vector<DeviceInfo>> enumerate() override {
        if (auto version = check_sdk_version(); !version) return version.error();
        std::lock_guard lock(discovery_mutex());
        PvSystem system;
        system.SetDetectionTimeout(discovery_timeout_ms);
        const auto found = system.Find();
        if (!found.IsOK()) return translate_result(found, "discover devices");
        std::vector<DeviceInfo> devices;
        for (std::uint32_t i = 0; i < system.GetDeviceCount(); ++i) {
            const auto* info = dynamic_cast<const PvDeviceInfoGEV*>(system.GetDeviceInfo(i));
            if (!info) continue;
            auto device_id = stable_id(*info);
            if (!device_id) return device_id.error();
            devices.push_back({std::string(backend_id), std::move(device_id).value(),
                               copy(info->GetVendorName()) + " " + copy(info->GetModelName()) + " serial " +
                                   copy(info->GetSerialNumber()) + " (GigE Vision " + copy(info->GetIPAddress()) + ")"});
        }
        return devices;
    }
    Result<std::shared_ptr<ICameraDevice>> open(std::string_view device_id) override {
        auto native = open_native(device_id);
        if (!native) return native.error();
        PleoraFeatures features(*native.value()->device->GetParameters());
        const auto snapshot = profile::read_snapshot(features);
        if (auto connected = native.value()->connection(); !connected) return connected.error();
        if (!snapshot) return snapshot.error();
        const auto caps = profile::capabilities_from(snapshot.value());
        if (!caps) return caps.error();
        return std::shared_ptr<ICameraDevice>(std::make_shared<PleoraDevice>(std::move(native).value(), caps.value(), snapshot.value()));
    }
};

Result<std::optional<std::string>> read_optional_identity(NativeOpen& native, PleoraFeatures& features,
                                                         std::string_view name) {
    const auto exists = features.implemented(name);
    if (!exists) return exists.error();
    if (!exists.value()) return std::optional<std::string>{};
    auto* parameter = native.device->GetParameters()->Get(std::string(name).c_str());
    if (!parameter) return fail(ErrorCode::unsupported, "Pleora identity feature '" + std::string(name) + "' has no eBUS wrapper");
    PvGenType type = PvGenTypeUndefined;
    const auto kind = parameter->GetType(type);
    if (!kind.IsOK()) return translate_result(kind, "query identity node type", name);
    if (type == PvGenTypeInteger) {
        const auto value = features.read_integer(name);
        if (!value) return value.error();
        return std::optional<std::string>{std::to_string(value.value()) + " (GenICam Integer node)"};
    }
    // The typed service reports an unsupported native type/access failure;
    // none is converted into optional absence or a fabricated identity.
    auto value = features.read_string_or_enum(name);
    if (!value) return value.error();
    return std::optional<std::string>{std::move(value).value()};
}

} // namespace

std::shared_ptr<ICameraBackend> make_backend() { return std::make_shared<PleoraBackend>(); }

std::string compiled_version() {
    return std::to_string(VERSION_MAJOR) + "." + std::to_string(VERSION_MINOR) + "." +
           std::to_string(VERSION_SUB) + "." + std::to_string(VERSION_BUILD);
}
std::string runtime_version() {
    return std::to_string(PvGetVersionMajor()) + "." + std::to_string(PvGetVersionMinor()) + "." +
           std::to_string(PvGetVersionSub()) + "." + std::to_string(PvGetVersionBuild());
}

Status configure_device_policy(PvDeviceGEV& device) {
    constexpr std::string_view name = "LinkRecoveryEnabled";
    auto* parameters = device.GetCommunicationParameters();
    if (!parameters) return fail(ErrorCode::unsupported, "Pleora SDK has no local communication parameters");
    auto* parameter = parameters->Get("LinkRecoveryEnabled");
    if (!parameter) return fail(ErrorCode::unsupported, "Pleora SDK has no LinkRecoveryEnabled policy; automatic recovery cannot be disabled explicitly");
    bool value = false;
    const auto implemented = parameter->IsImplemented(value);
    if (!implemented.IsOK()) return translate_result(implemented, "query local SDK policy implementation", name);
    if (!value) return fail(ErrorCode::unsupported, "Pleora SDK LinkRecoveryEnabled policy is not implemented");
    const auto available = parameter->IsAvailable(value);
    if (!available.IsOK()) return translate_result(available, "query local SDK policy availability", name);
    if (!value) return fail(ErrorCode::unsupported, "Pleora SDK LinkRecoveryEnabled policy is unavailable");
    const auto readable = parameter->IsReadable(value);
    if (!readable.IsOK()) return translate_result(readable, "query local SDK policy read access", name);
    if (!value) return fail(ErrorCode::unsupported, "Pleora SDK LinkRecoveryEnabled policy is not readable");
    const auto writable = parameter->IsWritable(value);
    if (!writable.IsOK()) return translate_result(writable, "query local SDK policy write access", name);
    if (!value) return fail(ErrorCode::unsupported, "Pleora SDK LinkRecoveryEnabled policy is not writable");
    auto* recovery = dynamic_cast<PvGenBoolean*>(parameter);
    if (!recovery) return fail(ErrorCode::unsupported, "Pleora SDK LinkRecoveryEnabled policy is not Boolean");
    const auto disabled = recovery->SetValue(false);
    if (!disabled.IsOK()) return translate_result(disabled, "disable local SDK automatic link recovery", name);
    const auto actual = recovery->GetValue(value);
    if (!actual.IsOK()) return translate_result(actual, "verify local SDK automatic link recovery disabled", name);
    if (value) return fail(ErrorCode::io_error, "Pleora SDK LinkRecoveryEnabled stayed true after disabling; no connection was attempted");
    return {};
}

Result<Inspection> inspect(ICameraBackend& backend, std::string_view device_id) {
    if (!dynamic_cast<PleoraBackend*>(&backend)) return fail(ErrorCode::invalid_argument, "inspection requires a Pleora backend");
    auto native = open_native(device_id);
    if (!native) return native.error();
    Inspection result;
    result.device_id = native.value()->device_id;
    result.interface_protocol = "GigE Vision";
    result.native.emplace_back("MACAddress", result.device_id.substr(4));
    result.native.emplace_back("IPAddress", native.value()->ip_address);
    result.native.emplace_back("DiscoveryDisplayName", native.value()->display_name);
    result.native.insert(result.native.end(), native.value()->discovery_identity.begin(), native.value()->discovery_identity.end());
    PleoraFeatures features(*native.value()->device->GetParameters());
    for (const auto name : {"DeviceVendorName", "DeviceModelName", "DeviceSerialNumber", "DeviceID", "DeviceVersion",
                           "DeviceFirmwareVersion", "DeviceManufacturerInfo", "AcquisitionMode", "PayloadSize"}) {
        auto value = read_optional_identity(*native.value(), features, name);
        if (!value) {
            result.native.emplace_back(name, "<error: " + value.error().message + ">");
            result.problems.push_back(value.error().message);
        } else if (value.value()) {
            result.native.emplace_back(name, *value.value());
        }
    }
    auto snapshot = profile::read_snapshot(features);
    if (auto connected = native.value()->connection(); !connected) return connected.error();
    if (!snapshot) { result.problems.push_back(snapshot.error().message); return result; }
    const auto& n = snapshot.value();
    result.native.emplace_back("SensorWidth", std::to_string(n.sensor_width));
    result.native.emplace_back("SensorHeight", std::to_string(n.sensor_height));
    for (const auto& [name, value] : std::array<std::pair<std::string_view, profile::IntegerFeature>, 4>{{
             {profile::width_feature, n.width}, {profile::height_feature, n.height},
             {profile::offset_x_feature, n.offset_x}, {profile::offset_y_feature, n.offset_y}}}) {
        result.native.emplace_back(name, std::to_string(value.value) + " [" + std::to_string(value.minimum) + ", " +
                                          std::to_string(value.maximum) + "] inc " + std::to_string(value.increment));
    }
    const auto add_float = [&](const std::string& name, const profile::FloatFeature& value) {
        char text[192];
        std::snprintf(text, sizeof text, "%.17g [%.17g, %.17g]", value.value, value.minimum, value.maximum);
        result.native.emplace_back(name, std::string(text) + (value.unit ? " unit " + *value.unit : "") +
                                          (value.increment ? " inc " + std::to_string(*value.increment) : " no increment"));
    };
    add_float(n.frame_rate_feature, n.frame_rate_hz);
    add_float(n.exposure_feature, n.exposure_us);
    result.native.emplace_back("PixelFormat", n.pixel_format);
    result.native.emplace_back("DeviceStreamChannelEndianness", n.pixel_endianness.value_or("<absent>"));
    result.native.emplace_back("TriggerSelector", n.trigger_selector.value_or("<absent>"));
    result.native.emplace_back("TriggerMode", n.trigger_mode.value_or("<absent>"));
    result.native.emplace_back("ExposureAuto", n.exposure_auto.value_or("<absent>"));
    result.native.emplace_back("AcquisitionFrameRateEnable", n.frame_rate_enable ? (*n.frame_rate_enable ? "true" : "false") : "<absent>");
    if (const auto caps = profile::capabilities_from(n)) result.capabilities = caps.value();
    else result.problems.push_back(caps.error().message);
    if (const auto config = profile::configuration_from(n)) result.configuration = config.value();
    else result.problems.push_back(config.error().message);
    return result;
}

} // namespace gige_mel::backend::pleora
