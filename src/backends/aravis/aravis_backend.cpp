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

#include <cfloat>
#include <cstdio>
#include <map>
#include <mutex>
#include <new>
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

// Optional features: absent or unimplemented is std::nullopt, not an error.
bool present(ArvDevice* device, std::string_view name) {
    const std::string feature(name);
    ArvGcNode* node = arv_device_get_feature(device, feature.c_str());
    if (!node || !ARV_IS_GC_FEATURE_NODE(node)) return false;
    ErrorSlot error;
    const bool implemented = arv_gc_feature_node_is_implemented(ARV_GC_FEATURE_NODE(node), error.out());
    return implemented && !error.failed();
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
    // Aravis 0.8.36 reports G_MINDOUBLE when no increment is advertised.
    if (increment > DBL_MIN) f.increment = increment;
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
    if (!present(device, name)) return std::optional<std::string>{};
    auto value = read_string(device, name);
    if (!value) return value.error();
    return std::optional<std::string>{std::move(value).value()};
}

Result<std::string> exposure_feature_name(ArvDevice* device) {
    for (const auto name : profile::exposure_features)
        if (present(device, name)) return std::string(name);
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
    if (present(device, "AcquisitionFrameRateEnable")) {
        ErrorSlot error;
        const bool enabled = arv_device_get_boolean_feature_value(device, "AcquisitionFrameRateEnable", error.out());
        if (error.failed()) return translate("read", "AcquisitionFrameRateEnable", *error.take());
        n.frame_rate_enable = enabled;
    }
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
// One natively open device: ownership reservation plus native reference.
struct NativeOpen {
    std::unique_ptr<Reservation> reservation;
    GObjectPtr<ArvDevice> device;
    std::string device_id;
};

class AravisDevice final : public ICameraDevice {
public:
    AravisDevice(NativeOpen native, CameraCapabilities caps, profile::NativeSnapshot envelope,
                 std::shared_ptr<const TestHooks> hooks)
        : reservation_(std::move(native.reservation)), device_(std::move(native.device)), caps_(caps),
          envelope_(std::move(envelope)), hooks_(std::move(hooks)) {}
    ~AravisDevice() override { (void)close(); }
    AravisDevice(const AravisDevice&) = delete;
    AravisDevice& operator=(const AravisDevice&) = delete;

    // Validated during open; immutable snapshot. No device I/O, also after close.
    CameraCapabilities capabilities() const override { return caps_; }

    Result<CameraConfiguration> configuration() const override {
        std::lock_guard lock(mutex_);
        if (!device_) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        return read_configuration_locked();
    }

    Result<CameraConfiguration> apply(const CameraConfiguration& request) override {
        std::lock_guard lock(mutex_);
        if (!device_) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        auto current = read_snapshot(device_.get());
        if (!current) return current.error();
        // Requires the stopped/manual profile; nothing is normalized.
        auto plan = profile::plan_writes(request, current.value(), envelope_);
        if (!plan) return plan.error();
        std::string written;
        for (const auto& write : plan.value()) {
            if (hooks_ && hooks_->before_write) hooks_->before_write(write.feature, device_.get());
            if (auto ok = write_feature(device_.get(), write); !ok) {
                // Preserve the original failure; report earlier writes and the
                // actual native state. No rollback is attempted or claimed.
                auto message = ok.error().message + "; earlier successful writes: " +
                               (written.empty() ? std::string("none") : written) + "; no rollback was attempted";
                const auto actual = read_configuration_locked();
                message += actual ? "; actual native configuration: " + text(actual.value())
                                  : "; actual native configuration unreadable or unrepresentable: " + actual.error().message;
                return fail(ok.error().code, std::move(message));
            }
            written += (written.empty() ? "" : ", ") + describe(write);
        }
        auto actual = read_configuration_locked();
        if (!actual)
            return fail(actual.error().code, actual.error().message + " (after writes: " + written + ")");
        return actual; // may differ from the request through native quantization
    }

    Result<std::unique_ptr<IFrameStream>> create_stream() override {
        std::lock_guard lock(mutex_);
        if (!device_) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        // No native stream is created, no worker started, no frame fabricated.
        return fail(ErrorCode::unsupported,
                    "Aravis image acquisition is not implemented (camera-control foundation only, GIGE-MEL-008); "
                    "no stream was created");
    }

    // Idempotent. Order: mark closed and drop the native reference, then
    // release the ownership reservation, so a new open cannot overlap the old
    // native device. Never calls arv_shutdown().
    Status close() override {
        GObjectPtr<ArvDevice> retired;
        std::unique_ptr<Reservation> released;
        {
            std::lock_guard lock(mutex_);
            retired = std::move(device_);
            released = std::move(reservation_);
        }
        retired.reset();
        released.reset();
        return {};
    }

    bool with_native(const std::function<void(ArvDevice*)>& fn) {
        std::lock_guard lock(mutex_);
        if (!device_) return false;
        fn(device_.get());
        return true;
    }

private:
    static std::string text(const CameraConfiguration& c) {
        return std::to_string(c.width) + "x" + std::to_string(c.height) + "@(" + std::to_string(c.offset_x) + "," +
               std::to_string(c.offset_y) + ") " + std::to_string(c.frame_rate_hz) + " Hz " +
               std::to_string(c.exposure.count()) + " ns";
    }
    Result<CameraConfiguration> read_configuration_locked() const {
        auto native = read_snapshot(device_.get());
        if (!native) return native.error();
        if (auto ok = profile::check_preconditions(native.value()); !ok) return ok.error();
        return profile::configuration_from(native.value());
    }

    mutable std::mutex mutex_; // serializes this device's native feature I/O
    std::unique_ptr<Reservation> reservation_;
    GObjectPtr<ArvDevice> device_;
    const CameraCapabilities caps_;
    const profile::NativeSnapshot envelope_;
    std::shared_ptr<const TestHooks> hooks_;
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
        std::lock_guard lock(interface_mutex(source_.name));
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
        if (!present(device, name)) continue;
        auto value = read_string(device, name);
        result.native.emplace_back(name, value ? value.value() : "<error: " + value.error().message + ">");
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
