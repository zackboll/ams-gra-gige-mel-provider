// GIGE-MEL-GENTL-002 internal GenTL camera backend over the GENTL-001
// consumer and the Aravis 0.8.36 GenApi session. Read-only foundation:
// apply() and create_stream() are explicitly unsupported.
//
// Ownership per open device (outermost first; destroyed in reverse):
//   Producer <- System <- Interface <- Device <- GenApiSession(ArvDevice -> ArvGc)
// The backend retains the Producer between explicit operations. Native close
// order is GenApi release (no I/O), DevClose, IFClose, TLClose.
//
// Every native chain, for enumerate() as well as open(), is owned by exactly
// one NativeChain (GIGE-MEL-GENTL-002-R1). Its bookkeeping (the retirement
// node and the device reservation) is allocated BEFORE the first native call.
// Each native close is attempted at most once. A conclusive teardown releases
// everything. A failed or uncertain close RETIRES the chain without
// allocating: the unresolved object, its unclosed parents and the producer
// are kept for the process lifetime, and no destructor can retry a close.
// The device identity is then recorded as unresolved, and the producer as
// blocked, in the backend's ownership registry. That registry outlives the
// public device object and the backend.
#include "gentl_backend.hpp"
#include "gentl_genapi.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <exception>
#include <functional>
#include <thread>
#include <map>
#include <mutex>
#include <new>
#include <optional>
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

// ---- Deterministic allocation-failure seam (test builds only) --------------
// Each allocation that precedes the irreversible ownership transfer passes a
// named checkpoint. An armed checkpoint throws std::bad_alloc exactly once.
#ifdef GIGE_MEL_GENTL_TESTING
std::atomic<AllocationPoint> armed_allocation{AllocationPoint::none};
void checkpoint(AllocationPoint point) {
    auto expected = point;
    if (armed_allocation.compare_exchange_strong(expected, AllocationPoint::none)) throw std::bad_alloc();
}
template <class T> struct CheckpointAllocator {
    using value_type = T;
    AllocationPoint point;
    explicit CheckpointAllocator(AllocationPoint p) noexcept : point(p) {}
    template <class U> CheckpointAllocator(const CheckpointAllocator<U>& other) noexcept : point(other.point) {}
    T* allocate(std::size_t n) { checkpoint(point); return std::allocator<T>{}.allocate(n); }
    void deallocate(T* p, std::size_t n) noexcept { std::allocator<T>{}.deallocate(p, n); }
    template <class U> bool operator==(const CheckpointAllocator<U>& other) const noexcept { return point == other.point; }
};
#define GIGE_MEL_GENTL_ALLOCATION(point) checkpoint(AllocationPoint::point)
template <class T> CheckpointAllocator<T> device_allocator() { return CheckpointAllocator<T>{AllocationPoint::device_object}; }
#else
#define GIGE_MEL_GENTL_ALLOCATION(point) ((void)0)
template <class T> std::allocator<T> device_allocator() { return {}; }
#endif

// ---- Ownership registry -----------------------------------------------------
// Owned by the backend and shared with every chain it creates, including
// retired chains. It therefore outlives the public device object and the
// backend. Per device identity:
//   no entry   - never opened, or the last session closed conclusively;
//   open       - a live session (or an open in progress) holds the identity;
//   unresolved - a native close failed or was uncertain: a permanent tombstone.
// An expired device object is never treated as proof of release. An unresolved
// chain always leaves its interface/system open on the producer, so the
// producer is then blocked for every further operation (no verified recovery
// mechanism exists).
struct Ownership {
    enum class State { open, unresolved };
    std::mutex mutex; // leaf lock: never held across a native or provider call
    std::map<std::string, State> devices;
    bool producer_unresolved{false};
};

// Native members of one chain. Children own their parents, so the producer
// cannot be unloaded while any member exists. Destroyed in reverse order.
struct Members {
    std::shared_ptr<Ownership> ownership;      // retired nodes only: pins the tombstones
    std::shared_ptr<gentl::Producer> producer; // explicit: retained even if a child handle was lost
    std::shared_ptr<gentl::System> system;
    std::shared_ptr<gentl::Interface> interface;
    std::shared_ptr<gentl::Device> device;
    std::unique_ptr<GenApiSession> genapi;
    Members* next{nullptr}; // retirement-list link
};

// Deliberately leaked intrusive list. Destructors of unresolved native objects
// never run, so no close is retried and nothing is unmapped. Linking a node
// allocates nothing.
std::mutex quarantine_mutex;
Members* quarantined_head = nullptr;
std::size_t quarantined_count = 0;

// The single cleanup owner of one native chain (one enumeration or one device).
// prepare() allocates all bookkeeping, including the retirement node, and
// reserves the device identity BEFORE the first native call. Teardown and
// retirement are then noexcept and allocate only diagnostic text, with a
// preallocated fallback. Each native close is attempted at most once.
class NativeChain {
public:
    static Result<std::unique_ptr<NativeChain>> prepare(const std::shared_ptr<Ownership>& ownership,
                                                       std::string identity) {
        GIGE_MEL_GENTL_ALLOCATION(native_chain);
        std::unique_ptr<NativeChain> chain(new NativeChain());
        GIGE_MEL_GENTL_ALLOCATION(retirement_node);
        chain->spare_ = std::make_unique<Members>();
        chain->fallback_ = "GenTL native teardown failed and its diagnostics could not be allocated; unresolved "
                           "native ownership is retained for the process lifetime (producer not closed or unmapped)";
        chain->ownership_ = ownership;
        std::lock_guard lock(ownership->mutex);
        if (!identity.empty()) {
            if (const auto held = ownership->devices.find(identity); held != ownership->devices.end())
                return fail(ErrorCode::busy,
                            held->second == Ownership::State::open
                                ? "GenTL device '" + identity + "' is already open through this backend"
                                : "GenTL device '" + identity + "' has unresolved native ownership from an earlier "
                                  "session; reopening is refused (no IFOpenDevice)");
        }
        if (ownership->producer_unresolved)
            return fail(ErrorCode::busy, "GenTL producer has unresolved native ownership from an earlier failed or "
                                         "uncertain close; it is retained for the process lifetime and refuses "
                                         "further operations (no native call made)");
        if (identity.empty()) return chain; // enumeration reserves no device identity
        GIGE_MEL_GENTL_ALLOCATION(registry_entry);
        chain->reservation_ = ownership->devices.try_emplace(std::move(identity), Ownership::State::open).first;
        chain->reserved_ = true;
        return chain;
    }

    // Safety net for an owner that never reached an explicit teardown: this is
    // then the FIRST and only attempt (never a retry).
    ~NativeChain() { if (!done_) (void)teardown(true); }
    NativeChain(const NativeChain&) = delete;
    NativeChain& operator=(const NativeChain&) = delete;

    Members native;

    // Orderly teardown: GenApi release (no I/O), DevClose, IFClose, TLClose.
    // Success releases the members, the producer reference and the device
    // reservation. Any failure or uncertainty retires the chain (retire()).
    // Returns the first failure with every later diagnostic appended.
    std::optional<Error> close() noexcept { return teardown(true); }
    // Enumeration: closes the current interface and keeps the system.
    std::optional<Error> close_interface() noexcept { return teardown(false); }
    [[nodiscard]] bool done() const noexcept { return done_; }
    // A consumer open failed and its own cleanup of a just-opened handle
    // failed (the producer is tainted): that handle's parents must stay open.
    void mark_child_unresolved() noexcept { child_unresolved_ = true; }

private:
    NativeChain() = default;

    struct Diagnostics {
        std::optional<Error> first;
        std::string later;
        const char* uncertain{nullptr}; // static text only: recorded without allocating
        void note(const gentl::Failure& failure) {
            if (!first) first = from(failure);
            else later += "; then " + failure.message();
        }
    };

    // One native close, attempted exactly once. An exception after the call
    // (e.g. while recording diagnostics) leaves the outcome unresolved.
    template <class Object> static bool attempt(Object& object, Diagnostics& d) noexcept {
        if (!object) return true;
        try {
            auto status = object->close();
            if (status) return true;
            d.note(status.error());
        } catch (...) {
            d.uncertain = "a native close outcome could not be recorded";
        }
        return false;
    }

    std::optional<Error> teardown(bool all) noexcept {
        if (done_) return std::nullopt;
        Diagnostics d;
        bool resolved = true;
        if (child_unresolved_) {
            resolved = false;
            d.uncertain = "parent handles were not closed because the producer reported a failed cleanup of a "
                          "just-opened child handle";
        }
        if (resolved && native.genapi) {
            if (auto released = native.genapi->release()) {
                native.genapi.reset(); // ArvGc, then ArvDevice; no native I/O
                if (released != native.device) native.device = std::move(released);
            } else {
                resolved = false; // refused inside a transaction: keep everything alive
                d.uncertain = "the GenApi session could not be released";
            }
        }
        if (resolved) resolved = attempt(native.device, d);
        if (resolved) resolved = attempt(native.interface, d);
        if (resolved && all) resolved = attempt(native.system, d);
        if (resolved) {
            native.interface.reset();
            if (!all) return std::nullopt;
            native.device.reset();
            native.system.reset();
            native.producer.reset(); // the last owner unloads it, only after conclusive teardown
            release_reservation();
            done_ = true;
            return std::nullopt;
        }
        done_ = true;
        retire();
        try {
            Error error = d.first ? std::move(*d.first)
                                  : Error{ErrorCode::io_error, std::string("GenTL ") + d.uncertain};
            if (d.first && d.uncertain) error.message += std::string("; ") + d.uncertain;
            error.message += d.later;
            error.message += "; unresolved native ownership is retained for the process lifetime (producer not "
                             "closed or unmapped; the device identity and the producer are blocked)";
            return error;
        } catch (...) {
            return Error{ErrorCode::io_error, std::move(fallback_)};
        }
    }

    void release_reservation() noexcept {
        if (!reserved_) return;
        std::lock_guard lock(ownership_->mutex);
        ownership_->devices.erase(reservation_);
        reserved_ = false;
    }

    // Moves every remaining member into the preallocated node and links it into
    // the leaked list. This allocates nothing, runs no destructor and makes no
    // second close attempt. The identity becomes a permanent tombstone and the
    // producer is blocked.
    void retire() noexcept {
        if (ownership_) { // always set once native members exist (prepare())
            std::lock_guard lock(ownership_->mutex);
            ownership_->producer_unresolved = true;
            if (reserved_) reservation_->second = Ownership::State::unresolved;
        }
        reserved_ = false; // the tombstone now belongs to the registry, permanently
        Members* node = spare_.release(); // never null: prepare() is the only constructor path
        node->ownership = ownership_;
        node->producer = std::move(native.producer);
        node->system = std::move(native.system);
        node->interface = std::move(native.interface);
        node->device = std::move(native.device);
        node->genapi = std::move(native.genapi);
        std::lock_guard lock(quarantine_mutex);
        node->next = quarantined_head;
        quarantined_head = node;
        ++quarantined_count;
    }

    std::shared_ptr<Ownership> ownership_;
    std::map<std::string, Ownership::State>::iterator reservation_{};
    bool reserved_{false}, done_{false}, child_unresolved_{false};
    std::unique_ptr<Members> spare_;
    std::string fallback_;
};

} // namespace

std::size_t quarantined_native_sessions() noexcept {
    std::lock_guard lock(quarantine_mutex);
    return quarantined_count;
}

#ifdef GIGE_MEL_GENTL_TESTING
AllocationPoint fail_allocation_for_testing(AllocationPoint point) noexcept { return armed_allocation.exchange(point); }
#endif

namespace {

// Never throws: even its diagnostic text may be unallocatable.
Error exhausted(const char* what) noexcept {
    try {
        return fail(ErrorCode::resource_exhausted, what);
    } catch (...) {
        return Error{ErrorCode::resource_exhausted, {}};
    }
}

// One open GenTL device. All operations are serialized by mutex_; a complete
// profile transaction (snapshot..readback) holds it plus the GenApi session
// lock. No callback into provider code runs under either lock.
class GenTLDevice final : public ICameraDevice {
public:
    // Fully constructed (every allocation done) BEFORE it adopts the chain.
    GenTLDevice(std::shared_ptr<const profiles::ICameraProfile> profile,
                std::shared_ptr<const profiles::ICameraProfileState> state, CameraCapabilities caps, std::string id,
                Inspection inspection)
        : profile_(std::move(profile)), state_(std::move(state)), caps_(caps), id_(std::move(id)),
          inspection_(std::move(inspection)) {}
    ~GenTLDevice() override { (void)close(); }

    // The irreversible ownership transfer: noexcept and allocation-free, and
    // nothing that can fail runs after it.
    void adopt(std::unique_ptr<NativeChain> chain) noexcept { chain_ = std::move(chain); }

    CameraCapabilities capabilities() const override { return caps_; }

    // Fresh, read-only readback through the profile (no writes, no cache).
    Result<CameraConfiguration> configuration() const override {
        if (reentered()) return reentry();
        Guard lock(*this);
        if (closed_) return fail(ErrorCode::stopped, "GenTL device '" + id_ + "' is closed");
        std::optional<Result<CameraConfiguration>> result;
        auto status = chain_->native.genapi->transaction([&](device_features::IDeviceFeatures& features) -> Status {
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

    // Idempotent: the first result is returned again. A conclusive close
    // releases the identity; a failed one leaves a permanent tombstone.
    Status close() override {
        if (reentered()) return reentry(); // never tear down under an active transaction
        Guard lock(*this);
        if (closed_) return close_result_;
        closed_ = true;
        if (!chain_) return close_result_; // unreachable: adopt() immediately follows construction
        if (auto error = chain_->close()) close_result_ = std::move(*error);
        return close_result_;
    }

    const Inspection& inspection() const noexcept { return inspection_; }

#ifdef GIGE_MEL_GENTL_TESTING
    Status features(const std::function<Status(device_features::IDeviceFeatures&)>& callback) {
        if (reentered()) return reentry();
        Guard lock(*this);
        if (closed_) return fail(ErrorCode::stopped, "GenTL device '" + id_ + "' is closed");
        return chain_->native.genapi->transaction(callback);
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
    std::unique_ptr<NativeChain> chain_; // sole native owner once adopted
    // Profile state can pin a dynamically loaded module. close() (always run
    // by the destructor) finishes every native teardown before any member is
    // destroyed, and no native object references the profile state.
    const std::shared_ptr<const profiles::ICameraProfile> profile_;
    const std::shared_ptr<const profiles::ICameraProfileState> state_;
    const CameraCapabilities caps_;
    const std::string id_;
    const Inspection inspection_;
    bool closed_{false};
    Status close_result_;
};

class GenTLBackend final : public ICameraBackend {
public:
    GenTLBackend(Options options, std::shared_ptr<const profiles::ICameraProfile> profile)
        : options_(std::move(options)), profile_(std::move(profile)), ownership_(std::make_shared<Ownership>()) {}

    std::string_view id() const noexcept override { return backend_id; }

    // Explicit discovery: every interface's IFUpdateDeviceList. Device IDs are
    // exact composite selection IDs; nothing is opened.
    Result<std::vector<DeviceInfo>> enumerate() override {
        std::lock_guard lock(mutex_);
        std::unique_ptr<NativeChain> chain; // sole cleanup owner of this enumeration's natives
        std::optional<Error> error;
        try {
            auto listed = enumerate_locked(chain);
            if (listed) return listed;
            error = listed.error();
        } catch (const std::bad_alloc&) {
            error = exhausted("GenTL enumeration: allocation failed");
        }
        return cleanup(chain, std::move(*error));
    }

    Result<std::shared_ptr<ICameraDevice>> open(std::string_view device_id) override {
        std::lock_guard lock(mutex_);
        std::unique_ptr<NativeChain> chain; // sole cleanup owner until a device adopts it
        std::optional<Error> error;
        try {
            auto opened = open_locked(device_id, chain);
            if (opened) return opened;
            error = opened.error();
        } catch (const std::bad_alloc&) {
            error = exhausted("GenTL open: allocation failed");
        }
        return cleanup(chain, std::move(*error));
    }

private:
    Result<std::vector<DeviceInfo>> enumerate_locked(std::unique_ptr<NativeChain>& chain) {
        auto prepared = prepare_locked({});
        if (!prepared) return prepared.error();
        chain = std::move(prepared).value();
        auto& native = chain->native;
        if (auto s = open_system_locked(*chain); !s) return s.error();
        auto interfaces = native.system->interfaces(false);
        if (!interfaces) return consumer(*chain, interfaces.error());
        std::map<std::string, DeviceInfo> found; // stable order by composite ID
        for (const auto& description : interfaces.value()) {
            auto iface = native.system->open_interface(description.id);
            if (!iface) return consumer(*chain, iface.error());
            native.interface = std::move(iface).value();
            if (auto s = list_devices(*chain, found); !s) return s.error();
            if (auto e = chain->close_interface()) return std::move(*e); // retired: nothing left to close
        }
        std::vector<DeviceInfo> devices;
        devices.reserve(found.size()); // allocated before the final close
        if (auto e = chain->close()) return std::move(*e);
        for (auto& [id, info] : found) devices.push_back(std::move(info));
        return devices;
    }

    Result<std::shared_ptr<ICameraDevice>> open_locked(std::string_view device_id,
                                                       std::unique_ptr<NativeChain>& chain) {
        auto selection = parse_device_id(device_id);
        if (!selection) return selection.error();
        const std::string id(device_id);
        // The registry reservation and the retirement node exist before any
        // native call; a blocked identity or producer makes no native call.
        auto prepared = prepare_locked(id);
        if (!prepared) return prepared.error();
        chain = std::move(prepared).value();
        auto& native = chain->native;
        if (auto s = open_system_locked(*chain); !s) return s.error();
        auto iface = native.system->open_interface(selection.value().interface_id);
        if (!iface) return consumer(*chain, iface.error());
        native.interface = std::move(iface).value();
        // Exact: refresh this interface's list, then require an exact listed ID.
        if (auto s = native.interface->update_devices(options_.discovery_timeout); !s)
            return consumer(*chain, s.error());
        // Access is exactly the configured mode; there is no escalation path.
        const auto access =
            options_.access == DeviceAccess::control ? gentl::Access::control : gentl::Access::read_only;
        auto device = native.interface->open_device(selection.value().device_id, access);
        if (!device) return consumer(*chain, device.error());
        native.device = std::move(device).value();
        auto genapi = GenApiSession::open(native.device, access, XmlLimits{options_.max_xml_bytes});
        if (!genapi) return fail(genapi.error().code, "GenTL device '" + id + "': " + genapi.error().message);
        native.genapi = std::move(genapi).value();
        // The injected profile alone decides compatibility; no fallback profile.
        std::optional<Result<std::shared_ptr<const profiles::ICameraProfileState>>> admitted;
        auto status = native.genapi->transaction([&](device_features::IDeviceFeatures& features) -> Status {
            admitted.emplace(profile_->open(features));
            return {};
        });
        if (!status) return status.error();
        if (!*admitted)
            return fail(admitted->error().code, "open '" + id + "' with profile '" + std::string(profile_->id()) +
                                                    "': " + admitted->error().message);
        auto state = std::move(*admitted).value();
        if (!state) return fail(ErrorCode::invalid_state, "profile open returned null state");
        const auto caps = state->capabilities();
        // Every allocation precedes the transfer. If one fails, the local
        // chain (the original owner) is cleaned up exactly once by open().
        GIGE_MEL_GENTL_ALLOCATION(inspection);
        const auto& xml = native.genapi->xml();
        Inspection inspection{xml.url, xml.length, xml.sha1_verified, xml.schema, xml.skipped,
                              state->native_diagnostics()};
        auto opened = std::allocate_shared<GenTLDevice>(device_allocator<GenTLDevice>(), profile_, std::move(state),
                                                        caps, id, std::move(inspection));
        opened->adopt(std::move(chain)); // irreversible; the identity is already registered
        return std::shared_ptr<ICameraDevice>(std::move(opened));
    }

    // Refuses a blocked producer, then prepares a chain (no native call).
    Result<std::unique_ptr<NativeChain>> prepare_locked(std::string identity) {
        if (producer_ && producer_->tainted())
            return fail(ErrorCode::busy, "GenTL producer reported an unresolved native close; it is retained for "
                                         "the process lifetime and refuses further operations");
        return NativeChain::prepare(ownership_, std::move(identity));
    }

    // A failed consumer open taints the producer when its own cleanup of a
    // just-opened handle also failed. That handle's parents must then stay
    // open. The flag is read under mutex_, which also serializes every
    // consumer open that can taint it. A producer that was already tainted is
    // refused in prepare_locked(), so any taint seen here came from this
    // operation.
    Error consumer(NativeChain& chain, const gentl::Failure& failure) {
        if (producer_ && producer_->tainted()) chain.mark_child_unresolved();
        return from(failure);
    }

    // Producer loaded on first explicit use, then retained by this backend.
    // The opened system is owned by the chain from the moment it exists.
    Status open_system_locked(NativeChain& chain) {
        if (!producer_) {
            auto loaded = gentl::Producer::load(options_.producer_path);
            if (!loaded) return from(loaded.error());
            producer_ = loaded.value();
        }
        chain.native.producer = producer_;
        auto system = producer_->open_system();
        if (!system) return consumer(chain, system.error());
        chain.native.system = std::move(system).value();
        if (auto s = chain.native.system->update_interfaces(options_.discovery_timeout); !s)
            return consumer(chain, s.error());
        return {};
    }

    // The single failure-path teardown. It keeps the first error and appends
    // the cleanup diagnostics. A chain that already retired itself, or that a
    // device adopted, is not touched again. If even the combined text cannot be
    // allocated, the cleanup error (which states the unresolved ownership) is
    // returned in preference: a cleanup failure is never concealed.
    static Error cleanup(std::unique_ptr<NativeChain>& chain, Error error) noexcept {
        if (!chain || chain->done()) return error;
        auto closed = chain->close();
        if (!closed) return error;
        try {
            error.message += "; cleanup: " + closed->message;
            return error;
        } catch (...) {
            return std::move(*closed);
        }
    }

    Status list_devices(NativeChain& chain, std::map<std::string, DeviceInfo>& found) {
        auto& iface = *chain.native.interface;
        if (auto s = iface.update_devices(options_.discovery_timeout); !s) return consumer(chain, s.error());
        auto devices = iface.devices();
        if (!devices) return consumer(chain, devices.error());
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
    const std::shared_ptr<Ownership> ownership_; // shared with every chain; outlives this backend
    std::shared_ptr<gentl::Producer> producer_;  // explicit; children keep it alive
};

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

