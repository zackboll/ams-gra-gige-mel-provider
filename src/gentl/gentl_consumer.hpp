#pragma once

// GIGE-MEL-GENTL-001 feasibility foundation: internal, uninstalled, built only
// with -DGIGE_MEL_BUILD_GENTL_PROBE=ON. Never linked into the MEL provider,
// selection catalog or any factory; not a camera backend. The verified EMVA
// GenTL.h is included only by the implementation; no GenTL type crosses here.
// Nothing is loaded, opened or discovered by construction or static init.
//
// Ownership: Producer <- System <- Interface <- Device <- Stream. Children own
// their parent, so the producer library stays mapped while any native handle,
// registered event or announced buffer depends on it. close() is idempotent; a
// parent refuses close() while a child is open. Destructors close leftovers.

#include "device_registers.hpp"

#include <gige_mel/backend.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gige_mel::gentl {

using backend::ErrorCode;

// `native` is the GC_ERROR of `function`, or 0 for a consumer-side refusal
// made before any producer call. `detail` keeps GCGetLastError text.
struct Failure {
    ErrorCode code{ErrorCode::io_error};
    std::int32_t native{0};
    std::string function;
    std::string detail;
    [[nodiscard]] std::string message() const;
};

template <class T> class Result {
public:
    Result(T value) : value_(std::in_place_index<0>, std::move(value)) {}
    Result(Failure failure) : value_(std::in_place_index<1>, std::move(failure)) {}
    explicit operator bool() const noexcept { return value_.index() == 0; }
    T& value() & { return std::get<0>(value_); }
    const T& value() const& { return std::get<0>(value_); }
    T&& value() && { return std::get<0>(std::move(value_)); }
    const Failure& error() const { return std::get<1>(value_); }
private:
    std::variant<T, Failure> value_;
};
template <> class Result<void> {
public:
    Result() = default;
    Result(Failure failure) : failure_(std::move(failure)) {}
    explicit operator bool() const noexcept { return !failure_; }
    const Failure& error() const { return *failure_; }
private:
    std::optional<Failure> failure_;
};
using Status = Result<void>;

std::string_view gc_error_name(std::int32_t code) noexcept;
ErrorCode translate(std::int32_t code) noexcept;
// Producers whose GCCloseLib failed and were deliberately kept mapped.
std::size_t retained_producers() noexcept;

// One independently queried producer fact; a failed query stays a failure.
struct Property {
    std::string name;
    Result<std::string> text;
};
const Property* find(const std::vector<Property>&, std::string_view name) noexcept;

// GenTL "Local:[///]file;address;length" (hexadecimal address and length).
struct LocalUrl {
    std::string filename;
    std::uint64_t address{0}, length{0};
};
std::optional<LocalUrl> parse_local_url(std::string_view url);

struct Api;
class System;
class Interface;
class Device;
class Stream;

// Threading: objects are driven by one control thread. Exceptions: Device
// register/port operations are serialized by the device mutex, and
// Stream::request_stop() may be called from any thread.

class Producer : public std::enable_shared_from_this<Producer> {
public:
    // Requires an absolute path. Refuses a second in-process load of the same
    // file (GenTL libraries are process singletons). dlopen(RTLD_NOW |
    // RTLD_LOCAL), resolve and validate every mandatory symbol BEFORE calling
    // into the producer, then GCInitLib. Failures release everything acquired.
    static Result<std::shared_ptr<Producer>> load(const std::string& absolute_path);
    ~Producer();
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::vector<std::string>& missing_optional() const noexcept { return missing_; }
    [[nodiscard]] bool has_port_write() const noexcept;
    [[nodiscard]] bool has_acquisition() const noexcept;
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    std::vector<Property> info();                   // GCGetInfo
    Result<std::shared_ptr<System>> open_system();  // TLOpen
    // GCCloseLib; refused (no native call) while a system is open. The
    // library is unmapped when the last owner releases it, and only when
    // GCCloseLib succeeded and no native close/revoke failed before it.
    Status close();
    // Memory whose announcement could not be revoked: freed only after dlclose.
    void quarantine(std::shared_ptr<void> memory);
    // Any failed native close/revoke means the producer may still own live
    // handles or threads: the library is then never unmapped (see ~Producer).
    void taint() noexcept { tainted_ = true; }
    [[nodiscard]] bool tainted() const noexcept { return tainted_; }
    [[nodiscard]] std::size_t quarantined() const noexcept { return quarantine_.size(); }
    [[nodiscard]] const Api& api() const noexcept { return *api_; }
    // Failure text from GCGetLastError for a failed call on this thread.
    Failure failure(std::string_view function, std::int32_t code) const;

private:
    struct Library;
    struct Registration;
    friend class System;
    Producer();
    // ~Producer calls GCCloseLib if still initialized. On success (and if not
    // tainted), members are destroyed in reverse order: library_ (dlclose)
    // first, then the function table, then quarantined memory. If GCCloseLib
    // fails or the producer is tainted, the mapping, the registration and
    // quarantined memory are deliberately retained for the rest of the process
    // instead of unmapping code that may still run.
    std::unique_ptr<Registration> registration_;
    std::vector<std::shared_ptr<void>> quarantine_;
    std::unique_ptr<Api> api_;
    std::vector<std::string> missing_;
    std::string path_;
    std::size_t systems_{0};
    bool closed_{false}, tainted_{false};
    std::unique_ptr<Library> library_;
};

struct InterfaceDescription {
    std::string id;
    std::vector<Property> properties;
};

class System : public std::enable_shared_from_this<System> {
public:
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;
    std::vector<Property> info(); // TLGetInfo
    // TLUpdateInterfaceList: host interface enumeration, not device discovery.
    Status update_interfaces(std::chrono::milliseconds timeout);
    // query_info=false lists IDs only (no TLGetInterfaceInfo). Needed for
    // producers that fault on optional info queries (see validation record).
    Result<std::vector<InterfaceDescription>> interfaces(bool query_info = true);
    // Exact listed interface ID only: no index, prefix or fallback selection.
    Result<std::shared_ptr<Interface>> open_interface(std::string_view id);
    Status close(); // TLClose; refused while an interface is open
private:
    friend class Producer;
    friend class Interface;
    friend class Device;
    System(std::shared_ptr<Producer>, void*);
    Result<std::vector<std::string>> ids();
    std::shared_ptr<Producer> producer_;
    void* handle_;
    std::size_t interfaces_{0};
    bool closed_{false};
};
struct DeviceDescription {
    std::string id;
    std::vector<Property> properties;
};
enum class Access { read_only, control, exclusive };

class Interface : public std::enable_shared_from_this<Interface> {
public:
    ~Interface();
    Interface(const Interface&) = delete;
    Interface& operator=(const Interface&) = delete;
    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    // IFUpdateDeviceList. For network producers this IS device discovery.
    Status update_devices(std::chrono::milliseconds timeout);
    Result<std::vector<DeviceDescription>> devices(); // cached list only
    // Exact listed device ID only; IFOpenDevice is never called otherwise.
    // A failure after IFOpenDevice closes the partially opened device.
    Result<std::shared_ptr<Device>> open_device(std::string_view id, Access access);
    Status close(); // IFClose; refused while a device is open
private:
    friend class System;
    friend class Device;
    Interface(std::shared_ptr<System>, void*, std::string);
    Result<std::vector<std::string>> ids();
    std::shared_ptr<System> system_;
    void* handle_;
    std::string id_;
    std::size_t devices_{0};
    bool closed_{false};
};

enum class PortByteOrder { little_endian, big_endian };

class Device final : public backend::device_registers::IRegisterDevice,
                     public std::enable_shared_from_this<Device> {
public:
    struct StreamOptions {
        std::size_t buffers{4};                         // GenTL buffers == host lease slots
        std::size_t max_payload_bytes{64u << 20};
        std::size_t max_total_bytes{256u << 20};        // announced + lease memory
    };
    ~Device() override;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    std::vector<Property> info();      // DevGetInfo (local device module)
    std::vector<Property> port_info(); // GCGetPortInfo on the remote device port
    // Exactly one of PORT_INFO_LITTLE_ENDIAN / PORT_INFO_BIG_ENDIAN must be
    // declared true; anything else is unsupported rather than assumed.
    Result<PortByteOrder> port_byte_order();
    Result<std::vector<std::vector<Property>>> xml_locations(); // GCGetNumPortURLs/GCGetPortURLInfo
    // Exact-length GCReadPort (read-only), 1 byte .. 1 MiB.
    Status read_port(std::uint64_t address, std::span<std::byte> destination);
    // Existing transport contract (validation, 32-bit window, 4096-byte limit,
    // write-completion semantics) over GCReadPort/GCWritePort under the device
    // mutex. Words use the declared port byte order; no retries or rollback.
    backend::device_registers::Status with_registers(
        const std::function<backend::device_registers::Status(backend::device_registers::IDeviceRegisters&)>&) override;
    Result<std::vector<std::string>> stream_ids();
    Result<std::shared_ptr<Stream>> open_stream(std::string_view id, StreamOptions options);
    Status close(); // DevClose; refused while a stream is open
private:
    friend class Interface;
    friend class Stream;
    class Port;
    Device(std::shared_ptr<Interface>, void* device, void* port, std::string id);
    Status open_locked() const;
    std::shared_ptr<Interface> interface_;
    std::shared_ptr<Producer> producer_;
    void* handle_;
    void* port_;
    std::string id_;
    mutable std::mutex mutex_;
    std::size_t streams_{0};
    bool closed_{false};
};

// Facts from one delivered buffer. Absent = not supplied; failed queries are
// listed in `notes`. Timestamps are raw producer values in an UNKNOWN clock
// domain: never host-monotonic, UTC or integration-start time.
struct FrameFacts {
    std::optional<std::uint64_t> frame_id, timestamp_ticks, timestamp_ns;
    std::optional<std::uint64_t> pixel_format, pixel_format_namespace;
    std::optional<std::size_t> size_filled, width, height, payload_type;
    std::optional<bool> incomplete;
    std::size_t capacity{0};
    std::vector<std::string> notes;
    [[nodiscard]] bool mono8() const noexcept; // PFNC 32-bit namespace Mono8
};

// Copy-first lease (ADR 0002): bytes are consumer-owned and stay valid after
// stream stop/close, device close and producer unload.
class Frame {
public:
    Frame(FrameFacts facts, std::span<const std::byte> bytes, std::shared_ptr<const void> owner) noexcept
        : facts_(std::move(facts)), bytes_(bytes), owner_(std::move(owner)) {}
    Frame(Frame&&) noexcept = default;
    Frame& operator=(Frame&&) noexcept = default;
    [[nodiscard]] const FrameFacts& facts() const noexcept { return facts_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
private:
    FrameFacts facts_;
    std::span<const std::byte> bytes_;
    std::shared_ptr<const void> owner_;
};

struct StreamCounters {
    std::uint64_t delivered{0}, incomplete{0}, timeouts{0}, starvation{0};
};

// Host acquisition engine only: DSStartAcquisition does NOT execute the remote
// device's AcquisitionStart (a GenApi command this foundation does not issue).
// One thread calls start/wait/stop/close; request_stop() may run concurrently
// and wakes a blocked wait() through EventKill. The caller must ensure no
// thread is inside wait() before stop()/close()/destruction.
class Stream {
public:
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    std::vector<Property> info(); // DSGetInfo
    // Requires STREAM_INFO_DEFINES_PAYLOADSIZE (no GenApi PayloadSize here).
    // Announces consumer memory, queues, registers NEW_BUFFER, starts. Refused
    // while an earlier lease is held. Failure revokes everything announced.
    Status start();
    Result<Frame> wait(std::chrono::milliseconds timeout);
    void request_stop() noexcept;
    // DSStopAcquisition(KILL), DSFlushQueue(ALL_DISCARD), DSRevokeBuffer each,
    // GCUnregisterEvent. Unrevoked memory is quarantined in the producer.
    Status stop();
    Status close(); // stop() then DSClose
    [[nodiscard]] StreamCounters counters() const;
private:
    friend class Device;
    struct Slot;
    Stream(std::shared_ptr<Device>, void*, Device::StreamOptions);
    Status teardown();
    FrameFacts read_facts(const Slot&);
    std::shared_ptr<Device> device_;
    std::shared_ptr<Producer> producer_;
    void* handle_;
    Device::StreamOptions options_;
    std::vector<std::shared_ptr<Slot>> slots_;
    std::vector<std::shared_ptr<std::vector<std::byte>>> leases_;
    std::mutex event_mutex_;
    void* event_{nullptr};
    std::atomic<bool> stop_requested_{false};
    bool running_{false}, closed_{false}, engine_attempted_{false};
    mutable std::mutex counters_mutex_;
    StreamCounters counters_;
};

} // namespace gige_mel::gentl
