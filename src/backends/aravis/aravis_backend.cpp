// Aravis SDK mechanics for the optional camera-control foundation. Profile
// translation (units, bounds, write planning) lives in camera_profile.cpp.
//
// Synchronization boundary: the adapter serializes ITS OWN discovery/open per
// Aravis interface (process-wide, across adapter instances) and its own
// feature I/O per device. It cannot make unrelated application calls into
// Aravis thread-safe. Ownership: one logically open device per (interface,
// device ID) per process, across adapter instances; a second open is `busy`.
#include "aravis_private.hpp"
#include "camera_profile.hpp"

#include <arv.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <functional>
#include <utility>

namespace gige_mel::backend::aravis {
namespace {

namespace profile = aravis_profile;

Error fail(ErrorCode code, std::string message) { return Error{code, std::move(message)}; }

struct ObjectUnref {
    void operator()(gpointer object) const noexcept { if (object) g_object_unref(object); }
};
template <class T> using GObjectPtr = std::unique_ptr<T, ObjectUnref>;

struct ErrorFree {
    void operator()(GError* error) const noexcept { if (error) g_error_free(error); }
};
using GErrorPtr = std::unique_ptr<GError, ErrorFree>;

// Collects one GError; owns it on every path.
class ErrorSlot {
public:
    GError** out() noexcept { return &raw_; }
    ~ErrorSlot() { if (raw_) g_error_free(raw_); }
    ErrorSlot() = default;
    ErrorSlot(const ErrorSlot&) = delete;
    ErrorSlot& operator=(const ErrorSlot&) = delete;
    [[nodiscard]] bool failed() const noexcept { return raw_ != nullptr; }
    GErrorPtr take() noexcept { return GErrorPtr(std::exchange(raw_, nullptr)); }

private:
    GError* raw_{nullptr};
};

std::string copy(const char* text) { return text ? std::string(text) : std::string(); }

ErrorCode translate_code(const GError& error) {
    if (error.domain == arv_device_error_quark()) {
        switch (error.code) {
        case ARV_DEVICE_ERROR_TIMEOUT: return ErrorCode::timeout;
        case ARV_DEVICE_ERROR_NOT_CONNECTED: return ErrorCode::disconnected;
        case ARV_DEVICE_ERROR_NOT_FOUND: return ErrorCode::not_found;
        case ARV_DEVICE_ERROR_NOT_CONTROLLER: return ErrorCode::busy;
        case ARV_DEVICE_ERROR_FEATURE_NOT_FOUND:
        case ARV_DEVICE_ERROR_WRONG_FEATURE: return ErrorCode::unsupported;
        default: return ErrorCode::io_error;
        }
    }
    if (error.domain == arv_gc_error_quark()) {
        switch (error.code) {
        case ARV_GC_ERROR_OUT_OF_RANGE:
        case ARV_GC_ERROR_ENUM_ENTRY_NOT_FOUND: return ErrorCode::invalid_argument;
        case ARV_GC_ERROR_NODE_NOT_FOUND:
        case ARV_GC_ERROR_READ_ONLY:
        case ARV_GC_ERROR_PROPERTY_NOT_DEFINED:
        case ARV_GC_ERROR_PVALUE_NOT_DEFINED: return ErrorCode::unsupported;
        default: return ErrorCode::io_error;
        }
    }
    return ErrorCode::io_error;
}

Error translate(std::string_view operation, std::string_view feature, const GError& error) {
    std::string message = "Aravis " + std::string(operation);
    if (!feature.empty()) message += " '" + std::string(feature) + "'";
    message += ": " + copy(error.message) + " [" + copy(g_quark_to_string(error.domain)) + " " +
               std::to_string(error.code) + "]";
    return fail(translate_code(error), std::move(message));
}

// Process-wide registries shared by every adapter instance.
std::mutex& interface_mutex(const std::string& interface_name) {
    static std::mutex registry;
    static std::map<std::string, std::mutex> mutexes; // node-stable
    std::lock_guard lock(registry);
    return mutexes[interface_name];
}

class Reservation {
public:
    // Exception-safe: the object exists before the key is inserted, and the
    // destructor erases only a key this object actually inserted.
    static Result<std::unique_ptr<Reservation>> acquire(const std::string& key) {
        auto reservation = std::unique_ptr<Reservation>(new Reservation(key));
        std::lock_guard lock(mutex());
        if (keys().count(key))
            return fail(ErrorCode::busy, "device is already open in this process (ownership key '" + key +
                                             "'); close it before opening it again");
        keys().insert(key);
        reservation->held_ = true;
        return reservation;
    }
    static bool held(const std::string& key) {
        std::lock_guard lock(mutex());
        return keys().count(key) != 0;
    }
    ~Reservation() {
        if (!held_) return;
        std::lock_guard lock(mutex());
        keys().erase(key_);
    }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

private:
    explicit Reservation(std::string key) : key_(std::move(key)) {}
    static std::mutex& mutex() { static std::mutex m; return m; }
    static std::set<std::string>& keys() { static std::set<std::string> k; return k; }
    std::string key_;
    bool held_{false};
};

// ---------------------------------------------------------------------------
// Native feature access. Callers hold the device operation lock. Node pointers
// are borrowed from the device's GenICam tree and never escape a call.
Result<ArvGcFeatureNode*> feature_node(ArvDevice* device, std::string_view name, bool writable) {
    const std::string feature(name);
    ArvGcNode* node = arv_device_get_feature(device, feature.c_str());
    if (!node || !ARV_IS_GC_FEATURE_NODE(node))
        return fail(ErrorCode::unsupported, "required feature '" + feature + "' is not present");
    auto* feature_node = ARV_GC_FEATURE_NODE(node);
    ErrorSlot error;
    const bool implemented = arv_gc_feature_node_is_implemented(feature_node, error.out());
    if (error.failed()) return translate("query implementation of", feature, *error.take());
    const bool available = implemented && arv_gc_feature_node_is_available(feature_node, error.out());
    if (error.failed()) return translate("query availability of", feature, *error.take());
    if (!implemented || !available)
        return fail(ErrorCode::unsupported, "required feature '" + feature + "' is not implemented/available");
    const auto mode = arv_gc_feature_node_get_actual_access_mode(feature_node);
    const bool readable = mode == ARV_GC_ACCESS_MODE_RO || mode == ARV_GC_ACCESS_MODE_RW;
    if (!readable || (writable && mode != ARV_GC_ACCESS_MODE_RW))
        return fail(ErrorCode::unsupported, "feature '" + feature + "' has access mode " + std::to_string(static_cast<int>(mode)) +
                                                (writable ? "; read/write access is required" : "; read access is required"));
    return feature_node;
}

// Optional features. Three distinct outcomes:
//   - Result value false: confirmed absent (no node) or the SDK successfully
//     reported "not implemented". Supported optional absence.
//   - Result value true: present and implemented.
//   - Result error: the implementation query itself failed (translated GError
//     with feature and operation context), or a node exists under the name
//     but is not a GenICam feature node. Never converted into absence.
Result<bool> present(ArvDevice* device, std::string_view name) {
    const std::string feature(name);
    ArvGcNode* node = arv_device_get_feature(device, feature.c_str());
    if (!node) return false;
    if (!ARV_IS_GC_FEATURE_NODE(node))
        return fail(ErrorCode::unsupported, "optional feature '" + feature + "' exists but is not a GenICam feature node");
    ErrorSlot error;
    const bool implemented = arv_gc_feature_node_is_implemented(ARV_GC_FEATURE_NODE(node), error.out());
    if (error.failed()) return translate("query implementation of optional feature", feature, *error.take());
    return implemented;
}

Result<std::int64_t> read_integer(ArvDevice* device, std::string_view name, bool writable) {
    const auto node = feature_node(device, name, writable);
    if (!node) return node.error();
    if (!ARV_IS_GC_INTEGER(node.value()))
        return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not an integer feature");
    ErrorSlot error;
    const gint64 value = arv_gc_integer_get_value(ARV_GC_INTEGER(node.value()), error.out());
    if (error.failed()) return translate("read", name, *error.take());
    return static_cast<std::int64_t>(value);
}

Result<profile::IntegerFeature> read_integer_feature(ArvDevice* device, std::string_view name) {
    const auto node = feature_node(device, name, true);
    if (!node) return node.error();
    if (!ARV_IS_GC_INTEGER(node.value()))
        return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not an integer feature");
    auto* integer = ARV_GC_INTEGER(node.value());
    profile::IntegerFeature f;
    ErrorSlot error;
    f.value = arv_gc_integer_get_value(integer, error.out());
    if (!error.failed()) f.minimum = arv_gc_integer_get_min(integer, error.out());
    if (!error.failed()) f.maximum = arv_gc_integer_get_max(integer, error.out());
    if (!error.failed()) f.increment = arv_gc_integer_get_inc(integer, error.out());
    if (error.failed()) return translate("read value/bounds/increment of", name, *error.take());
    return f;
}

Result<profile::FloatFeature> read_float_feature(ArvDevice* device, std::string_view name) {
    const auto node = feature_node(device, name, true);
    if (!node) return node.error();
    if (!ARV_IS_GC_FLOAT(node.value()))
        return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not a float feature");
    auto* real = ARV_GC_FLOAT(node.value());
    profile::FloatFeature f;
    ErrorSlot error;
    f.value = arv_gc_float_get_value(real, error.out());
    if (!error.failed()) f.minimum = arv_gc_float_get_min(real, error.out());
    if (!error.failed()) f.maximum = arv_gc_float_get_max(real, error.out());
    double increment = 0;
    if (!error.failed()) increment = arv_gc_float_get_inc(real, error.out());
    if (error.failed()) return translate("read value/bounds of", name, *error.take());
    // Aravis 0.8.36 (arv_gc_float_get_inc, arvgcfloat.c) documents exactly
    // G_MINDOUBLE (== DBL_MIN) as "not defined"; a failure is the GError
    // handled above. Only that value is absence. Every other successfully
    // returned value - including zero, negative and nonfinite ones from an
    // explicit <Inc> - is kept unchanged so check_limits() rejects it with
    // feature/value context instead of it silently becoming "no increment".
    if (increment != G_MINDOUBLE) f.increment = increment;
    if (const char* unit = arv_gc_float_get_unit(real); unit && *unit) f.unit = unit;
    return f;
}

Result<std::string> read_string(ArvDevice* device, std::string_view name) {
    const auto node = feature_node(device, name, false);
    if (!node) return node.error();
    ErrorSlot error;
    const char* value = nullptr;
    if (ARV_IS_GC_ENUMERATION(node.value()))
        value = arv_gc_enumeration_get_string_value(ARV_GC_ENUMERATION(node.value()), error.out());
    else if (ARV_IS_GC_STRING(node.value()))
        value = arv_gc_string_get_value(ARV_GC_STRING(node.value()), error.out());
    else
        return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not an enumeration or string");
    if (error.failed()) return translate("read", name, *error.take());
    return copy(value); // copied before any later SDK call can invalidate it
}

Result<std::optional<std::string>> read_optional_string(ArvDevice* device, std::string_view name) {
    const auto exists = present(device, name);
    if (!exists) return exists.error(); // a failed query is not absence
    if (!exists.value()) return std::optional<std::string>{};
    auto value = read_string(device, name); // wrong node type: explicit diagnostic
    if (!value) return value.error();
    return std::optional<std::string>{std::move(value).value()};
}

Result<std::optional<bool>> read_optional_boolean(ArvDevice* device, std::string_view name) {
    const auto exists = present(device, name);
    if (!exists) return exists.error();
    if (!exists.value()) return std::optional<bool>{};
    const auto node = feature_node(device, name, false);
    if (!node) return node.error();
    if (!ARV_IS_GC_BOOLEAN(node.value()))
        return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not a Boolean feature");
    ErrorSlot error;
    const bool value = arv_gc_boolean_get_value(ARV_GC_BOOLEAN(node.value()), error.out());
    if (error.failed()) return translate("read", name, *error.take());
    return std::optional<bool>{value};
}

// The first IMPLEMENTED name wins. A failed implementation query stops the
// selection with that error: it never falls through to the next name.
Result<std::string> exposure_feature_name(ArvDevice* device) {
    for (const auto name : profile::exposure_features) {
        const auto exists = present(device, name);
        if (!exists) return exists.error();
        if (exists.value()) return std::string(name);
    }
    return fail(ErrorCode::unsupported, "neither ExposureTime nor ExposureTimeAbs is implemented");
}

// Complete native snapshot; any failure is reported, never defaulted.
Result<profile::NativeSnapshot> read_snapshot(ArvDevice* device) {
    profile::NativeSnapshot n;
    const auto sw = read_integer(device, profile::sensor_width_feature, false);
    if (!sw) return sw.error();
    const auto sh = read_integer(device, profile::sensor_height_feature, false);
    if (!sh) return sh.error();
    n.sensor_width = sw.value();
    n.sensor_height = sh.value();
    struct { std::string_view name; profile::IntegerFeature* target; } integers[] = {
        {profile::width_feature, &n.width}, {profile::height_feature, &n.height},
        {profile::offset_x_feature, &n.offset_x}, {profile::offset_y_feature, &n.offset_y}};
    for (const auto& [name, target] : integers) {
        auto value = read_integer_feature(device, name);
        if (!value) return value.error();
        *target = value.value();
    }
    auto exposure_name = exposure_feature_name(device);
    if (!exposure_name) return exposure_name.error();
    n.exposure_feature = std::move(exposure_name).value();
    auto rate = read_float_feature(device, profile::frame_rate_feature);
    if (!rate) return rate.error();
    n.frame_rate_hz = std::move(rate).value();
    auto exposure = read_float_feature(device, n.exposure_feature);
    if (!exposure) return exposure.error();
    n.exposure_us = std::move(exposure).value();

    auto pixel = read_string(device, "PixelFormat");
    if (!pixel) return pixel.error();
    n.pixel_format = std::move(pixel).value();
    {
        ErrorSlot error;
        n.mono8_available = arv_device_is_enumeration_entry_available(device, "PixelFormat", "Mono8", error.out());
        if (error.failed()) return translate("query entry Mono8 of", "PixelFormat", *error.take());
    }
    struct { std::string_view name; std::optional<std::string>* target; } optional_strings[] = {
        {"TriggerSelector", &n.trigger_selector}, {"TriggerMode", &n.trigger_mode}, {"ExposureAuto", &n.exposure_auto}};
    for (const auto& [name, target] : optional_strings) {
        auto value = read_optional_string(device, name);
        if (!value) return value.error();
        *target = std::move(value).value();
    }
    auto enable = read_optional_boolean(device, "AcquisitionFrameRateEnable");
    if (!enable) return enable.error();
    n.frame_rate_enable = enable.value();
    return n;
}

// Exactly one native feature write through the GenICam node (no Aravis
// ArvCamera convenience setter, so no implicit trigger/auto/format changes).
Status write_feature(ArvDevice* device, const profile::NativeWrite& write) {
    const auto node = feature_node(device, write.feature, true);
    if (!node) return node.error();
    ErrorSlot error;
    if (write.is_float) {
        if (!ARV_IS_GC_FLOAT(node.value())) return fail(ErrorCode::unsupported, write.feature + " is not a float feature");
        arv_gc_float_set_value(ARV_GC_FLOAT(node.value()), write.real, error.out());
    } else {
        if (!ARV_IS_GC_INTEGER(node.value())) return fail(ErrorCode::unsupported, write.feature + " is not an integer feature");
        arv_gc_integer_set_value(ARV_GC_INTEGER(node.value()), write.integer, error.out());
    }
    if (error.failed()) return translate("write", write.feature, *error.take());
    return {};
}

std::string describe(const profile::NativeWrite& write) {
    return write.feature + "=" + (write.is_float ? std::to_string(write.real) : std::to_string(write.integer));
}

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

Status execute_command(ArvDevice* device, std::string_view name, const TestHooks* hooks) {
    auto node = command_node(device, name);
    if (!node) return node.error();
    if (hooks && hooks->before_command) hooks->before_command(name, device);
    ErrorSlot error;
    arv_gc_command_execute(node.value(), error.out());
    if (error.failed()) return translate("execute", name, *error.take());
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

using Clock = std::chrono::steady_clock;
// Longest single native timed pop; bounds cancellation latency.
constexpr std::chrono::milliseconds receive_polling_slice{5};
// Process-wide, so generations are distinct across wrappers and devices.
std::atomic<std::uint64_t> stream_generations{0};

struct Run {
    Run(std::uint64_t stream_gen, std::uint64_t config_gen, const CameraConfiguration& frozen, std::size_t buffers,
        std::size_t bytes)
        : stream_generation(stream_gen), configuration_generation(config_gen), configuration(frozen),
          buffer_count(buffers), buffer_bytes(bytes) {}
    const std::uint64_t stream_generation;
    const std::uint64_t configuration_generation;
    const CameraConfiguration configuration; // frozen from start()'s fresh readback
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
};

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

// One natively open device: ownership reservation plus native reference.
struct NativeOpen {
    std::unique_ptr<Reservation> reservation;
    GObjectPtr<ArvDevice> device;
    std::string device_id;
};

struct DeviceCore {
    DeviceCore(NativeOpen native, CameraCapabilities c, profile::NativeSnapshot e, std::shared_ptr<const TestHooks> h)
        : reservation(std::move(native.reservation)), device(std::move(native.device)), caps(c),
          envelope(std::move(e)), hooks(std::move(h)) {}
    std::mutex mutex; // D
    std::unique_ptr<Reservation> reservation;
    GObjectPtr<ArvDevice> device; // null after logical close
    const CameraCapabilities caps;
    const profile::NativeSnapshot envelope;
    const std::shared_ptr<const TestHooks> hooks;
    std::shared_ptr<Run> active; // the run owning native acquisition, if any
    std::shared_ptr<Run> latest; // most recent successful run (restart lease rule)
    std::optional<Error> stop_failure; // first failed AcquisitionStop not yet reported by close()
};

std::string text(const CameraConfiguration& c) {
    return std::to_string(c.width) + "x" + std::to_string(c.height) + "@(" + std::to_string(c.offset_x) + "," +
           std::to_string(c.offset_y) + ") " + std::to_string(c.frame_rate_hz) + " Hz " +
           std::to_string(c.exposure.count()) + " ns";
}

// Fresh, truthful configuration (device lock held).
Result<CameraConfiguration> read_configuration_locked(DeviceCore& core) {
    auto native = read_snapshot(core.device.get());
    if (!native) return native.error();
    if (auto ok = profile::check_preconditions(native.value()); !ok) return ok.error();
    return profile::configuration_from(native.value());
}

// Retires core.active (device lock held, device reference still present when
// `closing` is false or close() has not yet dropped it). Order:
//   1. cancel: receive returns within one polling slice;
//   2. AcquisitionStop through the SDK (failure recorded, never hidden);
//   3. mark retired, take the stream out, and wait (at most one slice) until
//      no receive is inside a native pop; lease releases now free storage;
//   4. drop the last native-stream reference: the SDK finalizer joins its own
//      receiver (Fake) or stops the receiver and writes the stream-channel
//      register (GigE) while this device's lock and reservation are held;
//      queued buffers are freed by that finalizer, leased ones by their lease.
// No second explicit thread stop is issued; arv_shutdown() is never called.
void retire_locked(DeviceCore& core, bool closing) noexcept {
    const std::shared_ptr<Run> run = std::move(core.active);
    if (!run) return;
    if (closing) run->closed.store(true); // before cancel: receive reports disconnected
    run->cancel.store(true);
    if (core.device) {
        try {
            if (auto stopped = execute_command(core.device.get(), "AcquisitionStop", core.hooks.get()); !stopped) {
                if (!core.stop_failure)
                    core.stop_failure = Error{stopped.error().code,
                                              stopped.error().message + " (stream generation " +
                                                  std::to_string(run->stream_generation) +
                                                  "); local stream resources were retired anyway; the camera may "
                                                  "still be acquiring"};
            }
        } catch (...) {
            // Allocation failure while describing the error: keep retiring.
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

// Lightweight wrapper. create_stream() performs no native work; start()
// creates a FRESH native stream and pool for every run. Several wrappers may
// exist for one device, but DeviceCore::active admits one acquisition at a
// time and the restart lease rule is per device, so no wrapper bypasses them.
// Statistics are per wrapper and accumulate across that wrapper's runs.
class AravisStream final : public IFrameStream {
public:
    explicit AravisStream(std::shared_ptr<DeviceCore> core) : core_(std::move(core)) {}
    ~AravisStream() override { stop(); } // caller has joined its receiving thread
    AravisStream(const AravisStream&) = delete;
    AravisStream& operator=(const AravisStream&) = delete;

    Status start(std::uint64_t configuration_generation) override;
    Result<FrameLease> receive(std::chrono::milliseconds timeout) override;

    void request_stop() noexcept override {
        if (const auto run = current()) run->cancel.store(true);
    }

    void stop() noexcept override {
        std::lock_guard lock(core_->mutex);
        const auto run = current();
        if (!run) return;
        run->cancel.store(true);
        if (core_->active == run) retire_locked(*core_, false);
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
    mutable std::mutex mutex_; // W: guards run_ only
    std::shared_ptr<Run> run_;
    std::atomic<std::uint64_t> delivered_{0}, incomplete_{0}, timeouts_{0}, starvation_{0};
};

// Start order (device lock held throughout, so apply()/close()/other starts
// are serialized and see either no run or a fully published one):
//   reject closed / active acquisition / outstanding leases of the previous
//   run -> fresh snapshot + profile preconditions + representable readback ->
//   AcquisitionMode Continuous, commands present and writable, PayloadSize ->
//   checked pool plan -> native stream (receiver starts) -> allocate & queue
//   the fixed pool -> AcquisitionStart -> publish. AcquisitionStart is the
//   last native step, so any failure after the native stream exists retires
//   it WITHOUT AcquisitionStop and frees every queued buffer before
//   returning; nothing is published.
Status AravisStream::start(std::uint64_t configuration_generation) {
    std::lock_guard device_lock(core_->mutex);
    DeviceCore& core = *core_;
    if (!core.device) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
    if (core.active)
        return fail(ErrorCode::busy, "an acquisition is already active on this Aravis device (stream generation " +
                                         std::to_string(core.active->stream_generation) +
                                         "); stop it before starting another; no native stream was created");
    if (core.latest) {
        std::lock_guard run_lock(core.latest->mutex);
        if (core.latest->outstanding != 0)
            return fail(ErrorCode::resource_exhausted,
                        std::to_string(core.latest->outstanding) + " source lease(s) from stream generation " +
                            std::to_string(core.latest->stream_generation) +
                            " are still held; release them before restarting; no native stream was created");
    }
    ArvDevice* device = core.device.get();
    auto snapshot = read_snapshot(device);
    if (!snapshot) return snapshot.error();
    if (auto ok = profile::check_preconditions(snapshot.value()); !ok) return ok.error();
    auto frozen = profile::configuration_from(snapshot.value());
    if (!frozen) return fail(frozen.error().code, "current configuration is not representable: " + frozen.error().message);
    auto mode = read_string(device, "AcquisitionMode");
    if (!mode) return mode.error();
    if (mode.value() != profile::acquisition_mode)
        return fail(ErrorCode::unsupported, "AcquisitionMode is '" + mode.value() +
                                                "'; the profile requires Continuous, which the adapter does not set");
    for (const char* command : {"AcquisitionStart", "AcquisitionStop"})
        if (auto node = command_node(device, command); !node) return node.error();
    auto payload = read_integer(device, "PayloadSize", false);
    if (!payload) return payload.error();
    auto pool = profile::plan_pool(payload.value(), frozen.value());
    if (!pool) return pool.error();

    auto run = std::make_shared<Run>(stream_generations.fetch_add(1) + 1, configuration_generation, frozen.value(),
                                     pool.value().buffers, pool.value().buffer_bytes);
    {
        ErrorSlot error;
        run->stream.reset(arv_device_create_stream(device, nullptr, nullptr, error.out()));
        if (error.failed()) return translate("create stream on", "", *error.take());
        if (!run->stream) return fail(ErrorCode::io_error, "Aravis returned no stream object; nothing was started");
    }
    if (core.hooks->after_stream_created) core.hooks->after_stream_created(run->stream.get());
    // Partial-setup cleanup. AcquisitionStart is the last native step, so a
    // failure never needs AcquisitionStop. Cancel, retire, and drop the sole
    // native-stream reference: the SDK finalizer stops its receiver and frees
    // every queued pool buffer. Nothing was published; configuration was not
    // written; start() can be retried explicitly.
    const auto abandon = [&](Error error) -> Status {
        run->cancel.store(true);
        GObjectPtr<ArvStream> stream;
        {
            std::lock_guard lock(run->mutex);
            run->retired = true;
            stream = std::move(run->stream);
        }
        stream.reset();
        return error;
    };
    try {
        for (std::size_t i = 0; i < run->buffer_count; ++i) {
            GObjectPtr<ArvBuffer> buffer(arv_buffer_new_allocate(run->buffer_bytes));
            if (!buffer) return abandon(fail(ErrorCode::resource_exhausted, "Aravis buffer allocation failed"));
            if (core.hooks->after_buffer_allocated) core.hooks->after_buffer_allocated(buffer.get());
            arv_stream_push_buffer(run->stream.get(), buffer.release()); // transfer full
        }
        if (auto ok = execute_command(device, "AcquisitionStart", core.hooks.get()); !ok)
            return abandon(fail(ok.error().code, ok.error().message + "; the native stream and " +
                                                     std::to_string(run->buffer_count) +
                                                     " queued buffers were released; configuration is unchanged; "
                                                     "retry start() explicitly"));
        {
            std::lock_guard lock(mutex_);
            run_ = run;
        }
        core.active = run;
        core.latest = run;
        return {};
    } catch (...) {
        (void)abandon(fail(ErrorCode::io_error, "start failed"));
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
Result<FrameLease> AravisStream::receive(std::chrono::milliseconds timeout) {
    if (timeout < std::chrono::milliseconds::zero())
        return fail(ErrorCode::invalid_argument, "receive timeout must not be negative");
    const auto run = current();
    if (!run) return fail(ErrorCode::stopped, "Aravis stream has not been started");
    const auto entry = Clock::now();
    // Saturating deadline: a huge timeout cannot overflow the time point.
    const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - entry);
    const auto deadline = timeout >= room ? Clock::time_point::max() : entry + timeout;
    const auto& hooks = core_->hooks;
    std::optional<Error> malformed; // last rejected complete buffer, reported on expiry
    for (;;) {
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
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(slice).count();
            popped = arv_stream_timeout_pop_buffer(stream, static_cast<guint64>(std::max<std::int64_t>(micros, 1)));
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
            const auto decision = profile::validate_frame(facts, run->configuration);
            if (decision.verdict == profile::FrameVerdict::accept) {
                const auto* data = static_cast<const std::byte*>(arv_buffer_get_image_data(buffer.get(), nullptr));
                FrameDescriptor d;
                d.width = run->configuration.width;
                d.height = run->configuration.height;
                d.row_stride_bytes = decision.row_stride;
                d.storage_bits_per_pixel = 8;
                d.significant_bits_per_pixel = 8;
                d.bands = 1;
                d.pixel_format = PixelFormat::mono8;
                d.byte_order = ByteOrder::not_applicable; // single-byte samples
                d.status = FrameStatus::complete;
                d.native_frame_id = facts.frame_id; // SDK value, unmodified
                d.stream_generation = run->stream_generation;
                d.configuration_generation = run->configuration_generation;
                d.configuration = run->configuration;
                d.timestamp = FrameTimestamp{}; // deliberately unqualified (GIGE-MEL-009)
                {
                    std::lock_guard lock(run->mutex);
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
                return FrameLease{d, std::span<const std::byte>(data, decision.span_bytes), std::move(owner)};
            }
            if (decision.verdict == profile::FrameVerdict::incomplete) incomplete_.fetch_add(1);
            else malformed = Error{decision.code, decision.reason};
            recycle(*run, buffer.release());
        }
        if (Clock::now() >= deadline) break;
    }
    timeouts_.fetch_add(1);
    if (malformed)
        return fail(ErrorCode::timeout, "no valid frame before the receive deadline; last rejected buffer: " +
                                            malformed->message);
    return fail(ErrorCode::timeout, "no valid frame before the receive deadline (stream generation " +
                                        std::to_string(run->stream_generation) + ")");
}

class AravisDevice final : public ICameraDevice {
public:
    AravisDevice(NativeOpen native, CameraCapabilities caps, profile::NativeSnapshot envelope,
                 std::shared_ptr<const TestHooks> hooks)
        : core_(std::make_shared<DeviceCore>(std::move(native), caps, std::move(envelope), std::move(hooks))) {}
    ~AravisDevice() override { (void)close(); }
    AravisDevice(const AravisDevice&) = delete;
    AravisDevice& operator=(const AravisDevice&) = delete;

    // Validated during open; immutable snapshot. No device I/O, also after close.
    CameraCapabilities capabilities() const override { return core_->caps; }

    Result<CameraConfiguration> configuration() const override {
        std::lock_guard lock(core_->mutex);
        if (!core_->device) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        return read_configuration_locked(*core_);
    }

    Result<CameraConfiguration> apply(const CameraConfiguration& request) override {
        std::lock_guard lock(core_->mutex);
        DeviceCore& core = *core_;
        if (!core.device) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        // Start, active acquisition and stop/retirement all hold this lock, so
        // a set `active` is the only acquisition state visible here.
        if (core.active)
            return fail(ErrorCode::invalid_state, "an acquisition is active on this Aravis device (stream generation " +
                                                      std::to_string(core.active->stream_generation) +
                                                      "); stop it before applying configuration; nothing was written");
        auto current = read_snapshot(core.device.get());
        if (!current) return current.error();
        // Requires the stopped/manual profile; nothing is normalized.
        auto plan = profile::plan_writes(request, current.value(), core.envelope);
        if (!plan) return plan.error();
        std::string written;
        for (const auto& write : plan.value()) {
            if (core.hooks && core.hooks->before_write) core.hooks->before_write(write.feature, core.device.get());
            if (auto ok = write_feature(core.device.get(), write); !ok) {
                // Preserve the original failure; report earlier writes and the
                // actual native state. No rollback is attempted or claimed.
                auto message = ok.error().message + "; earlier successful writes: " +
                               (written.empty() ? std::string("none") : written) + "; no rollback was attempted";
                const auto actual = read_configuration_locked(core);
                message += actual ? "; actual native configuration: " + text(actual.value())
                                  : "; actual native configuration unreadable or unrepresentable: " + actual.error().message;
                return fail(ok.error().code, std::move(message));
            }
            written += (written.empty() ? "" : ", ") + describe(write);
        }
        auto actual = read_configuration_locked(core);
        if (!actual)
            return fail(actual.error().code, actual.error().message + " (after writes: " + written + ")");
        return actual; // may differ from the request through native quantization
    }

    // Lightweight: no native stream, receiver, allocation or feature write.
    Result<std::unique_ptr<IFrameStream>> create_stream() override {
        std::lock_guard lock(core_->mutex);
        if (!core_->device) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        return std::unique_ptr<IFrameStream>(std::make_unique<AravisStream>(core_));
    }

    // Idempotent logical close. Order: retire the active run (cancel, SDK
    // AcquisitionStop, wait for in-flight native waits, synchronous native
    // stream finalization) while the device reference and reservation are
    // held; then drop the native device; then release the reservation, so a
    // reopen cannot overlap old native I/O. Outstanding leases stay valid.
    // Reports (once) an AcquisitionStop failure recorded by stop() or here.
    // Never calls arv_shutdown().
    Status close() override {
        GObjectPtr<ArvDevice> retired;
        std::unique_ptr<Reservation> released;
        std::optional<Error> stop_failure;
        {
            std::lock_guard lock(core_->mutex);
            retire_locked(*core_, true);
            retired = std::move(core_->device);
            released = std::move(core_->reservation);
            stop_failure = std::exchange(core_->stop_failure, std::nullopt);
        }
        retired.reset();
        released.reset();
        if (stop_failure) return *stop_failure;
        return {};
    }

    bool with_native(const std::function<void(ArvDevice*)>& fn) {
        std::lock_guard lock(core_->mutex);
        if (!core_->device) return false;
        fn(core_->device.get());
        return true;
    }

private:
    const std::shared_ptr<DeviceCore> core_;
};

class AravisBackend final : public ICameraBackend {
public:
    AravisBackend(InterfaceSource source, TestHooks hooks)
        : source_(std::move(source)), hooks_(std::make_shared<const TestHooks>(std::move(hooks))) {}
    ~AravisBackend() override = default; // drops only its own interface reference
    AravisBackend(const AravisBackend&) = delete;
    AravisBackend& operator=(const AravisBackend&) = delete;

    std::string_view id() const noexcept override { return backend_id; }

    // Aravis 0.8.36's update_device_list() has no error result; the detectable
    // SDK failures are a missing interface instance or a malformed entry. An
    // empty vector is a successful empty enumeration.
    Result<std::vector<DeviceInfo>> enumerate() override {
        std::lock_guard lock(interface_mutex(source_.name));
        auto iface = interface_locked();
        if (!iface) return iface.error();
        arv_interface_update_device_list(iface.value());
        std::vector<DeviceInfo> devices;
        const unsigned count = arv_interface_get_n_devices(iface.value());
        for (unsigned i = 0; i < count; ++i) {
            const char* id = arv_interface_get_device_id(iface.value(), i);
            if (!id || !*id)
                return fail(ErrorCode::io_error, "Aravis " + source_.name + " enumeration returned an entry without a device ID");
            // Copied now: Aravis frees these strings at the next refresh.
            std::string display = copy(arv_interface_get_device_vendor(iface.value(), i)) + " " +
                                  copy(arv_interface_get_device_model(iface.value(), i)) + " serial " +
                                  copy(arv_interface_get_device_serial_nbr(iface.value(), i)) + " (" +
                                  copy(arv_interface_get_device_protocol(iface.value(), i)) + " " +
                                  copy(arv_interface_get_device_address(iface.value(), i)) + ")";
            devices.push_back({std::string(backend_id), std::string(id), std::move(display)});
        }
        return devices;
    }

    Result<std::shared_ptr<ICameraDevice>> open(std::string_view device_id) override {
        auto native = open_native(device_id);
        if (!native) return native.error();
        auto snapshot = read_snapshot(native.value().device.get());
        if (!snapshot) return fail(snapshot.error().code, "open '" + std::string(device_id) + "': " + snapshot.error().message);
        auto caps = profile::capabilities_from(snapshot.value());
        if (!caps) return fail(caps.error().code, "open '" + std::string(device_id) + "': unsupported camera profile: " + caps.error().message);
        // A current configuration that is not representable (e.g. a fractional
        // rate) does not prevent open: configuration() reports it explicitly
        // and apply() can recover with an exactly representable request.
        std::shared_ptr<ICameraDevice> device =
            std::make_shared<AravisDevice>(std::move(native).value(), caps.value(), std::move(snapshot).value(), hooks_);
        return device;
    }

    // Exact-ID open with reservation; no profile validation (inspection uses it).
    Result<NativeOpen> open_native(std::string_view device_id) {
        if (device_id.empty()) return fail(ErrorCode::invalid_argument, "an explicit, nonempty Aravis device ID is required");
        const std::string id(device_id);
        // Same blocking acquisition as a lock_guard; the try_lock only lets a
        // private test observe genuine contention at this boundary.
        std::unique_lock lock(interface_mutex(source_.name), std::try_to_lock);
        if (!lock.owns_lock()) {
            if (hooks_->interface_lock_contended) hooks_->interface_lock_contended(id);
            lock.lock();
        }
        auto iface = interface_locked();
        if (!iface) return iface.error();
        // Exact selection: the ID must equal a discovered device ID. Aravis's
        // own open would also accept user IDs, serials, aliases or addresses.
        arv_interface_update_device_list(iface.value());
        unsigned matches = 0;
        const unsigned count = arv_interface_get_n_devices(iface.value());
        for (unsigned i = 0; i < count; ++i)
            if (const char* found = arv_interface_get_device_id(iface.value(), i); found && id == found) ++matches;
        if (matches != 1)
            return fail(ErrorCode::not_found, "Aravis " + source_.name + " device ID '" + id + "' was " +
                                                  (matches ? "ambiguous" : "not found") + " among " + std::to_string(count) +
                                                  " discovered device(s); no other device or alias was tried");
        NativeOpen native;
        native.device_id = id;
        auto reservation = Reservation::acquire(source_.name + "/" + id);
        if (!reservation) return reservation.error();
        native.reservation = std::move(reservation).value();
        if (hooks_->after_reservation) hooks_->after_reservation(id);
        ErrorSlot error;
        native.device.reset(arv_interface_open_device(iface.value(), id.c_str(), error.out()));
        if (error.failed()) return translate("open device", id, *error.take()); // native/reservation released
        if (!native.device) return fail(ErrorCode::not_found, "Aravis could not open device '" + id + "'");
        // Per-device client policy (not process-wide, not a camera feature):
        // the SDK revalidates bounds and access mode on every write.
        arv_device_set_range_check_policy(native.device.get(), ARV_RANGE_CHECK_POLICY_ENABLE);
        arv_device_set_access_check_policy(native.device.get(), ARV_ACCESS_CHECK_POLICY_ENABLE);
        if (hooks_->after_native_open) hooks_->after_native_open(native.device.get());
        return native;
    }

    const std::string& interface_name() const { return source_.name; }

private:
    // Lazily acquired, then retained for this backend's lifetime.
    Result<ArvInterface*> interface_locked() {
        if (!interface_) {
            ArvInterface* instance = source_.instance ? source_.instance() : nullptr;
            if (!instance) return fail(ErrorCode::io_error, "Aravis interface '" + source_.name + "' is unavailable");
            interface_.reset(ARV_INTERFACE(g_object_ref(instance)));
        }
        return interface_.get();
    }

    InterfaceSource source_;
    std::shared_ptr<const TestHooks> hooks_;
    GObjectPtr<ArvInterface> interface_; // guarded by interface_mutex(source_.name)
};

std::string integer_text(const profile::IntegerFeature& f) {
    return std::to_string(f.value) + " [" + std::to_string(f.minimum) + ", " + std::to_string(f.maximum) + "] inc " +
           std::to_string(f.increment);
}

std::string float_text(const profile::FloatFeature& f) {
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, "%.17g [%.17g, %.17g]%s%s", f.value, f.minimum, f.maximum,
                  f.unit ? " unit " : "", f.unit ? f.unit->c_str() : "");
    return buffer;
}

} // namespace

std::shared_ptr<ICameraBackend> make_backend_for_interface(InterfaceSource source, TestHooks hooks) {
    return std::make_shared<AravisBackend>(std::move(source), std::move(hooks));
}

std::shared_ptr<ICameraBackend> make_backend() {
    // GigE Vision only. Lazily resolved; no discovery during construction.
    return make_backend_for_interface({"GigEVision", &arv_gv_interface_get_instance});
}

std::string runtime_version() {
    return std::to_string(arv_get_major_version()) + "." + std::to_string(arv_get_minor_version()) + "." +
           std::to_string(arv_get_micro_version());
}

std::string compiled_version() { return ARAVIS_VERSION; }

bool with_native_device_for_testing(ICameraDevice& device, const std::function<void(ArvDevice*)>& fn) {
    auto* concrete = dynamic_cast<AravisDevice*>(&device);
    return concrete && concrete->with_native(fn);
}

Result<Inspection> inspect(ICameraBackend& backend, std::string_view device_id) {
    auto* concrete = dynamic_cast<AravisBackend*>(&backend);
    if (!concrete) return fail(ErrorCode::invalid_argument, "inspection requires a backend created by the Aravis adapter");
    auto native = concrete->open_native(device_id);
    if (!native) return native.error();
    Inspection result;
    result.device_id = native.value().device_id;
    result.interface_protocol = concrete->interface_name();
    ArvDevice* device = native.value().device.get();
    // Identity strings (read-only features; absence is reported, not fatal).
    for (const char* name : {"DeviceVendorName", "DeviceModelName", "DeviceSerialNumber", "DeviceID", "DeviceVersion"}) {
        auto value = read_optional_string(device, name);
        if (!value) {
            // A failed query is reported, not shown as absence.
            result.native.emplace_back(name, "<error: " + value.error().message + ">");
            result.problems.push_back(value.error().message);
        } else if (value.value()) {
            result.native.emplace_back(name, *value.value());
        }
    }
    auto snapshot = read_snapshot(device);
    if (!snapshot) {
        result.problems.push_back(snapshot.error().message);
        return result; // the native reference and reservation are released here
    }
    const auto& n = snapshot.value();
    result.native.emplace_back("SensorWidth", std::to_string(n.sensor_width));
    result.native.emplace_back("SensorHeight", std::to_string(n.sensor_height));
    result.native.emplace_back("Width", integer_text(n.width));
    result.native.emplace_back("Height", integer_text(n.height));
    result.native.emplace_back("OffsetX", integer_text(n.offset_x));
    result.native.emplace_back("OffsetY", integer_text(n.offset_y));
    result.native.emplace_back("AcquisitionFrameRate", float_text(n.frame_rate_hz));
    result.native.emplace_back(n.exposure_feature, float_text(n.exposure_us));
    result.native.emplace_back("PixelFormat", n.pixel_format + (n.mono8_available ? " (Mono8 available)" : " (Mono8 unavailable)"));
    result.native.emplace_back("TriggerSelector", n.trigger_selector.value_or("<absent>"));
    result.native.emplace_back("TriggerMode", n.trigger_mode.value_or("<absent>"));
    result.native.emplace_back("ExposureAuto", n.exposure_auto.value_or("<absent>"));
    result.native.emplace_back("AcquisitionFrameRateEnable",
                               n.frame_rate_enable ? (*n.frame_rate_enable ? "true" : "false") : "<absent>");
    if (auto caps = profile::capabilities_from(n)) result.capabilities = caps.value();
    else result.problems.push_back("unsupported profile: " + caps.error().message);
    if (auto actual = profile::configuration_from(n)) result.configuration = actual.value();
    else result.problems.push_back("current configuration: " + actual.error().message);
    return result;
}

} // namespace gige_mel::backend::aravis
