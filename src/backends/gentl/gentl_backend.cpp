// GIGE-MEL-GENTL-002 internal GenTL camera backend over the GENTL-001
// consumer and the Aravis 0.8.36 GenApi session. Read-only foundation:
// apply() and create_stream() are explicitly unsupported.
//
// Ownership per open device (outermost first; destroyed in reverse):
//   Producer <- System <- Interface <- Device <- GenApiSession(ArvDevice -> ArvGc)
// The backend retains the Producer between explicit operations. Native close
// order is GenApi release (no I/O), DevClose, IFClose, TLClose. A failed close
// is reported once and leaves that object (and its parents) open; the
// consumer then taints the producer and keeps it mapped for the process
// lifetime (the GENTL-001 quarantine), so nothing is unmapped under live
// native state and no double close occurs.
#include "gentl_backend.hpp"
#include "gentl_genapi.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <thread>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace gige_mel::backend::gentl_backend {
namespace {

Error fail(ErrorCode code, std::string message) { return Error{code, std::move(message)}; }
Error from(const gentl::Failure& failure) { return Error{failure.code, failure.message()}; }
constexpr std::string_view id_prefix = "gentl1:";

} // namespace

std::string compose_device_id(std::string_view interface_id, std::string_view device_id) {
    std::string result(id_prefix);
    result += std::to_string(interface_id.size());
    result += ':';
    result += interface_id;
    result += device_id;
    return result;
}

Result<DeviceSelection> parse_device_id(std::string_view composite) {
    const auto invalid = [&](std::string why) {
        return fail(ErrorCode::invalid_argument, "GenTL device ID '" + std::string(composite) + "' is not an exact "
                    "composite selection ID (gentl1:<interface-id-length>:<interface-id><device-id>): " + std::move(why));
    };
    if (composite.rfind(id_prefix, 0) != 0) return invalid("missing gentl1: prefix");
    auto rest = composite.substr(id_prefix.size());
    const auto colon = rest.find(':');
    if (colon == std::string_view::npos || colon == 0) return invalid("missing interface ID length");
    const auto digits = rest.substr(0, colon);
    if (digits.size() > 1 && digits[0] == '0') return invalid("interface ID length has a leading zero");
    std::size_t length = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), length);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
        return invalid("interface ID length is not a decimal number");
    rest.remove_prefix(colon + 1);
    if (length == 0 || length >= rest.size()) return invalid("interface or device ID is empty or truncated");
    return DeviceSelection{std::string(rest.substr(0, length)), std::string(rest.substr(length))};
}

namespace {

struct NativeChain;
void quarantine(NativeChain&&) noexcept;

// Opened native chain for one device. Children own their parents, so the
// producer cannot be unloaded while any member exists.
struct NativeChain {
    std::shared_ptr<gentl::System> system;
    std::shared_ptr<gentl::Interface> interface;
    std::shared_ptr<gentl::Device> device;
    std::unique_ptr<GenApiSession> genapi;

    // Orderly teardown, each native close attempted at most once. Returns the
    // first failure with every later diagnostic appended. On failure the
    // unresolved object and its parents are NOT closed again by their
    // destructors' success paths: the consumer taints the producer instead.
    Status close() noexcept {
        std::optional<Error> first;
        std::string later;
        const auto note = [&](const gentl::Failure& failure) {
            if (!first) first = from(failure);
            else later += "; then " + failure.message();
        };
        try {
            std::shared_ptr<gentl::Device> released = genapi ? genapi->release() : device;
            genapi.reset();
            if (released && released != device) device = std::move(released);
            bool parents = true;
            if (device) {
                if (auto s = device->close(); !s) { note(s.error()); parents = false; }
            }
            if (parents && interface) {
                if (auto s = interface->close(); !s) { note(s.error()); parents = false; }
            }
            if (parents && system) {
                if (auto s = system->close(); !s) note(s.error());
            }
            if (!first) {
                device.reset();
                interface.reset();
                system.reset();
                return {};
            }
            // Never re-close (a destructor would retry DevClose/IFClose/TLClose)
            // and never unmap: retain the unresolved chain, and through it the
            // producer, for the process lifetime.
            quarantine(std::move(*this));
            return fail(first->code, first->message + later +
                                         "; unresolved native ownership is retained for the process lifetime "
                                         "(producer not closed or unmapped)");
        } catch (...) {
            return fail(ErrorCode::io_error, "GenTL teardown failed with an unexpected exception");
        }
    }
};

// Deliberately leaked: destructors of unresolved native objects never run.
std::mutex quarantine_mutex;
std::vector<NativeChain>* quarantined = nullptr;

void quarantine(NativeChain&& chain) noexcept {
    std::lock_guard lock(quarantine_mutex);
    try {
        if (!quarantined) quarantined = new std::vector<NativeChain>();
        quarantined->push_back(std::move(chain));
    } catch (...) {
        // Allocation failed: leak the members individually rather than close them.
        new (std::nothrow) std::shared_ptr<gentl::Device>(std::move(chain.device));
        new (std::nothrow) std::shared_ptr<gentl::Interface>(std::move(chain.interface));
        new (std::nothrow) std::shared_ptr<gentl::System>(std::move(chain.system));
    }
}

} // namespace

std::size_t quarantined_native_sessions() noexcept {
    std::lock_guard lock(quarantine_mutex);
    return quarantined ? quarantined->size() : 0;
}

namespace {

// One open GenTL device. All operations are serialized by mutex_; a complete
// profile transaction (snapshot..readback) holds it plus the GenApi session
// lock. No callback into provider code runs under either lock.
class GenTLDevice final : public ICameraDevice {
public:
    GenTLDevice(NativeChain chain, std::shared_ptr<const profiles::ICameraProfile> profile,
                std::shared_ptr<const profiles::ICameraProfileState> state, CameraCapabilities caps, std::string id)
        : chain_(std::move(chain)), profile_(std::move(profile)), state_(std::move(state)), caps_(caps),
          id_(std::move(id)) {
        const auto& xml = chain_.genapi->xml();
        inspection_ = Inspection{xml.url, xml.length, xml.sha1_verified, xml.schema, xml.skipped,
                                 state_->native_diagnostics()};
    }
    ~GenTLDevice() override { (void)close(); }

    CameraCapabilities capabilities() const override { return caps_; }

    // Fresh, read-only readback through the profile (no writes, no cache).
    Result<CameraConfiguration> configuration() const override {
        if (reentered()) return reentry();
        Guard lock(*this);
        if (closed_) return fail(ErrorCode::stopped, "GenTL device '" + id_ + "' is closed");
        std::optional<Result<CameraConfiguration>> result;
        auto status = chain_.genapi->transaction([&](device_features::IDeviceFeatures& features) -> Status {
            result.emplace(state_->configuration(features, [this]() -> Status { return connected(); }));
            return {};
        });
        if (!status) return status.error();
        return std::move(*result);
    }

    Result<CameraConfiguration> apply(const CameraConfiguration&) override {
        return fail(ErrorCode::unsupported, "GenTL backend foundation (GIGE-MEL-GENTL-002) is read-only: "
                                            "configuration writes are not enabled; nothing was written");
    }

    Result<std::unique_ptr<IFrameStream>> create_stream() override {
        return fail(ErrorCode::unsupported, "GenTL backend foundation (GIGE-MEL-GENTL-002) has no acquisition: "
                                            "streaming is out of scope; no stream or AcquisitionStart was attempted");
    }

    Status close() override {
        if (reentered()) return reentry(); // never tear down under an active transaction
        Guard lock(*this);
        if (closed_) return close_result_;
        closed_ = true;
        close_result_ = chain_.close();
        return close_result_;
    }

    const Inspection& inspection() const noexcept { return inspection_; }

#ifdef GIGE_MEL_GENTL_TESTING
    Status features(const std::function<Status(device_features::IDeviceFeatures&)>& callback) {
        if (reentered()) return reentry();
        Guard lock(*this);
        if (closed_) return fail(ErrorCode::stopped, "GenTL device '" + id_ + "' is closed");
        return chain_.genapi->transaction(callback);
    }
#endif

private:
    // Runs under mutex_ during a transaction: a native-only check, no callback.
    Status connected() const {
        if (closed_) return fail(ErrorCode::stopped, "GenTL device '" + id_ + "' is closed");
        return {};
    }

    // Same-thread reentry (a profile/test callback calling back into this
    // device) is refused instead of self-deadlocking on mutex_.
    struct Guard {
        const GenTLDevice& device;
        std::lock_guard<std::mutex> lock;
        explicit Guard(const GenTLDevice& d) : device(d), lock(d.mutex_) { d.owner_.store(std::this_thread::get_id()); }
        ~Guard() { device.owner_.store(std::thread::id{}); }
    };
    bool reentered() const noexcept { return owner_.load() == std::this_thread::get_id(); }
    static Error reentry() {
        return fail(ErrorCode::invalid_state, "reentrant GenTL device call from inside an active operation refused");
    }

    mutable std::mutex mutex_;
    mutable std::atomic<std::thread::id> owner_{};
    mutable NativeChain chain_;
    // Profile state can pin a dynamically loaded module: declared after the
    // native chain, so it is destroyed FIRST (no native object references it).
    const std::shared_ptr<const profiles::ICameraProfile> profile_;
    const std::shared_ptr<const profiles::ICameraProfileState> state_;
    const CameraCapabilities caps_;
    const std::string id_;
    Inspection inspection_;
    bool closed_{false};
    Status close_result_;
};

class GenTLBackend final : public ICameraBackend {
public:
    GenTLBackend(Options options, std::shared_ptr<const profiles::ICameraProfile> profile)
        : options_(std::move(options)), profile_(std::move(profile)) {}

    std::string_view id() const noexcept override { return backend_id; }

    // Explicit discovery: every interface's IFUpdateDeviceList. Device IDs are
    // exact composite selection IDs; nothing is opened.
    Result<std::vector<DeviceInfo>> enumerate() override {
        std::lock_guard lock(mutex_);
        auto system = open_system_locked();
        if (!system) return system.error();
        auto interfaces = system.value()->interfaces(false);
        if (!interfaces) return close_system(system.value(), from(interfaces.error()));
        std::map<std::string, DeviceInfo> found; // stable order by composite ID
        for (const auto& description : interfaces.value()) {
            auto iface = system.value()->open_interface(description.id);
            if (!iface) return close_system(system.value(), from(iface.error()));
            Status listed = list_devices(*iface.value(), found);
            auto closed = iface.value()->close();
            if (!listed) return close_system(system.value(), listed.error());
            if (!closed) return close_system(system.value(), from(closed.error()));
        }
        if (auto s = system.value()->close(); !s) return from(s.error());
        std::vector<DeviceInfo> devices;
        for (auto& [id, info] : found) devices.push_back(std::move(info));
        return devices;
    }

    Result<std::shared_ptr<ICameraDevice>> open(std::string_view device_id) override;

private:
    // Producer loaded on first explicit use, then retained by this backend.
    Result<std::shared_ptr<gentl::System>> open_system_locked() {
        if (!producer_) {
            auto loaded = gentl::Producer::load(options_.producer_path);
            if (!loaded) return from(loaded.error());
            producer_ = loaded.value();
        }
        auto system = producer_->open_system();
        if (!system) return from(system.error());
        if (auto s = system.value()->update_interfaces(options_.discovery_timeout); !s)
            return close_system(system.value(), from(s.error()));
        return system.value();
    }

    static Error close_system(const std::shared_ptr<gentl::System>& system, Error error) {
        if (auto s = system->close(); !s) error.message += "; TLClose: " + s.error().message();
        return error;
    }

    Status list_devices(gentl::Interface& iface, std::map<std::string, DeviceInfo>& found) {
        if (auto s = iface.update_devices(options_.discovery_timeout); !s) return from(s.error());
        auto devices = iface.devices();
        if (!devices) return from(devices.error());
        for (const auto& device : devices.value()) {
            std::string display = device.id;
            for (const auto* name : {"DEVICE_INFO_VENDOR", "DEVICE_INFO_MODEL", "DEVICE_INFO_SERIAL_NUMBER"})
                if (const auto* p = gentl::find(device.properties, name); p && p->text && !p->text.value().empty())
                    display += " " + p->text.value();
            auto composite = compose_device_id(iface.id(), device.id);
            found.emplace(composite, DeviceInfo{std::string(backend_id), composite, display + " @ " + iface.id()});
        }
        return {};
    }

    const Options options_;
    const std::shared_ptr<const profiles::ICameraProfile> profile_;
    std::mutex mutex_;
    std::map<std::string, std::weak_ptr<GenTLDevice>> open_;
    std::shared_ptr<gentl::Producer> producer_; // explicit; children keep it alive
};

Result<std::shared_ptr<ICameraDevice>> GenTLBackend::open(std::string_view device_id) {
    auto selection = parse_device_id(device_id);
    if (!selection) return selection.error();
    const std::string id(device_id);
    std::lock_guard lock(mutex_);
    if (auto held = open_.find(id); held != open_.end() && !held->second.expired())
        return fail(ErrorCode::busy, "GenTL device '" + id + "' is already open through this backend");
    NativeChain chain;
    auto system = open_system_locked();
    if (!system) return system.error();
    chain.system = system.value();
    const auto failed = [&](Error error) -> Error { // partial-open cleanup, at most once
        if (auto s = chain.close(); !s) error.message += "; cleanup: " + s.error().message;
        return error;
    };
    auto iface = chain.system->open_interface(selection.value().interface_id);
    if (!iface) return failed(from(iface.error()));
    chain.interface = iface.value();
    // Exact: refresh this interface's list, then require an exact listed ID.
    if (auto s = chain.interface->update_devices(options_.discovery_timeout); !s) return failed(from(s.error()));
    // Access is exactly the configured mode; there is no escalation path.
    const auto access = options_.access == DeviceAccess::control ? gentl::Access::control : gentl::Access::read_only;
    auto device = chain.interface->open_device(selection.value().device_id, access);
    if (!device) return failed(from(device.error()));
    chain.device = device.value();
    auto genapi = GenApiSession::open(chain.device, access, XmlLimits{options_.max_xml_bytes});
    if (!genapi) return failed(fail(genapi.error().code, "GenTL device '" + id + "': " + genapi.error().message));
    chain.genapi = std::move(genapi).value();
    // The injected profile alone decides compatibility; no fallback profile.
    std::optional<Result<std::shared_ptr<const profiles::ICameraProfileState>>> admitted;
    auto status = chain.genapi->transaction([&](device_features::IDeviceFeatures& features) -> Status {
        admitted.emplace(profile_->open(features));
        return {};
    });
    if (!status) return failed(status.error());
    if (!*admitted)
        return failed(fail(admitted->error().code, "open '" + id + "' with profile '" + std::string(profile_->id()) +
                                                       "': " + admitted->error().message));
    auto state = std::move(*admitted).value();
    if (!state) return failed(fail(ErrorCode::invalid_state, "profile open returned null state"));
    const auto caps = state->capabilities();
    try {
        auto opened = std::make_shared<GenTLDevice>(std::move(chain), profile_, std::move(state), caps, id);
        open_[id] = opened;
        return std::shared_ptr<ICameraDevice>(std::move(opened));
    } catch (const std::bad_alloc&) {
        return failed(fail(ErrorCode::resource_exhausted, "GenTL device allocation failed"));
    }
}

} // namespace

std::shared_ptr<ICameraBackend> make_backend(Options options, std::shared_ptr<const profiles::ICameraProfile> profile) {
    if (!profile) throw std::invalid_argument("GenTL backend requires an explicit camera profile");
    return std::make_shared<GenTLBackend>(std::move(options), std::move(profile));
}

std::optional<Inspection> inspect(const ICameraDevice& device) {
    if (const auto* gentl = dynamic_cast<const GenTLDevice*>(&device)) return gentl->inspection();
    return std::nullopt;
}

#ifdef GIGE_MEL_GENTL_TESTING
Status with_features_for_testing(ICameraDevice& device,
                                 const std::function<Status(device_features::IDeviceFeatures&)>& callback) {
    auto* gentl = dynamic_cast<GenTLDevice*>(&device);
    if (!gentl) return fail(ErrorCode::invalid_argument, "not a GenTL backend device");
    return gentl->features(callback);
}
#endif

} // namespace gige_mel::backend::gentl_backend

