// Aravis SDK mechanics for the optional camera-control foundation. Profile
// policy and feature orchestration live in profiles/mono_free_running*.cpp.
//
// Synchronization boundary: the adapter serializes ITS OWN discovery/open per
// Aravis interface (process-wide, across adapter instances) and its own
// feature I/O per device. It cannot make unrelated application calls into
// Aravis thread-safe. Ownership: one logically open device per (interface,
// device ID) per process, across adapter instances; a second open is `busy`.
#include "aravis_private.hpp"
#include "aravis_features.hpp"
#include "aravis_stream.hpp"
#include "device_registers.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace gige_mel::backend::aravis {
namespace detail {

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

// Independent of cancellation, closure and uncertain camera-command outcomes.
// The closure owns a shared latch, NOT DeviceCore or any native object. GLib
// retains the closure during an invocation; disconnect can invalidate it while
// the heartbeat callback is running without destroying its user data early.
struct ControlLoss {
    std::atomic<bool> lost{false};
    std::atomic<bool> detached{false}; // closure no longer selected/invoking
};
static_assert(std::atomic<bool>::is_always_lock_free);

namespace {

void control_lost(ArvDevice*, gpointer data) noexcept {
    (*static_cast<std::shared_ptr<ControlLoss>*>(data))->lost.store(true);
}

void release_control_loss(gpointer data, GClosure*) noexcept {
    (*static_cast<std::shared_ptr<ControlLoss>*>(data))->detached.store(true);
    delete static_cast<std::shared_ptr<ControlLoss>*>(data);
}

void drain_loss_emission(ArvDevice* device, const ControlLoss& state) noexcept {
    if (device && ARV_IS_GV_DEVICE(device)) {
        while (!state.detached.load()) std::this_thread::yield();
        while (g_atomic_int_get(&G_OBJECT(device)->ref_count) > 1)
            std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
}

} // namespace

Error control_loss_error() {
    return fail(ErrorCode::disconnected,
                "Aravis reported camera control lost; this logical device is terminal; close and reopen explicitly; "
                "physical Ethernet connectivity and camera acquisition state were not independently measured");
}

// Subscribe immediately after SDK open, before profile reads/validation.
// Failed opens have a heartbeat too and need the same safe release order.
struct NativeOpen {
    NativeOpen() = default;
    NativeOpen(NativeOpen&&) = default;
    NativeOpen& operator=(NativeOpen&&) = delete;
    ~NativeOpen() {
        if (device && loss_handler) {
            g_signal_handler_disconnect(device.get(), loss_handler);
            drain_loss_emission(device.get(), *control_loss);
        }
    }
    void connect_loss() {
        loss_handler = g_signal_connect_data(device.get(), "control-lost", G_CALLBACK(control_lost), loss_data.get(),
                                             release_control_loss, G_CONNECT_DEFAULT);
        (void)loss_data.release(); // GClosure owns user data through its last invocation
    }
    std::unique_ptr<Reservation> reservation;
    GObjectPtr<ArvDevice> device;
    std::string device_id;
    std::shared_ptr<ControlLoss> control_loss{std::make_shared<ControlLoss>()};
    // Constructed BEFORE native open creates the heartbeat thread. In addition
    // to GLib's registration synchronization this supplies an instrumented
    // thread-creation publication edge for the immutable callback user data.
    std::unique_ptr<std::shared_ptr<ControlLoss>> loss_data{
        std::make_unique<std::shared_ptr<ControlLoss>>(control_loss)};
    gulong loss_handler{0};
};

DeviceCore::DeviceCore(NativeOpen native, CameraCapabilities c, std::shared_ptr<const profiles::ICameraProfileState> e,
                       std::shared_ptr<const profiles::ICameraProfile> p, std::shared_ptr<const TestHooks> h)
    : reservation(std::move(native.reservation)), device(std::move(native.device)), caps(c),
      profile_owner(std::move(p)), policy(std::move(e)), hooks(std::move(h)), control_loss(std::move(native.control_loss)),
      loss_handler(std::exchange(native.loss_handler, 0)) {}

DeviceCore::~DeviceCore() {
    if (device) {
        disconnect_loss();
        drain_loss_emission(device.get(), *control_loss);
    }
}

void DeviceCore::disconnect_loss() noexcept {
    if (device && loss_handler) g_signal_handler_disconnect(device.get(), std::exchange(loss_handler, 0));
}

bool DeviceCore::lost() const noexcept { return control_loss->lost.load(); }

} // namespace detail

namespace {

namespace profile = profiles;

using detail::AravisDeviceFeatures;
using detail::copy;
using detail::ErrorSlot;
using detail::fail;
using detail::GObjectPtr;
using detail::read_optional_identity;
using detail::translate;

// Process-wide registries shared by every adapter instance.
std::mutex& interface_mutex(const std::string& interface_name) {
    static std::mutex registry;
    static std::map<std::string, std::mutex> mutexes; // node-stable
    std::lock_guard lock(registry);
    return mutexes[interface_name];
}

using detail::Acquisition;
using detail::active_generation_locked;
using detail::control_loss_error;
using detail::DeviceCore;
using detail::drain_loss_emission;
using detail::NativeOpen;
using detail::Reservation;
using detail::retire_locked;
using detail::stop_command_locked;
using detail::uncertain_refusal;

// Fresh, truthful configuration (device lock held).
Result<CameraConfiguration> read_configuration_locked(DeviceCore& core) {
    AravisDeviceFeatures features(core.device.get());
    return core.policy->configuration(features, [&core]() -> Status {
        if (core.lost()) return control_loss_error();
        return {};
    });
}

class AravisRegisters final : public device_registers::CheckedRegisters {
public:
    explicit AravisRegisters(DeviceCore& core) : core_(core) {}
private:
    device_registers::Status connected() const override {
        if (core_.lost()) return control_loss_error();
        if (!core_.device) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        return {};
    }
    device_registers::Status outcome(gboolean success, ErrorSlot& error, std::string_view operation) {
        if (error.failed()) {
            auto native = error.take();
            auto diagnostic = translate(operation, {}, *native);
            if (native->domain == ARV_DEVICE_ERROR &&
                native->code == ARV_DEVICE_ERROR_PROTOCOL_ERROR_NOT_IMPLEMENTED)
                diagnostic.code = ErrorCode::unsupported;
            return diagnostic;
        }
        if (!success) return fail(ErrorCode::io_error, "Aravis " + std::string(operation) + " failed without GError");
        return {};
    }
    device_registers::Result<std::uint32_t> native_read_register(std::uint64_t address) override {
        ErrorSlot error;
        guint32 value = 0;
        const auto success = arv_device_read_register(core_.device.get(), address, &value, error.out());
        if (auto s = outcome(success, error, "read_register"); !s) return s.error();
        return value;
    }
    device_registers::Status native_write_register(std::uint64_t address, std::uint32_t value) override {
        ErrorSlot error;
        const auto success = arv_device_write_register(core_.device.get(), address, value, error.out());
        return outcome(success, error, "write_register");
    }
    device_registers::Status native_read_memory(std::uint64_t address, std::span<std::byte> bytes) override {
        ErrorSlot error;
        const auto success = arv_device_read_memory(core_.device.get(), address,
            static_cast<guint32>(bytes.size()), bytes.data(), error.out());
        return outcome(success, error, "read_memory");
    }
    device_registers::Status native_write_memory(std::uint64_t address, std::span<const std::byte> bytes) override {
        ErrorSlot error;
        const auto success = arv_device_write_memory(core_.device.get(), address,
            static_cast<guint32>(bytes.size()), bytes.data(), error.out());
        return outcome(success, error, "write_memory");
    }
    DeviceCore& core_;
};

class AravisDevice final : public ICameraDevice, public device_registers::IRegisterDevice {
public:
    AravisDevice(NativeOpen native, CameraCapabilities caps, std::shared_ptr<const profiles::ICameraProfileState> policy,
                 std::shared_ptr<const profiles::ICameraProfile> owner, std::shared_ptr<const TestHooks> hooks)
        : core_(std::make_shared<DeviceCore>(std::move(native), caps, std::move(policy), std::move(owner), std::move(hooks))) {}
    ~AravisDevice() override { (void)close(); }
    AravisDevice(const AravisDevice&) = delete;
    AravisDevice& operator=(const AravisDevice&) = delete;

    // Validated during open; immutable snapshot. No device I/O, also after close.
    CameraCapabilities capabilities() const override { return core_->caps; }

    device_registers::Status with_registers(
        const std::function<device_registers::Status(device_registers::IDeviceRegisters&)>& operation) override {
        if (!operation) return fail(ErrorCode::invalid_argument, "empty register transaction");
        std::lock_guard lock(core_->mutex);
        if (core_->lost()) return control_loss_error();
        if (!core_->device) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        AravisRegisters registers(*core_);
        return operation(registers);
    }

    Result<CameraConfiguration> configuration() const override {
        std::lock_guard lock(core_->mutex);
        if (core_->lost()) return control_loss_error();
        if (!core_->device) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        return read_configuration_locked(*core_);
    }

    Result<CameraConfiguration> apply(const CameraConfiguration& request) override {
        std::lock_guard lock(core_->mutex);
        DeviceCore& core = *core_;
        if (core.lost()) return control_loss_error();
        if (!core.device) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        // Start, active acquisition and stop/retirement all hold this lock, so
        // a set `active` is the only acquisition state visible here.
        if (core.active)
            return fail(ErrorCode::invalid_state, "an acquisition is active on this Aravis device (stream generation " +
                                                      std::to_string(active_generation_locked(core)) +
                                                      "); stop it before applying configuration; nothing was written");
        // No active receiver is NOT proof that reuse is safe: an unresolved
        // AcquisitionStart/Stop outcome refuses configuration before writes.
        if (core.acquisition == Acquisition::uncertain) return uncertain_refusal(core, "nothing was written");
        AravisDeviceFeatures features(core.device.get());
        const profile::ConnectionCheck check_connection = [&core]() -> Status {
            if (core.lost()) return control_loss_error();
            return {};
        };
        const profile::BeforeWrite before_write = [&core](std::string_view feature) -> Status {
            if (core.lost()) return control_loss_error();
            if (core.hooks && core.hooks->before_write) core.hooks->before_write(feature, core.device.get());
            if (core.lost()) return control_loss_error();
            return {};
        };
        return core.policy->apply(features, request, check_connection, before_write);
    }

    // Lightweight: no native stream, receiver, allocation or feature write.
    Result<std::unique_ptr<IFrameStream>> create_stream() override {
        std::lock_guard lock(core_->mutex);
        if (core_->lost()) return control_loss_error();
        if (!core_->device) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        return detail::make_stream(core_);
    }

    // Idempotent logical close; it has authority over the device's outstanding
    // acquisition state whichever wrapper owns it. Order, while the device
    // reference and reservation are held: retire an active run (cancel, SDK
    // AcquisitionStop, wait for in-flight native waits, synchronous native
    // stream finalization); or, with NO native stream but an `uncertain`
    // outcome, still attempt one AcquisitionStop. Then drop the native device;
    // then release the reservation, so a reopen cannot overlap old native I/O.
    // The device closes whatever the command outcome. Outstanding leases stay
    // valid. Returns (once) an unreported stop() failure - marked recovered if
    // a later stop succeeded - and/or this close's own stop failure, stating
    // that the camera outcome remains uncertain. A repeated close performs no
    // device I/O. Close is not proof the camera stopped. Never arv_shutdown().
    Status close() override {
        GObjectPtr<ArvDevice> retired;
        std::unique_ptr<Reservation> released;
        std::optional<DeviceCore::Fault> history;
        std::optional<Error> now;
        bool unresolved = false;
        bool had_device = false;
        {
            std::lock_guard lock(core_->mutex);
            DeviceCore& core = *core_;
            if (core.device) {
                had_device = true;
                if (core.active) {
                    retire_locked(core, true, &now);
                } else if (!core.lost() && core.acquisition != Acquisition::none) {
                    try {
                        if (auto stopped = stop_command_locked(core, "AcquisitionStop during close()"); !stopped)
                            now = stopped.error();
                    } catch (...) {
                        core.acquisition = Acquisition::uncertain;
                    }
                }
                unresolved = core.acquisition != Acquisition::none;
                core.disconnect_loss(); // no DeviceCore pointer is exposed to the SDK
            }
            retired = std::move(core.device);
            released = std::move(core.reservation);
            history = std::exchange(core.fault, std::nullopt);
            core.acquisition = Acquisition::none; // logically closed: no further device I/O
            core.owner = 0;
        }
        // Disconnect prevents future selection, but does not wait for a handler
        // already selected by GSignal. Its closure notifier proves that such
        // an invocation has finished (including selection-before-instance-ref).
        // GSignal also references its instance until after handler release.
        // Keep OUR reference until that emission reference is gone: otherwise
        // the heartbeat could run the finalizer and attempt to join itself.
        // Streams are already gone; no native handles are exported. ref_count
        // is a public atomic GObject field, not an invented camera-status API.
        // These are teardown-only waits, never a background health poll.
        drain_loss_emission(retired.get(), *core_->control_loss);
        retired.reset();
        released.reset();
        // Finalization joined the heartbeat: even an invocation already running
        // at disconnect has completed its latch store before this observation.
        if (had_device && core_->lost()) {
            auto error = control_loss_error();
            if (history) error.message += "; prior command failure: " + history->error.message;
            return error;
        }
        if (!history && !now && !unresolved) return {};
        const ErrorCode code = history ? history->error.code : now ? now->code : ErrorCode::io_error;
        try {
            std::string message;
            if (history) {
                message = history->error.message;
                if (history->recovered_by) message += "; recovered: " + std::string(history->recovered_by) + " succeeded";
            }
            if (now) message += (message.empty() ? "" : "; ") + std::string("close(): ") + now->message;
            if (unresolved)
                message += (message.empty() ? "" : "; ") +
                           std::string("the camera acquisition outcome remains uncertain (the camera may still be "
                                       "acquiring); the device was closed and its native resources released");
            return fail(code, std::move(message));
        } catch (...) {
            // Out of memory while describing: return what exists (no allocation).
            if (history) return std::move(history->error);
            if (now) return std::move(*now);
            return Error{code, {}};
        }
    }

    bool with_native(const std::function<void(ArvDevice*)>& fn) {
        std::lock_guard lock(core_->mutex);
        if (!core_->device) return false;
        fn(core_->device.get());
        return true;
    }

    // GIGE-MEL-010: read-only timing context (device lock held; no write).
    Result<TimingContext> timing_context() {
        std::lock_guard lock(core_->mutex);
        if (!core_->device) return fail(ErrorCode::disconnected, "Aravis device is logically closed");
        ArvDevice* device = core_->device.get();
        TimingContext context;
        context.native_device_type = copy(G_OBJECT_TYPE_NAME(device));
        context.native_interface = ARV_IS_GV_DEVICE(device) ? NativeInterface::gige_vision
                                   : ARV_IS_FAKE_DEVICE(device) ? NativeInterface::fake
                                                                : NativeInterface::other;
        for (const char* name : {"DeviceVendorName", "DeviceModelName", "DeviceSerialNumber", "DeviceVersion",
                                 "DeviceFirmwareVersion", "DeviceManufacturerInfo", "DeviceID"}) {
            auto value = read_optional_identity(device, name);
            if (!value) return fail(value.error().code, "timing context: " + value.error().message);
            if (value.value()) context.identity.emplace_back(name, *value.value());
        }
        // GenICam XML identity: size and SHA-256 of the description the SDK
        // actually loaded (device-owned memory; read, never modified).
        std::size_t xml_size = 0;
        const char* xml = arv_device_get_genicam_xml(device, &xml_size);
        if (xml && xml_size > 0) {
            std::unique_ptr<gchar, decltype(&g_free)> digest(
                g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(xml), xml_size), &g_free);
            context.identity.emplace_back("GenICamXmlBytes", std::to_string(xml_size));
            if (digest) context.identity.emplace_back("GenICamXmlSha256", digest.get());
        }
        // Tick frequency only through the supported GigE Vision accessor. The
        // Fake device is not an ArvGvDevice, so none is reported for it.
        if (context.native_interface == NativeInterface::gige_vision) {
            ErrorSlot error;
            const guint64 frequency = arv_gv_device_get_timestamp_tick_frequency(ARV_GV_DEVICE(device), error.out());
            if (error.failed()) return translate("read timestamp tick frequency of", "", *error.take());
            context.tick_frequency_hz = static_cast<std::uint64_t>(frequency);
        }
        return context;
    }

private:
    const std::shared_ptr<DeviceCore> core_;
};

class AravisBackend final : public ICameraBackend {
public:
    AravisBackend(InterfaceSource source, TestHooks hooks, std::shared_ptr<const profiles::ICameraProfile> profile)
        : profile_(std::move(profile)), source_(std::move(source)), hooks_(std::make_shared<const TestHooks>(std::move(hooks))) {}
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
        if (native.value().control_loss->lost.load()) return control_loss_error();
        AravisDeviceFeatures features(native.value().device.get());
        auto policy = profile_->open(features);
        if (native.value().control_loss->lost.load()) return control_loss_error();
        if (!policy) return fail(policy.error().code, "open '" + std::string(device_id) + "': " + policy.error().message);
        if (!policy.value()) return fail(ErrorCode::invalid_state, "profile open returned null state");
        auto caps = policy.value()->capabilities();
        // A current configuration that is not representable (e.g. a fractional
        // rate) does not prevent open: configuration() reports it explicitly
        // and apply() can recover with an exactly representable request.
        std::shared_ptr<ICameraDevice> device =
            std::make_shared<AravisDevice>(std::move(native).value(), caps, std::move(policy).value(), profile_, hooks_);
        return device;
    }

    Result<std::shared_ptr<const profiles::ICameraProfileState>> open_profile(device_features::IDeviceFeatures& features) const {
        return profile_->open(features);
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
        native.connect_loss(); // cover profile validation and failed-open cleanup too
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

    const std::shared_ptr<const profiles::ICameraProfile> profile_;
    InterfaceSource source_;
    std::shared_ptr<const TestHooks> hooks_;
    GObjectPtr<ArvInterface> interface_; // guarded by interface_mutex(source_.name)
};

} // namespace

std::shared_ptr<ICameraBackend> make_backend_for_interface(InterfaceSource source, TestHooks hooks) {
    return make_backend_for_interface(std::move(source), std::move(hooks), profiles::make_mono_free_running_profile());
}

std::shared_ptr<ICameraBackend> make_backend_for_interface(InterfaceSource source, TestHooks hooks,
    std::shared_ptr<const profiles::ICameraProfile> profile) {
    if (!profile) throw std::invalid_argument("Aravis requires a camera profile");
    return std::make_shared<AravisBackend>(std::move(source), std::move(hooks), std::move(profile));
}
std::shared_ptr<ICameraBackend> make_backend(std::shared_ptr<const profiles::ICameraProfile> profile) {
    return make_backend_for_interface({"GigEVision", &arv_gv_interface_get_instance}, {}, std::move(profile));
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

Result<TimingContext> read_timing_context(ICameraDevice& device) {
    auto* concrete = dynamic_cast<AravisDevice*>(&device);
    if (!concrete) return fail(ErrorCode::invalid_argument, "timing context requires a device opened by the Aravis adapter");
    return concrete->timing_context();
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
    for (const char* name : {"DeviceVendorName", "DeviceModelName", "DeviceSerialNumber", "DeviceID", "DeviceVersion",
                             "DeviceFirmwareVersion", "DeviceManufacturerInfo"}) {
        auto value = read_optional_identity(device, name);
        if (!value) {
            // A failed query is reported, not shown as absence.
            result.native.emplace_back(name, "<error: " + value.error().message + ">");
            result.problems.push_back(value.error().message);
        } else if (value.value()) {
            result.native.emplace_back(name, *value.value());
        }
    }
    AravisDeviceFeatures features(device);
    auto policy = concrete->open_profile(features);
    if (!policy) result.problems.push_back("unsupported profile: " + policy.error().message);
    else if (!policy.value()) result.problems.push_back("profile open returned null state");
    else {
        const auto values = policy.value()->native_diagnostics();
        result.native.insert(result.native.end(), values.begin(), values.end());
        result.capabilities = policy.value()->capabilities();
        auto actual = policy.value()->configuration(features, [] { return Status{}; });
        if (actual) result.configuration = actual.value();
        else result.problems.push_back("current configuration: " + actual.error().message);
    }
    return result;
}

} // namespace gige_mel::backend::aravis
