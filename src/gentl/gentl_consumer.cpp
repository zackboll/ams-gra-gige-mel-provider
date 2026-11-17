#include "gentl/gentl_consumer.hpp"
#include "gentl/gentl_api.hpp"
#include "profiles/frame_policy.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <set>
#include <type_traits>

namespace gige_mel::gentl {
#ifdef GIGE_MEL_GENTL_TESTING
thread_local int construction_allocation = -1;
void fail_construction_allocation(int index) noexcept { construction_allocation = index; }
#endif
namespace {

void construction_checkpoint() {
#ifdef GIGE_MEL_GENTL_TESTING
    if (construction_allocation >= 0 && construction_allocation-- == 0) throw std::bad_alloc();
#endif
}
template <class T> struct ConstructionAllocator {
    using value_type = T;
    ConstructionAllocator() = default;
    template <class U> ConstructionAllocator(const ConstructionAllocator<U>&) {}
    T* allocate(std::size_t n) { construction_checkpoint(); return std::allocator<T>{}.allocate(n); }
    void deallocate(T* p, std::size_t n) noexcept { std::allocator<T>{}.deallocate(p, n); }
};

// Sole owner until a fully allocated, inert child can adopt the handle. Inert
// children have null handles and neither close natives nor decrement counts.
template <class Close> class NativeOpen {
public:
    NativeOpen(Producer& producer, void* handle, const char* name, Close close)
        : producer_(producer), handle_(handle), name_(name), close_(close) {}
    ~NativeOpen() {
        // Unwinding must not allocate diagnostic text (notably under real OOM).
        if (handle_ && close_(handle_) != G::GC_ERR_SUCCESS) producer_.taint();
    }
    void release() noexcept { handle_ = nullptr; }
    Failure abandon(Failure original) {
        if (auto failure = cleanup()) original.detail += "; cleanup " + failure->message();
        return original;
    }
private:
    std::optional<Failure> cleanup() {
        const auto status = close_(std::exchange(handle_, nullptr));
        if (status == G::GC_ERR_SUCCESS) return {};
        producer_.taint();
        return producer_.failure(name_, status);
    }
    Producer& producer_;
    void* handle_;
    const char* name_;
    Close close_;
};

constexpr std::size_t max_info_bytes = 64 * 1024;
constexpr std::uint32_t max_entries = 4096;
constexpr std::size_t max_port_read = 1024 * 1024;

std::mutex registry_mutex;
std::set<std::string>& registry() {
    static std::set<std::string> paths; // canonical producer files currently loaded
    return paths;
}

// Producers in unknown state (failed GCCloseLib or failed native close/revoke):
// retained (still reachable) for the rest of the process instead of unmapping
// code a producer thread may still run or freeing memory it may still write.
struct Retained {
    void* library{nullptr}; // never dlclosed
    std::vector<std::shared_ptr<void>> memory;
    std::string path;
};
std::vector<Retained>& retained() {
    static std::vector<Retained> values;
    return values;
}

Failure refuse(ErrorCode code, std::string function, std::string detail) {
    return Failure{code, 0, std::move(function), std::move(detail)};
}

template <class T> std::string number(T value) {
    std::array<char, 64> text{};
    const auto end = std::to_chars(text.data(), text.data() + text.size(), value).ptr;
    return std::string(text.data(), end);
}

std::string hex(std::uint64_t value) {
    std::array<char, 32> text{};
    const auto end = std::to_chars(text.data(), text.data() + text.size(), value, 16).ptr;
    return "0x" + std::string(text.data(), end);
}

template <class T> T load_as(const std::vector<unsigned char>& data) {
    T value{};
    std::memcpy(&value, data.data(), sizeof(T));
    return value;
}

// Formats one producer-supplied value according to its declared datatype.
Result<std::string> render(std::string_view function, G::INFO_DATATYPE type,
                           const std::vector<unsigned char>& data, std::size_t size) {
    const auto exact = [&](std::size_t expected) -> Status {
        if (size == expected) return {};
        return refuse(ErrorCode::io_error, std::string(function),
                      "datatype " + number(type) + " returned " + number(size) + " bytes, expected " + number(expected));
    };
    switch (type) {
    case G::INFO_DATATYPE_STRING:
        return std::string(reinterpret_cast<const char*>(data.data()), strnlen(reinterpret_cast<const char*>(data.data()), size));
    case G::INFO_DATATYPE_STRINGLIST: {
        std::string text(reinterpret_cast<const char*>(data.data()), size);
        while (!text.empty() && text.back() == '\0') text.pop_back();
        std::replace(text.begin(), text.end(), '\0', ';');
        return text;
    }
#define GIGE_MEL_GENTL_FORMAT(enumerator, type_name) \
    case G::enumerator: { if (auto s = exact(sizeof(type_name)); !s) return s.error(); return number(load_as<type_name>(data)); }
    GIGE_MEL_GENTL_FORMAT(INFO_DATATYPE_INT16, std::int16_t)
    GIGE_MEL_GENTL_FORMAT(INFO_DATATYPE_UINT16, std::uint16_t)
    GIGE_MEL_GENTL_FORMAT(INFO_DATATYPE_INT32, std::int32_t)
    GIGE_MEL_GENTL_FORMAT(INFO_DATATYPE_UINT32, std::uint32_t)
    GIGE_MEL_GENTL_FORMAT(INFO_DATATYPE_INT64, std::int64_t)
    GIGE_MEL_GENTL_FORMAT(INFO_DATATYPE_UINT64, std::uint64_t)
    GIGE_MEL_GENTL_FORMAT(INFO_DATATYPE_SIZET, std::size_t)
    GIGE_MEL_GENTL_FORMAT(INFO_DATATYPE_PTRDIFF, std::ptrdiff_t)
#undef GIGE_MEL_GENTL_FORMAT
    case G::INFO_DATATYPE_FLOAT64: {
        if (auto s = exact(sizeof(double)); !s) return s.error();
        return number(load_as<double>(data));
    }
    case G::INFO_DATATYPE_BOOL8:
        if (auto s = exact(1); !s) return s.error();
        return std::string(data[0] ? "true" : "false");
    case G::INFO_DATATYPE_PTR:
        if (auto s = exact(sizeof(void*)); !s) return s.error();
        return std::string("pointer ") + hex(reinterpret_cast<std::uintptr_t>(load_as<void*>(data)));
    case G::INFO_DATATYPE_BUFFER: {
        static constexpr char digits[] = "0123456789abcdef";
        std::string text = "buffer[" + number(size) + "] ";
        for (std::size_t i = 0; i < std::min<std::size_t>(size, 32); ++i) {
            text += digits[data[i] >> 4];
            text += digits[data[i] & 15];
        }
        return text;
    }
    default:
        return refuse(ErrorCode::unsupported, std::string(function), "undeclared datatype " + number(type));
    }
}

} // namespace

std::string_view gc_error_name(std::int32_t code) noexcept {
    switch (code) {
#define GIGE_MEL_GENTL_NAME(x) case G::x: return #x;
    GIGE_MEL_GENTL_NAME(GC_ERR_SUCCESS) GIGE_MEL_GENTL_NAME(GC_ERR_ERROR)
    GIGE_MEL_GENTL_NAME(GC_ERR_NOT_INITIALIZED) GIGE_MEL_GENTL_NAME(GC_ERR_NOT_IMPLEMENTED)
    GIGE_MEL_GENTL_NAME(GC_ERR_RESOURCE_IN_USE) GIGE_MEL_GENTL_NAME(GC_ERR_ACCESS_DENIED)
    GIGE_MEL_GENTL_NAME(GC_ERR_INVALID_HANDLE) GIGE_MEL_GENTL_NAME(GC_ERR_INVALID_ID)
    GIGE_MEL_GENTL_NAME(GC_ERR_NO_DATA) GIGE_MEL_GENTL_NAME(GC_ERR_INVALID_PARAMETER)
    GIGE_MEL_GENTL_NAME(GC_ERR_IO) GIGE_MEL_GENTL_NAME(GC_ERR_TIMEOUT)
    GIGE_MEL_GENTL_NAME(GC_ERR_ABORT) GIGE_MEL_GENTL_NAME(GC_ERR_INVALID_BUFFER)
    GIGE_MEL_GENTL_NAME(GC_ERR_NOT_AVAILABLE) GIGE_MEL_GENTL_NAME(GC_ERR_INVALID_ADDRESS)
    GIGE_MEL_GENTL_NAME(GC_ERR_BUFFER_TOO_SMALL) GIGE_MEL_GENTL_NAME(GC_ERR_INVALID_INDEX)
    GIGE_MEL_GENTL_NAME(GC_ERR_PARSING_CHUNK_DATA) GIGE_MEL_GENTL_NAME(GC_ERR_INVALID_VALUE)
    GIGE_MEL_GENTL_NAME(GC_ERR_RESOURCE_EXHAUSTED) GIGE_MEL_GENTL_NAME(GC_ERR_OUT_OF_MEMORY)
    GIGE_MEL_GENTL_NAME(GC_ERR_BUSY) GIGE_MEL_GENTL_NAME(GC_ERR_AMBIGUOUS)
#undef GIGE_MEL_GENTL_NAME
    default: return code <= G::GC_ERR_CUSTOM_ID ? "GC_ERR_CUSTOM" : "GC_ERR_UNKNOWN";
    }
}
ErrorCode translate(std::int32_t code) noexcept {
    switch (code) {
    case G::GC_ERR_TIMEOUT: return ErrorCode::timeout;
    case G::GC_ERR_ABORT: return ErrorCode::stopped;
    case G::GC_ERR_NOT_IMPLEMENTED: case G::GC_ERR_NOT_AVAILABLE: return ErrorCode::unsupported;
    case G::GC_ERR_RESOURCE_IN_USE: case G::GC_ERR_BUSY: case G::GC_ERR_ACCESS_DENIED: return ErrorCode::busy;
    case G::GC_ERR_INVALID_ID: case G::GC_ERR_INVALID_INDEX: return ErrorCode::not_found;
    case G::GC_ERR_INVALID_PARAMETER: case G::GC_ERR_INVALID_ADDRESS: case G::GC_ERR_INVALID_VALUE:
    case G::GC_ERR_INVALID_BUFFER: case G::GC_ERR_BUFFER_TOO_SMALL: return ErrorCode::invalid_argument;
    case G::GC_ERR_NOT_INITIALIZED: case G::GC_ERR_INVALID_HANDLE: return ErrorCode::invalid_state;
    case G::GC_ERR_RESOURCE_EXHAUSTED: case G::GC_ERR_OUT_OF_MEMORY: return ErrorCode::resource_exhausted;
    default: return ErrorCode::io_error;
    }
}

std::string Failure::message() const {
    std::string text = "GenTL " + function;
    if (native) text += " returned " + std::string(gc_error_name(native)) + " (" + number(native) + ")";
    else text += " refused";
    if (!detail.empty()) text += ": " + detail;
    return text;
}

const Property* find(const std::vector<Property>& properties, std::string_view name) noexcept {
    for (const auto& property : properties)
        if (property.name == name) return &property;
    return nullptr;
}

std::optional<LocalUrl> parse_local_url(std::string_view url) {
    constexpr std::string_view scheme = "local:";
    if (url.size() < scheme.size()) return std::nullopt;
    for (std::size_t i = 0; i < scheme.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(url[i])) != scheme[i]) return std::nullopt;
    url.remove_prefix(scheme.size());
    while (!url.empty() && url.front() == '/') url.remove_prefix(1);
    if (const auto query = url.find('?'); query != std::string_view::npos) url = url.substr(0, query);
    const auto first = url.find(';');
    const auto second = first == std::string_view::npos ? first : url.find(';', first + 1);
    if (first == 0 || second == std::string_view::npos || url.find(';', second + 1) != std::string_view::npos)
        return std::nullopt;
    const auto parse = [](std::string_view text, std::uint64_t& value) {
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
        const auto r = std::from_chars(text.data(), text.data() + text.size(), value, 16);
        return !text.empty() && r.ec == std::errc{} && r.ptr == text.data() + text.size();
    };
    LocalUrl result;
    result.filename = std::string(url.substr(0, first));
    if (!parse(url.substr(first + 1, second - first - 1), result.address) ||
        !parse(url.substr(second + 1), result.length) || result.length == 0)
        return std::nullopt;
    return result;
}

namespace {
// Size probe (null buffer), then fetch. Producer failures keep GCGetLastError.
template <class Call>
Result<std::string> query(const Producer& producer, std::string_view function, Call call) {
    G::INFO_DATATYPE type = G::INFO_DATATYPE_UNKNOWN;
    std::size_t size = 0;
    auto status = call(&type, nullptr, &size);
    if (status != G::GC_ERR_SUCCESS) return producer.failure(function, status);
    if (size == 0 || size > max_info_bytes)
        return refuse(ErrorCode::io_error, std::string(function), "implausible value size " + number(size));
    std::vector<unsigned char> data(size + 1, 0);
    std::size_t actual = size;
    status = call(&type, data.data(), &actual);
    if (status != G::GC_ERR_SUCCESS) return producer.failure(function, status);
    if (actual > size)
        return refuse(ErrorCode::io_error, std::string(function), "producer wrote beyond its announced size");
    return render(function, type, data, actual);
}

// ID getters have no datatype argument; the result is always a C string.
template <class Call>
Result<std::string> query_id(const Producer& producer, std::string_view function, Call call) {
    return query(producer, function, [&](G::INFO_DATATYPE* type, void* buffer, std::size_t* size) {
        *type = G::INFO_DATATYPE_STRING;
        return call(static_cast<char*>(buffer), size);
    });
}
} // namespace
// ---- Producer ---------------------------------------------------------------

struct Producer::Library {
    void* handle{nullptr};
    ~Library() { if (handle) dlclose(handle); }
};

struct Producer::Registration {
    std::string canonical;
    bool keep{false}; // never inserted, or producer retained in unknown state
    ~Registration() {
        if (keep) return;
        std::lock_guard lock(registry_mutex);
        registry().erase(canonical);
    }
};

Producer::Producer() = default;

Producer::~Producer() {
    // Children own their parent, so no child can exist here.
    if (!closed_ && api_ && api_->GCCloseLib && api_->GCCloseLib() == G::GC_ERR_SUCCESS) closed_ = true;
    if (closed_ && !tainted_) return; // members destroyed in order: dlclose, then memory
    if (!library_ || !library_->handle) return; // never loaded: nothing can be running
    // Unknown producer state (failed GCCloseLib or an earlier failed native
    // close/revoke): never unmap its code or free memory it may still reference.
    // Retained and still registered, so a reload in this process is refused.
    if (registration_) registration_->keep = true;
    Retained kept;
    kept.library = library_ ? library_->handle : nullptr;
    if (library_) library_->handle = nullptr;
    kept.memory = std::move(quarantine_);
    kept.path = path_;
    std::lock_guard lock(registry_mutex);
    retained().push_back(std::move(kept));
}

std::size_t retained_producers() noexcept {
    std::lock_guard lock(registry_mutex);
    return retained().size();
}

Failure Producer::failure(std::string_view function, std::int32_t code) const {
    Failure failure{translate(code), code, std::string(function), {}};
    if (!api_ || !api_->GCGetLastError) return failure;
    G::GC_ERROR last = G::GC_ERR_SUCCESS;
    std::array<char, 1024> text{};
    std::size_t size = text.size();
    if (api_->GCGetLastError(&last, text.data(), &size) == G::GC_ERR_SUCCESS) {
        text.back() = '\0';
        failure.detail = text.data();
        if (last != code && last != G::GC_ERR_SUCCESS)
            failure.detail += " [last error " + std::string(gc_error_name(last)) + "]";
    }
    return failure;
}

bool Producer::has_port_write() const noexcept { return api_ && api_->GCWritePort; }
bool Producer::has_acquisition() const noexcept { return api_ && api_->DSStartAcquisition; }

void Producer::quarantine(std::shared_ptr<void> memory) {
    if (memory) quarantine_.push_back(std::move(memory));
}
void Producer::release_quarantine(const std::shared_ptr<void>& memory) {
    std::erase(quarantine_, memory);
}

std::vector<Property> Producer::info() {
    std::vector<Property> result;
    if (closed_) return result;
    const std::pair<const char*, G::TL_INFO_CMD> commands[]{
        {"TL_INFO_ID", G::TL_INFO_ID}, {"TL_INFO_VENDOR", G::TL_INFO_VENDOR},
        {"TL_INFO_MODEL", G::TL_INFO_MODEL}, {"TL_INFO_VERSION", G::TL_INFO_VERSION},
        {"TL_INFO_TLTYPE", G::TL_INFO_TLTYPE}, {"TL_INFO_NAME", G::TL_INFO_NAME},
        {"TL_INFO_PATHNAME", G::TL_INFO_PATHNAME}, {"TL_INFO_DISPLAYNAME", G::TL_INFO_DISPLAYNAME},
        {"TL_INFO_GENTL_VER_MAJOR", G::TL_INFO_GENTL_VER_MAJOR},
        {"TL_INFO_GENTL_VER_MINOR", G::TL_INFO_GENTL_VER_MINOR}};
    for (const auto& [name, command] : commands)
        result.push_back({name, query(*this, "GCGetInfo", [&](auto* type, void* buffer, std::size_t* size) {
            return api_->GCGetInfo(command, type, buffer, size);
        })});
    return result;
}

Result<std::shared_ptr<System>> Producer::open_system() {
    if (closed_) return refuse(ErrorCode::invalid_state, "TLOpen", "producer library is closed");
    void* handle = nullptr;
    const auto status = api_->TLOpen(&handle);
    if (status != G::GC_ERR_SUCCESS) return failure("TLOpen", status);
    if (!handle) return refuse(ErrorCode::io_error, "TLOpen", "succeeded with a null handle");
    NativeOpen owner(*this, handle, "TLClose", api_->TLClose);
    try {
        construction_checkpoint();
        std::shared_ptr<System> system(new System(shared_from_this(), nullptr),
                                      std::default_delete<System>{}, ConstructionAllocator<System>{});
        system->handle_ = handle;
        ++systems_;
        owner.release();
        return system;
    } catch (...) {
        return owner.abandon(refuse(ErrorCode::resource_exhausted, "TLOpen", "allocation failed"));
    }
}

Status Producer::close() {
    if (closed_) return {};
    if (systems_) return refuse(ErrorCode::busy, "GCCloseLib", "a transport-layer system is still open");
    const auto status = api_->GCCloseLib();
    if (status != G::GC_ERR_SUCCESS) return failure("GCCloseLib", status);
    closed_ = true;
    return {};
}
Result<std::shared_ptr<Producer>> Producer::load(const std::string& path) try {
    if (path.empty() || path.find('\0') != std::string::npos || !std::filesystem::path(path).is_absolute())
        return refuse(ErrorCode::invalid_argument, "load", "producer requires an explicit absolute .cti path");
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(path, ec);
    if (ec) return refuse(ErrorCode::not_found, "load", path + ": " + ec.message());
    if (!std::filesystem::is_regular_file(canonical, ec))
        return refuse(ErrorCode::not_found, "load", path + " is not a regular file");
    std::shared_ptr<Producer> producer(new Producer());
    producer->path_ = path;
    producer->api_ = std::make_unique<Api>();
    producer->closed_ = true; // nothing to GCCloseLib until GCInitLib succeeds
    auto registration = std::make_unique<Registration>();
    registration->canonical = canonical.string();
    registration->keep = true; // not yet inserted: destruction must not erase another owner's entry
    {
        std::lock_guard lock(registry_mutex);
        if (!registry().insert(registration->canonical).second)
            return refuse(ErrorCode::busy, "load", registration->canonical +
                          " is already loaded (or retained) in this process; GenTL producers are process singletons");
        registration->keep = false; // no allocation between insert and ownership transfer
    }
    producer->registration_ = std::move(registration);
    producer->library_ = std::make_unique<Library>();
    dlerror();
    producer->library_->handle = dlopen(canonical.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!producer->library_->handle) {
        const char* why = dlerror();
        return refuse(ErrorCode::not_found, "dlopen", why ? why : "unknown dlopen failure");
    }
    auto& api = *producer->api_;
    std::vector<std::string> missing;
    const auto resolve = [&](auto& slot, const char* name) {
        dlerror();
        void* symbol = dlsym(producer->library_->handle, name);
        if (dlerror() || !symbol) { missing.emplace_back(name); return; }
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(symbol);
    };
#define R(name) resolve(api.name, #name);
    R(GCInitLib) R(GCCloseLib) R(GCGetInfo) R(GCGetLastError) R(GCReadPort) R(GCGetPortInfo)
    R(GCGetNumPortURLs) R(GCGetPortURLInfo) R(TLOpen) R(TLClose) R(TLGetInfo) R(TLGetNumInterfaces)
    R(TLGetInterfaceID) R(TLGetInterfaceInfo) R(TLOpenInterface) R(TLUpdateInterfaceList) R(IFClose)
    R(IFGetNumDevices) R(IFGetDeviceID) R(IFUpdateDeviceList) R(IFGetDeviceInfo) R(IFOpenDevice)
    R(DevGetPort) R(DevGetInfo) R(DevClose)
    if (!missing.empty()) {
        std::string list;
        for (const auto& name : missing) list += (list.empty() ? "" : ", ") + name;
        return refuse(ErrorCode::unsupported, "dlsym",
                      "missing mandatory entry points: " + list + "; no producer function was called");
    }
    R(GCWritePort)
    producer->missing_ = missing;
    missing.clear();
    R(DevGetNumDataStreams) R(DevGetDataStreamID) R(DevOpenDataStream) R(DSGetInfo) R(DSAnnounceBuffer)
    R(DSQueueBuffer) R(DSRevokeBuffer) R(DSFlushQueue) R(DSStartAcquisition) R(DSStopAcquisition)
    R(DSGetBufferInfo) R(DSClose) R(GCRegisterEvent) R(GCUnregisterEvent) R(EventGetData) R(EventKill)
#undef R
    if (!missing.empty()) { // all or nothing: never drive a partial acquisition table
        api.DevGetNumDataStreams = {}; api.DevGetDataStreamID = {}; api.DevOpenDataStream = {};
        api.DSGetInfo = {}; api.DSAnnounceBuffer = {}; api.DSQueueBuffer = {}; api.DSRevokeBuffer = {};
        api.DSFlushQueue = {}; api.DSStartAcquisition = {}; api.DSStopAcquisition = {};
        api.DSGetBufferInfo = {}; api.DSClose = {}; api.GCRegisterEvent = {}; api.GCUnregisterEvent = {};
        api.EventGetData = {}; api.EventKill = {};
        producer->missing_.insert(producer->missing_.end(), missing.begin(), missing.end());
    }
    const auto status = api.GCInitLib();
    // GCGetLastError is not consulted: the library never became initialized.
    if (status != G::GC_ERR_SUCCESS)
        return Failure{translate(status), status, "GCInitLib", "library not initialized; GCCloseLib not called, unloaded"};
    producer->closed_ = false;
    return producer;
} catch (const std::bad_alloc&) {
    return refuse(ErrorCode::resource_exhausted, "load", "allocation failed");
}
// ---- System -----------------------------------------------------------------

System::System(std::shared_ptr<Producer> producer, void* handle)
    : producer_(std::move(producer)), handle_(handle) {}

// The parent's open-child count is released exactly once: by a successful
// close() or, failing that, by the destructor's best-effort native close.
System::~System() {
    if (closed_ || !handle_) return;
    if (producer_->api().TLClose(handle_) != G::GC_ERR_SUCCESS) producer_->taint(); // children own us
    --producer_->systems_;
}

Status System::close() {
    if (closed_) return {};
    if (interfaces_) return refuse(ErrorCode::busy, "TLClose", "an interface is still open");
    const auto status = producer_->api().TLClose(handle_);
    if (status != G::GC_ERR_SUCCESS) return producer_->failure("TLClose", status);
    closed_ = true;
    --producer_->systems_;
    return {};
}

std::vector<Property> System::info() {
    std::vector<Property> result;
    if (closed_) return result;
    const std::pair<const char*, G::TL_INFO_CMD> commands[]{
        {"TL_INFO_ID", G::TL_INFO_ID}, {"TL_INFO_VENDOR", G::TL_INFO_VENDOR}, {"TL_INFO_MODEL", G::TL_INFO_MODEL},
        {"TL_INFO_VERSION", G::TL_INFO_VERSION}, {"TL_INFO_TLTYPE", G::TL_INFO_TLTYPE}};
    for (const auto& [name, command] : commands)
        result.push_back({name, query(*producer_, "TLGetInfo", [&](auto* type, void* buffer, std::size_t* size) {
            return producer_->api().TLGetInfo(handle_, command, type, buffer, size);
        })});
    return result;
}

Status System::update_interfaces(std::chrono::milliseconds timeout) {
    if (closed_) return refuse(ErrorCode::invalid_state, "TLUpdateInterfaceList", "system is closed");
    ::bool8_t changed = false;
    const auto status = producer_->api().TLUpdateInterfaceList(
        handle_, &changed, static_cast<std::uint64_t>(std::max<std::int64_t>(0, timeout.count())));
    if (status != G::GC_ERR_SUCCESS) return producer_->failure("TLUpdateInterfaceList", status);
    return {};
}

Result<std::vector<std::string>> System::ids() {
    if (closed_) return refuse(ErrorCode::invalid_state, "TLGetNumInterfaces", "system is closed");
    std::uint32_t count = 0;
    const auto status = producer_->api().TLGetNumInterfaces(handle_, &count);
    if (status != G::GC_ERR_SUCCESS) return producer_->failure("TLGetNumInterfaces", status);
    if (count > max_entries) return refuse(ErrorCode::io_error, "TLGetNumInterfaces", "implausible count " + number(count));
    std::vector<std::string> result;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto id = query_id(*producer_, "TLGetInterfaceID", [&](char* buffer, std::size_t* size) {
            return producer_->api().TLGetInterfaceID(handle_, i, buffer, size);
        });
        if (!id) return id.error();
        result.push_back(std::move(id).value());
    }
    return result;
}

Result<std::vector<InterfaceDescription>> System::interfaces(bool query_info) {
    auto list = ids();
    if (!list) return list.error();
    std::vector<InterfaceDescription> result;
    const std::pair<const char*, G::INTERFACE_INFO_CMD> commands[]{
        {"INTERFACE_INFO_DISPLAYNAME", G::INTERFACE_INFO_DISPLAYNAME}, {"INTERFACE_INFO_TLTYPE", G::INTERFACE_INFO_TLTYPE}};
    for (auto& id : list.value()) {
        InterfaceDescription description{id, {}};
        if (query_info)
            for (const auto& [name, command] : commands)
            description.properties.push_back({name, query(*producer_, "TLGetInterfaceInfo",
                [&](auto* type, void* buffer, std::size_t* size) {
                    return producer_->api().TLGetInterfaceInfo(handle_, id.c_str(), command, type, buffer, size);
                })});
        result.push_back(std::move(description));
    }
    return result;
}

Result<std::shared_ptr<Interface>> System::open_interface(std::string_view id) {
    auto list = ids();
    if (!list) return list.error();
    if (std::find(list.value().begin(), list.value().end(), id) == list.value().end())
        return refuse(ErrorCode::not_found, "TLOpenInterface", "interface '" + std::string(id) +
                      "' is not in the producer's current list; no fallback is selected");
    const std::string exact(id);
    void* handle = nullptr;
    const auto status = producer_->api().TLOpenInterface(handle_, exact.c_str(), &handle);
    if (status != G::GC_ERR_SUCCESS) return producer_->failure("TLOpenInterface", status);
    if (!handle) return refuse(ErrorCode::io_error, "TLOpenInterface", "succeeded with a null handle");
    NativeOpen owner(*producer_, handle, "IFClose", producer_->api().IFClose);
    try {
        construction_checkpoint();
        std::shared_ptr<Interface> result(new Interface(shared_from_this(), nullptr, exact),
                                         std::default_delete<Interface>{}, ConstructionAllocator<Interface>{});
        result->handle_ = handle;
        ++interfaces_;
        owner.release();
        return result;
    } catch (...) {
        return owner.abandon(refuse(ErrorCode::resource_exhausted, "TLOpenInterface", "allocation failed"));
    }
}
// ---- Interface --------------------------------------------------------------

Interface::Interface(std::shared_ptr<System> system, void* handle, std::string id)
    : system_(std::move(system)), handle_(handle), id_(std::move(id)) {}

Interface::~Interface() {
    if (closed_ || !handle_) return;
    if (system_->producer_->api().IFClose(handle_) != G::GC_ERR_SUCCESS) system_->producer_->taint();
    --system_->interfaces_;
}

Status Interface::close() {
    if (closed_) return {};
    if (devices_) return refuse(ErrorCode::busy, "IFClose", "a device is still open");
    const auto status = system_->producer_->api().IFClose(handle_);
    if (status != G::GC_ERR_SUCCESS) return system_->producer_->failure("IFClose", status);
    closed_ = true;
    --system_->interfaces_;
    return {};
}

Status Interface::update_devices(std::chrono::milliseconds timeout) {
    if (closed_) return refuse(ErrorCode::invalid_state, "IFUpdateDeviceList", "interface is closed");
    ::bool8_t changed = false;
    const auto status = system_->producer_->api().IFUpdateDeviceList(
        handle_, &changed, static_cast<std::uint64_t>(std::max<std::int64_t>(0, timeout.count())));
    if (status != G::GC_ERR_SUCCESS) return system_->producer_->failure("IFUpdateDeviceList", status);
    return {};
}

Result<std::vector<std::string>> Interface::ids() {
    if (closed_) return refuse(ErrorCode::invalid_state, "IFGetNumDevices", "interface is closed");
    const auto& producer = *system_->producer_;
    std::uint32_t count = 0;
    const auto status = producer.api().IFGetNumDevices(handle_, &count);
    if (status != G::GC_ERR_SUCCESS) return producer.failure("IFGetNumDevices", status);
    if (count > max_entries) return refuse(ErrorCode::io_error, "IFGetNumDevices", "implausible count " + number(count));
    std::vector<std::string> result;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto id = query_id(producer, "IFGetDeviceID", [&](char* buffer, std::size_t* size) {
            return producer.api().IFGetDeviceID(handle_, i, buffer, size);
        });
        if (!id) return id.error();
        result.push_back(std::move(id).value());
    }
    return result;
}

Result<std::vector<DeviceDescription>> Interface::devices() {
    auto list = ids();
    if (!list) return list.error();
    const auto& producer = *system_->producer_;
    const std::pair<const char*, G::DEVICE_INFO_CMD> commands[]{
        {"DEVICE_INFO_VENDOR", G::DEVICE_INFO_VENDOR}, {"DEVICE_INFO_MODEL", G::DEVICE_INFO_MODEL},
        {"DEVICE_INFO_SERIAL_NUMBER", G::DEVICE_INFO_SERIAL_NUMBER},
        {"DEVICE_INFO_USER_DEFINED_NAME", G::DEVICE_INFO_USER_DEFINED_NAME},
        {"DEVICE_INFO_DISPLAYNAME", G::DEVICE_INFO_DISPLAYNAME}, {"DEVICE_INFO_TLTYPE", G::DEVICE_INFO_TLTYPE},
        {"DEVICE_INFO_VERSION", G::DEVICE_INFO_VERSION}, {"DEVICE_INFO_ACCESS_STATUS", G::DEVICE_INFO_ACCESS_STATUS}};
    std::vector<DeviceDescription> result;
    for (auto& id : list.value()) {
        DeviceDescription description{id, {}};
        for (const auto& [name, command] : commands)
            description.properties.push_back({name, query(producer, "IFGetDeviceInfo",
                [&](auto* type, void* buffer, std::size_t* size) {
                    return producer.api().IFGetDeviceInfo(handle_, id.c_str(), command, type, buffer, size);
                })});
        result.push_back(std::move(description));
    }
    return result;
}

Result<std::shared_ptr<Device>> Interface::open_device(std::string_view id, Access access) {
    auto list = ids();
    if (!list) return list.error();
    if (std::find(list.value().begin(), list.value().end(), id) == list.value().end())
        return refuse(ErrorCode::not_found, "IFOpenDevice", "device '" + std::string(id) +
                      "' is not in the interface's current list; no fallback is selected");
    const auto& producer = *system_->producer_;
    const std::string exact(id);
    const G::DEVICE_ACCESS_FLAGS flags = access == Access::read_only ? G::DEVICE_ACCESS_READONLY
                                       : access == Access::control ? G::DEVICE_ACCESS_CONTROL
                                                                   : G::DEVICE_ACCESS_EXCLUSIVE;
    void* device = nullptr;
    auto status = producer.api().IFOpenDevice(handle_, exact.c_str(), flags, &device);
    if (status != G::GC_ERR_SUCCESS) return producer.failure("IFOpenDevice", status);
    if (!device) return refuse(ErrorCode::io_error, "IFOpenDevice", "succeeded with a null handle");
    NativeOpen owner(*system_->producer_, device, "DevClose", producer.api().DevClose);
    void* port = nullptr;
    status = producer.api().DevGetPort(device, &port);
    if (status != G::GC_ERR_SUCCESS) return owner.abandon(producer.failure("DevGetPort", status));
    if (!port) return owner.abandon(refuse(ErrorCode::io_error, "DevGetPort", "succeeded with a null port"));
    try {
        construction_checkpoint();
        std::shared_ptr<Device> result(new Device(shared_from_this(), nullptr, port, exact),
                                      std::default_delete<Device>{}, ConstructionAllocator<Device>{});
        result->handle_ = device;
        ++devices_;
        owner.release();
        return result;
    } catch (...) {
        return owner.abandon(refuse(ErrorCode::resource_exhausted, "IFOpenDevice", "allocation failed"));
    }
}
// ---- Device -----------------------------------------------------------------

Device::Device(std::shared_ptr<Interface> interface, void* device, void* port, std::string id)
    : interface_(std::move(interface)), producer_(interface_->system_->producer_), handle_(device), port_(port),
      id_(std::move(id)) {}

Device::~Device() {
    if (closed_ || !handle_) return;
    if (producer_->api().DevClose(handle_) != G::GC_ERR_SUCCESS) producer_->taint();
    --interface_->devices_;
}

Status Device::open_locked() const {
    if (closed_) return refuse(ErrorCode::stopped, "device", "device is closed");
    return {};
}

Status Device::close() {
    std::lock_guard lock(mutex_);
    if (closed_) return {};
    if (streams_) return refuse(ErrorCode::busy, "DevClose", "a data stream is still open");
    const auto status = producer_->api().DevClose(handle_);
    if (status != G::GC_ERR_SUCCESS) return producer_->failure("DevClose", status);
    closed_ = true;
    --interface_->devices_;
    return {};
}

std::vector<Property> Device::info() {
    std::lock_guard lock(mutex_);
    std::vector<Property> result;
    if (closed_) return result;
    const std::pair<const char*, G::DEVICE_INFO_CMD> commands[]{
        {"DEVICE_INFO_ID", G::DEVICE_INFO_ID}, {"DEVICE_INFO_VENDOR", G::DEVICE_INFO_VENDOR},
        {"DEVICE_INFO_MODEL", G::DEVICE_INFO_MODEL}, {"DEVICE_INFO_TLTYPE", G::DEVICE_INFO_TLTYPE},
        {"DEVICE_INFO_SERIAL_NUMBER", G::DEVICE_INFO_SERIAL_NUMBER}, {"DEVICE_INFO_VERSION", G::DEVICE_INFO_VERSION},
        {"DEVICE_INFO_ACCESS_STATUS", G::DEVICE_INFO_ACCESS_STATUS},
        {"DEVICE_INFO_TIMESTAMP_FREQUENCY", G::DEVICE_INFO_TIMESTAMP_FREQUENCY}};
    for (const auto& [name, command] : commands)
        result.push_back({name, query(*producer_, "DevGetInfo", [&](auto* type, void* buffer, std::size_t* size) {
            return producer_->api().DevGetInfo(handle_, command, type, buffer, size);
        })});
    return result;
}

std::vector<Property> Device::port_info() {
    std::lock_guard lock(mutex_);
    std::vector<Property> result;
    if (closed_) return result;
    const std::pair<const char*, G::PORT_INFO_CMD> commands[]{
        {"PORT_INFO_ID", G::PORT_INFO_ID}, {"PORT_INFO_VENDOR", G::PORT_INFO_VENDOR},
        {"PORT_INFO_MODEL", G::PORT_INFO_MODEL}, {"PORT_INFO_TLTYPE", G::PORT_INFO_TLTYPE},
        {"PORT_INFO_MODULE", G::PORT_INFO_MODULE}, {"PORT_INFO_LITTLE_ENDIAN", G::PORT_INFO_LITTLE_ENDIAN},
        {"PORT_INFO_BIG_ENDIAN", G::PORT_INFO_BIG_ENDIAN}, {"PORT_INFO_ACCESS_READ", G::PORT_INFO_ACCESS_READ},
        {"PORT_INFO_ACCESS_WRITE", G::PORT_INFO_ACCESS_WRITE}, {"PORT_INFO_ACCESS_NA", G::PORT_INFO_ACCESS_NA},
        {"PORT_INFO_ACCESS_NI", G::PORT_INFO_ACCESS_NI}, {"PORT_INFO_VERSION", G::PORT_INFO_VERSION},
        {"PORT_INFO_PORTNAME", G::PORT_INFO_PORTNAME}};
    for (const auto& [name, command] : commands)
        result.push_back({name, query(*producer_, "GCGetPortInfo", [&](auto* type, void* buffer, std::size_t* size) {
            return producer_->api().GCGetPortInfo(port_, command, type, buffer, size);
        })});
    return result;
}

Result<PortByteOrder> Device::port_byte_order() {
    const auto facts = port_info();
    if (facts.empty()) return refuse(ErrorCode::stopped, "GCGetPortInfo", "device is closed");
    const auto flag = [&](std::string_view name) -> Result<bool> {
        const auto* property = find(facts, name);
        if (!property->text) return property->text.error();
        if (property->text.value() == "true") return true;
        if (property->text.value() == "false") return false;
        return refuse(ErrorCode::io_error, "GCGetPortInfo", std::string(name) + " is not a BOOL8 value");
    };
    auto little = flag("PORT_INFO_LITTLE_ENDIAN");
    auto big = flag("PORT_INFO_BIG_ENDIAN");
    if (!little || !big) {
        auto failure = !little ? little.error() : big.error();
        failure.code = ErrorCode::unsupported;
        failure.detail += " (port byte order is undeclared; it is not assumed)";
        return failure;
    }
    if (little.value() == big.value())
        return refuse(ErrorCode::unsupported, "GCGetPortInfo", "port declares both or neither byte order");
    return little.value() ? PortByteOrder::little_endian : PortByteOrder::big_endian;
}
Result<std::vector<std::vector<Property>>> Device::xml_locations() {
    std::lock_guard lock(mutex_);
    if (auto s = open_locked(); !s) return s.error();
    std::uint32_t count = 0;
    const auto status = producer_->api().GCGetNumPortURLs(port_, &count);
    if (status != G::GC_ERR_SUCCESS) return producer_->failure("GCGetNumPortURLs", status);
    if (count > 64) return refuse(ErrorCode::io_error, "GCGetNumPortURLs", "implausible count " + number(count));
    const std::pair<const char*, G::URL_INFO_CMD> commands[]{
        {"URL_INFO_URL", G::URL_INFO_URL}, {"URL_INFO_SCHEME", G::URL_INFO_SCHEME},
        {"URL_INFO_FILENAME", G::URL_INFO_FILENAME},
        {"URL_INFO_FILE_REGISTER_ADDRESS", G::URL_INFO_FILE_REGISTER_ADDRESS},
        {"URL_INFO_FILE_SIZE", G::URL_INFO_FILE_SIZE}, {"URL_INFO_SCHEMA_VER_MAJOR", G::URL_INFO_SCHEMA_VER_MAJOR},
        {"URL_INFO_SCHEMA_VER_MINOR", G::URL_INFO_SCHEMA_VER_MINOR},
        {"URL_INFO_FILE_SHA1_HASH", G::URL_INFO_FILE_SHA1_HASH}};
    std::vector<std::vector<Property>> result;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::vector<Property> location;
        for (const auto& [name, command] : commands)
            location.push_back({name, query(*producer_, "GCGetPortURLInfo",
                [&](auto* type, void* buffer, std::size_t* size) {
                    return producer_->api().GCGetPortURLInfo(port_, i, command, type, buffer, size);
                })});
        result.push_back(std::move(location));
    }
    return result;
}

Status Device::read_port(std::uint64_t address, std::span<std::byte> destination) {
    std::lock_guard lock(mutex_);
    if (auto s = open_locked(); !s) return s;
    if (destination.empty() || destination.size() > max_port_read)
        return refuse(ErrorCode::invalid_argument, "GCReadPort", "length must be 1.." + number(max_port_read) + " bytes");
    if (address > std::numeric_limits<std::uint64_t>::max() - (destination.size() - 1))
        return refuse(ErrorCode::invalid_argument, "GCReadPort", "address arithmetic overflow");
    std::vector<std::byte> temporary(destination.size());
    std::size_t size = temporary.size();
    const auto status = producer_->api().GCReadPort(port_, address, temporary.data(), &size);
    if (status != G::GC_ERR_SUCCESS) return producer_->failure("GCReadPort", status);
    if (size != temporary.size())
        return refuse(ErrorCode::io_error, "GCReadPort", "transferred " + number(size) + " of " +
                      number(temporary.size()) + " bytes; partial data discarded");
    std::copy(temporary.begin(), temporary.end(), destination.begin());
    return {};
}
// Existing shared validation (CheckedRegisters) and write-completion semantics:
// any failure after GCWritePort is entered is WriteCompletion::uncertain. A
// successful call says nothing about packets, retries, deadlines or acceptance.
namespace reg = backend::device_registers;

class Device::Port final : public reg::CheckedRegisters {
public:
    Port(Device& device, PortByteOrder order) : device_(device), order_(order) {}
private:
    static reg::Failure native(const Failure& failure) {
        return reg::Failure{backend::Error{failure.code, failure.message()}};
    }
    reg::Status connected() const override {
        if (device_.closed_) return backend::Error{ErrorCode::stopped, "GenTL device is closed"};
        return {};
    }
    reg::Status transfer_read(std::uint64_t address, std::span<std::byte> bytes) {
        std::size_t size = bytes.size();
        const auto status = device_.producer_->api().GCReadPort(device_.port_, address, bytes.data(), &size);
        if (status != G::GC_ERR_SUCCESS) return native(device_.producer_->failure("GCReadPort", status));
        if (size != bytes.size())
            return backend::Error{ErrorCode::io_error, "GenTL GCReadPort transferred " + number(size) + " of " +
                                  number(bytes.size()) + " bytes"};
        return {};
    }
    reg::Status transfer_write(std::uint64_t address, std::span<const std::byte> bytes) {
        // The shared final write wrapper marks every failure after validation
        // as uncertain. For a missing symbol that is conservative: the message
        // states that no native call was made.
        if (!device_.producer_->api().GCWritePort)
            return backend::Error{ErrorCode::unsupported,
                                  "GenTL producer does not export GCWritePort; no native write was made"};
        std::size_t size = bytes.size();
        const auto status = device_.producer_->api().GCWritePort(device_.port_, address, bytes.data(), &size);
        if (status != G::GC_ERR_SUCCESS) return native(device_.producer_->failure("GCWritePort", status));
        if (size != bytes.size())
            return backend::Error{ErrorCode::io_error, "GenTL GCWritePort reported " + number(size) + " of " +
                                  number(bytes.size()) + " bytes written"};
        return {};
    }
    std::array<std::byte, 4> encode(std::uint32_t value) const {
        std::array<std::byte, 4> bytes{};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto shift = order_ == PortByteOrder::little_endian ? 8 * i : 8 * (3 - i);
            bytes[i] = static_cast<std::byte>((value >> shift) & 0xffu);
        }
        return bytes;
    }
    std::uint32_t decode(const std::array<std::byte, 4>& bytes) const {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto shift = order_ == PortByteOrder::little_endian ? 8 * i : 8 * (3 - i);
            value |= std::to_integer<std::uint32_t>(bytes[i]) << shift;
        }
        return value;
    }
    reg::Result<std::uint32_t> native_read_register(std::uint64_t address) override {
        std::array<std::byte, 4> bytes{};
        if (auto s = transfer_read(address, bytes); !s) return s.error();
        return decode(bytes);
    }
    reg::Status native_write_register(std::uint64_t address, std::uint32_t value) override {
        const auto bytes = encode(value);
        return transfer_write(address, bytes);
    }
    reg::Status native_read_memory(std::uint64_t address, std::span<std::byte> bytes) override {
        return transfer_read(address, bytes);
    }
    reg::Status native_write_memory(std::uint64_t address, std::span<const std::byte> bytes) override {
        return transfer_write(address, bytes);
    }
    Device& device_;
    const PortByteOrder order_;
};

reg::Status Device::with_registers(const std::function<reg::Status(reg::IDeviceRegisters&)>& callback) {
    if (!callback) return backend::Error{ErrorCode::invalid_argument, "GenTL register callback is empty"};
    auto order = port_byte_order(); // before taking the operation lock (port_info locks)
    std::lock_guard lock(mutex_);
    if (closed_) return backend::Error{ErrorCode::stopped, "GenTL device is closed"};
    if (!order) return backend::Error{order.error().code, order.error().message()};
    Port port(*this, order.value());
    return callback(port);
}
Result<std::vector<std::string>> Device::stream_ids() {
    std::lock_guard lock(mutex_);
    if (auto s = open_locked(); !s) return s.error();
    const auto& api = producer_->api();
    if (!api.DevGetNumDataStreams)
        return refuse(ErrorCode::unsupported, "DevGetNumDataStreams", "producer lacks the acquisition entry points");
    std::uint32_t count = 0;
    const auto status = api.DevGetNumDataStreams(handle_, &count);
    if (status != G::GC_ERR_SUCCESS) return producer_->failure("DevGetNumDataStreams", status);
    if (count > 64) return refuse(ErrorCode::io_error, "DevGetNumDataStreams", "implausible count " + number(count));
    std::vector<std::string> result;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto id = query_id(*producer_, "DevGetDataStreamID", [&](char* buffer, std::size_t* size) {
            return api.DevGetDataStreamID(handle_, i, buffer, size);
        });
        if (!id) return id.error();
        result.push_back(std::move(id).value());
    }
    return result;
}

Result<std::shared_ptr<Stream>> Device::open_stream(std::string_view id, StreamOptions options) {
    auto list = stream_ids();
    if (!list) return list.error();
    if (std::find(list.value().begin(), list.value().end(), id) == list.value().end())
        return refuse(ErrorCode::not_found, "DevOpenDataStream", "stream '" + std::string(id) + "' is not listed");
    if (options.buffers == 0 || options.buffers > 64 || options.max_payload_bytes == 0)
        return refuse(ErrorCode::invalid_argument, "DevOpenDataStream", "buffers must be 1..64 and payload limit nonzero");
    std::lock_guard lock(mutex_);
    if (auto s = open_locked(); !s) return s.error();
    const std::string exact(id);
    void* handle = nullptr;
    const auto status = producer_->api().DevOpenDataStream(handle_, exact.c_str(), &handle);
    if (status != G::GC_ERR_SUCCESS) return producer_->failure("DevOpenDataStream", status);
    if (!handle) return refuse(ErrorCode::io_error, "DevOpenDataStream", "succeeded with a null handle");
    NativeOpen owner(*producer_, handle, "DSClose", producer_->api().DSClose);
    try {
        construction_checkpoint();
        std::shared_ptr<Stream> stream(new Stream(shared_from_this(), nullptr, options),
                                      std::default_delete<Stream>{}, ConstructionAllocator<Stream>{});
        stream->handle_ = handle;
        ++streams_;
        owner.release();
        return stream;
    } catch (...) {
        return owner.abandon(refuse(ErrorCode::resource_exhausted, "DevOpenDataStream", "allocation failed"));
    }
}

// ---- Stream -----------------------------------------------------------------

// Consumer-owned announced memory. Retained (quarantined) by the producer if
// it cannot be revoked, so the producer can never write into freed storage.
struct Stream::Slot {
    std::shared_ptr<std::vector<std::byte>> memory;
    void* buffer{nullptr};
    bool announced{false};
};

bool FrameFacts::mono8() const noexcept {
    // PFNC Mono8 (0x01080001). GigE Vision uses the same value in its namespace.
    return pixel_format == backend::profiles::pfnc_mono8 &&
           (pixel_format_namespace == static_cast<std::uint64_t>(G::PIXELFORMAT_NAMESPACE_PFNC_32BIT) ||
            pixel_format_namespace == static_cast<std::uint64_t>(G::PIXELFORMAT_NAMESPACE_GEV));
}

Stream::Stream(std::shared_ptr<Device> device, void* handle, Device::StreamOptions options)
    : device_(std::move(device)), producer_(device_->producer_), handle_(handle), options_(options) {}

Stream::~Stream() {
    if (!handle_) return;
    (void)close();
    if (!closed_) {
        std::lock_guard lock(device_->mutex_);
        --device_->streams_;
    }
}

std::vector<Property> Stream::info() {
    std::vector<Property> result;
    if (closed_) return result;
    const std::pair<const char*, G::STREAM_INFO_CMD> commands[]{
        {"STREAM_INFO_ID", G::STREAM_INFO_ID}, {"STREAM_INFO_TLTYPE", G::STREAM_INFO_TLTYPE},
        {"STREAM_INFO_PAYLOAD_SIZE", G::STREAM_INFO_PAYLOAD_SIZE},
        {"STREAM_INFO_DEFINES_PAYLOADSIZE", G::STREAM_INFO_DEFINES_PAYLOADSIZE},
        {"STREAM_INFO_BUF_ANNOUNCE_MIN", G::STREAM_INFO_BUF_ANNOUNCE_MIN},
        {"STREAM_INFO_BUF_ALIGNMENT", G::STREAM_INFO_BUF_ALIGNMENT},
        {"STREAM_INFO_NUM_DELIVERED", G::STREAM_INFO_NUM_DELIVERED},
        {"STREAM_INFO_NUM_UNDERRUN", G::STREAM_INFO_NUM_UNDERRUN},
        {"STREAM_INFO_IS_GRABBING", G::STREAM_INFO_IS_GRABBING}};
    for (const auto& [name, command] : commands)
        result.push_back({name, query(*producer_, "DSGetInfo", [&](auto* type, void* buffer, std::size_t* size) {
            return producer_->api().DSGetInfo(handle_, command, type, buffer, size);
        })});
    return result;
}

StreamCounters Stream::counters() const {
    std::lock_guard lock(counters_mutex_);
    return counters_;
}

void Stream::request_stop() noexcept {
    std::lock_guard lock(event_mutex_);
    stop_requested_.store(true);
    if (lifecycle_ == Lifecycle::starting || lifecycle_ == Lifecycle::running)
        lifecycle_ = Lifecycle::stopping;
    if (event_ && !event_retirement_attempted_)
        producer_->api().EventKill(event_); // wakes a blocked EventGetData
}
namespace {
// Fixed-size typed query with declared-datatype and size verification.
template <class T, class Call>
Result<T> typed(const Producer& producer, std::string_view function, G::INFO_DATATYPE expected, Call call) {
    G::INFO_DATATYPE type = G::INFO_DATATYPE_UNKNOWN;
    T value{};
    std::size_t size = sizeof(T);
    const auto status = call(&type, &value, &size);
    if (status != G::GC_ERR_SUCCESS) return producer.failure(function, status);
    if (type != expected || size != sizeof(T))
        return refuse(ErrorCode::io_error, std::string(function), "unexpected datatype " + number(type) +
                      " or size " + number(size));
    return value;
}
} // namespace

Status Stream::start() {
    if (closed_) return refuse(ErrorCode::invalid_state, "DSStartAcquisition", "stream is closed");
    if (running_) return refuse(ErrorCode::invalid_state, "DSStartAcquisition", "stream already started");
    for (const auto& lease : leases_)
        if (lease.use_count() > 1)
            return refuse(ErrorCode::resource_exhausted, "DSStartAcquisition",
                          "a frame lease from an earlier run is still held; release it before restarting");
    if (teardown_failure_)
        return refuse(ErrorCode::invalid_state, "DSStartAcquisition", "earlier teardown failed; close this stream");
    {
        std::lock_guard lock(event_mutex_);
        stop_requested_.store(false);
        lifecycle_ = Lifecycle::starting;
        event_retirement_attempted_ = false;
    }
    teardown_done_ = false;
    const auto abort = [&](Failure failure) -> Status {
        if (auto cleaned = teardown(); !cleaned) failure.detail += "; cleanup: " + cleaned.error().message();
        return failure;
    };
    const auto cancellation = [&] {
        return abort(refuse(ErrorCode::stopped, "DSStartAcquisition", "stop requested during startup"));
    };
    const auto& api = producer_->api();
    const auto info = [&](G::STREAM_INFO_CMD command) {
        return [&, command](auto* type, void* buffer, std::size_t* size) {
            return api.DSGetInfo(handle_, command, type, buffer, size);
        };
    };
    auto defines = typed<::bool8_t>(*producer_, "DSGetInfo(DEFINES_PAYLOADSIZE)", G::INFO_DATATYPE_BOOL8,
                                     info(G::STREAM_INFO_DEFINES_PAYLOADSIZE));
    if (!defines) return abort(defines.error());
    if (!defines.value())
        return abort(refuse(ErrorCode::unsupported, "DSGetInfo", "payload size is defined only by the remote device's "
                      "GenApi PayloadSize feature, which this foundation does not evaluate"));
    auto payload = typed<std::size_t>(*producer_, "DSGetInfo(PAYLOAD_SIZE)", G::INFO_DATATYPE_SIZET,
                                      info(G::STREAM_INFO_PAYLOAD_SIZE));
    if (!payload) return abort(payload.error());
    const auto bytes = payload.value();
    if (bytes == 0 || bytes > options_.max_payload_bytes)
        return abort(refuse(ErrorCode::resource_exhausted, "DSGetInfo", "payload size " + number(bytes) +
                      " is outside 1.." + number(options_.max_payload_bytes)));
    // Announced plus lease memory, bounded before any allocation.
    if (options_.buffers > options_.max_total_bytes / bytes / 2)
        return abort(refuse(ErrorCode::resource_exhausted, "DSAnnounceBuffer", number(options_.buffers) +
                      " buffers of " + number(bytes) + " bytes (x2 for leases) exceed " + number(options_.max_total_bytes)));
    if (stop_requested_.load()) return cancellation();
    try {
        leases_.clear();
        for (std::size_t i = 0; i < options_.buffers; ++i) {
            leases_.push_back(std::make_shared<std::vector<std::byte>>(bytes));
            auto slot = std::make_shared<Slot>();
            slot->memory = std::make_shared<std::vector<std::byte>>(bytes);
            slots_.push_back(slot);
        }
    } catch (const std::bad_alloc&) {
        slots_.clear();
        leases_.clear();
        return abort(refuse(ErrorCode::resource_exhausted, "DSAnnounceBuffer", "host buffer allocation failed"));
    }
    for (auto& slot : slots_) {
        if (stop_requested_.load()) return cancellation();
        const auto status = api.DSAnnounceBuffer(handle_, slot->memory->data(), slot->memory->size(), slot.get(),
                                                 &slot->buffer);
        if (status != G::GC_ERR_SUCCESS) return abort(producer_->failure("DSAnnounceBuffer", status));
        slot->announced = true;
    }
    for (auto& slot : slots_) {
        if (stop_requested_.load()) return cancellation();
        const auto status = api.DSQueueBuffer(handle_, slot->buffer);
        if (status != G::GC_ERR_SUCCESS) return abort(producer_->failure("DSQueueBuffer", status));
    }
    if (stop_requested_.load()) return cancellation();
    void* event = nullptr;
    const auto registered = api.GCRegisterEvent(handle_, G::EVENT_NEW_BUFFER, &event);
    {
        std::lock_guard lock(event_mutex_);
        if (registered == G::GC_ERR_SUCCESS) event_ = event;
    }
    if (registered != G::GC_ERR_SUCCESS) return abort(producer_->failure("GCRegisterEvent(NEW_BUFFER)", registered));
    if (stop_requested_.load()) return cancellation();
    // Even a failed start is followed by DSStopAcquisition in teardown().
    engine_attempted_ = true;
    const auto status = api.DSStartAcquisition(handle_, G::ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE);
    if (status != G::GC_ERR_SUCCESS) return abort(producer_->failure("DSStartAcquisition", status));
    running_ = true;
    {
        std::lock_guard lock(event_mutex_);
        if (!stop_requested_.load()) {
            lifecycle_ = Lifecycle::running;
            return {};
        }
    }
    return cancellation();
}
Result<Frame> Stream::wait(std::chrono::milliseconds timeout) {
    if (!running_) return refuse(ErrorCode::stopped, "EventGetData", "stream is not started");
    if (stop_requested_.load()) return refuse(ErrorCode::stopped, "EventGetData", "stop requested");
    if (timeout.count() < 0 || timeout > std::chrono::hours(24))
        return refuse(ErrorCode::invalid_argument, "EventGetData", "timeout outside 0..24 hours");
    const auto& api = producer_->api();
    G::EVENT_NEW_BUFFER_DATA data{};
    std::size_t size = sizeof(data);
    const auto status = api.EventGetData(event_, &data, &size, static_cast<std::uint64_t>(timeout.count()));
    if (status == G::GC_ERR_TIMEOUT) {
        std::lock_guard lock(counters_mutex_);
        ++counters_.timeouts;
        return producer_->failure("EventGetData", status);
    }
    if (status != G::GC_ERR_SUCCESS) {
        auto failure = producer_->failure("EventGetData", status);
        if (status == G::GC_ERR_ABORT || stop_requested_.load()) failure.code = ErrorCode::stopped;
        return failure;
    }
    // Accept only a handle returned for this stream's own announcement.
    const auto match = std::find_if(slots_.begin(), slots_.end(),
                                    [&](const auto& slot) { return slot->buffer == data.BufferHandle; });
    if (size != sizeof(data) || match == slots_.end() || data.pUserPointer != match->get())
        return refuse(ErrorCode::io_error, "EventGetData", "NEW_BUFFER event names no buffer announced by this stream");
    auto& slot = **match;
    FrameFacts facts = read_facts(slot);
    // Copy at most the filled, in-capacity bytes into a free lease, then requeue
    // the native buffer at once. A held lease is never overwritten.
    std::shared_ptr<std::vector<std::byte>> lease;
    for (auto& candidate : leases_)
        if (candidate.use_count() == 1) { lease = candidate; break; }
    std::size_t copied = 0;
    if (lease && facts.size_filled && *facts.size_filled <= slot.memory->size()) {
        copied = *facts.size_filled;
        std::memcpy(lease->data(), slot.memory->data(), copied);
    }
    const auto requeued = api.DSQueueBuffer(handle_, slot.buffer);
    std::lock_guard lock(counters_mutex_);
    if (requeued != G::GC_ERR_SUCCESS) return producer_->failure("DSQueueBuffer(requeue)", requeued);
    if (!lease) {
        ++counters_.starvation;
        return refuse(ErrorCode::resource_exhausted, "lease pool", "all frame leases are held; frame dropped");
    }
    if (!facts.size_filled || *facts.size_filled > slot.memory->size())
        return refuse(ErrorCode::io_error, "BUFFER_INFO_SIZE_FILLED", "missing or larger than the announced buffer");
    if (facts.incomplete.value_or(true)) ++counters_.incomplete;
    ++counters_.delivered;
    return Frame(std::move(facts), std::span<const std::byte>(lease->data(), copied), lease);
}

FrameFacts Stream::read_facts(const Slot& slot) {
    const auto& api = producer_->api();
    FrameFacts facts;
    facts.capacity = slot.memory->size();
    const auto field = [&](auto& out, G::BUFFER_INFO_CMD command, G::INFO_DATATYPE expected, const char* name) {
        using T = typename std::remove_reference_t<decltype(out)>::value_type;
        auto value = typed<T>(*producer_, name, expected, [&](auto* type, void* buffer, std::size_t* bytes) {
            return api.DSGetBufferInfo(handle_, slot.buffer, command, type, buffer, bytes);
        });
        if (value) out = value.value();
        else facts.notes.push_back(value.error().message());
    };
    std::optional<::bool8_t> incomplete;
    field(incomplete, G::BUFFER_INFO_IS_INCOMPLETE, G::INFO_DATATYPE_BOOL8, "BUFFER_INFO_IS_INCOMPLETE");
    if (incomplete) facts.incomplete = *incomplete != 0;
    field(facts.frame_id, G::BUFFER_INFO_FRAMEID, G::INFO_DATATYPE_UINT64, "BUFFER_INFO_FRAMEID");
    field(facts.size_filled, G::BUFFER_INFO_SIZE_FILLED, G::INFO_DATATYPE_SIZET, "BUFFER_INFO_SIZE_FILLED");
    field(facts.width, G::BUFFER_INFO_WIDTH, G::INFO_DATATYPE_SIZET, "BUFFER_INFO_WIDTH");
    field(facts.height, G::BUFFER_INFO_HEIGHT, G::INFO_DATATYPE_SIZET, "BUFFER_INFO_HEIGHT");
    field(facts.payload_type, G::BUFFER_INFO_PAYLOADTYPE, G::INFO_DATATYPE_SIZET, "BUFFER_INFO_PAYLOADTYPE");
    field(facts.pixel_format, G::BUFFER_INFO_PIXELFORMAT, G::INFO_DATATYPE_UINT64, "BUFFER_INFO_PIXELFORMAT");
    field(facts.pixel_format_namespace, G::BUFFER_INFO_PIXELFORMAT_NAMESPACE, G::INFO_DATATYPE_UINT64,
          "BUFFER_INFO_PIXELFORMAT_NAMESPACE");
    field(facts.timestamp_ticks, G::BUFFER_INFO_TIMESTAMP, G::INFO_DATATYPE_UINT64, "BUFFER_INFO_TIMESTAMP");
    field(facts.timestamp_ns, G::BUFFER_INFO_TIMESTAMP_NS, G::INFO_DATATYPE_UINT64, "BUFFER_INFO_TIMESTAMP_NS");
    return facts;
}
// Each native release is attempted once. Failures retain their ownership state
// until DSClose conclusively releases the stream, or quarantine lasts forever.
Status Stream::teardown() {
    if (teardown_done_) {
        if (teardown_failure_) return *teardown_failure_;
        return {};
    }
    teardown_done_ = true;
    {
        std::lock_guard lock(event_mutex_);
        lifecycle_ = Lifecycle::stopping;
    }
    const auto& api = producer_->api();
    const auto record = [&](std::string_view function, G::GC_ERROR status) {
        if (status == G::GC_ERR_SUCCESS) return;
        auto failure = producer_->failure(function, status);
        if (!teardown_failure_) teardown_failure_ = std::move(failure);
        else teardown_failure_->detail += "; cleanup: " + failure.message();
    };
    if (engine_attempted_) {
        const auto status = api.DSStopAcquisition(handle_, G::ACQ_STOP_FLAGS_KILL);
        bool stopped = status == G::GC_ERR_SUCCESS;
        // A rejected start may have left no acquisition to stop. Only an
        // explicit not-grabbing readback resolves that otherwise uncertain case.
        if (!stopped && !running_) {
            const auto original = producer_->failure("DSStopAcquisition", status);
            auto grabbing = typed<::bool8_t>(*producer_, "DSGetInfo(IS_GRABBING)", G::INFO_DATATYPE_BOOL8,
                [&](auto* type, void* data, auto* size) {
                    return api.DSGetInfo(handle_, G::STREAM_INFO_IS_GRABBING, type, data, size);
                });
            stopped = grabbing && !grabbing.value();
            if (!stopped) teardown_failure_ = original;
        } else record("DSStopAcquisition", status);
        if (stopped) engine_attempted_ = false;
    }
    running_ = false;
    if (!slots_.empty()) record("DSFlushQueue", api.DSFlushQueue(handle_, G::ACQ_QUEUE_ALL_DISCARD));
    for (auto& slot : slots_) {
        if (!slot->announced) continue;
        void* memory = nullptr;
        void* user = nullptr;
        const auto status = api.DSRevokeBuffer(handle_, slot->buffer, &memory, &user);
        if (status == G::GC_ERR_SUCCESS) { slot->announced = false; continue; }
        record("DSRevokeBuffer", status);
        // Retain Slot as well: its address is the announcement's user pointer.
        producer_->quarantine(slot);
    }
    {
        std::lock_guard lock(event_mutex_);
        if (event_ && !event_retirement_attempted_) {
            event_retirement_attempted_ = true;
            const auto status = api.GCUnregisterEvent(handle_, G::EVENT_NEW_BUFFER);
            record("GCUnregisterEvent", status);
            if (status == G::GC_ERR_SUCCESS) event_ = nullptr;
        }
        lifecycle_ = teardown_failure_ ? Lifecycle::stopping : Lifecycle::idle;
    }
    if (!teardown_failure_) slots_.clear();
    if (teardown_failure_) return *teardown_failure_;
    return {};
}

Status Stream::stop() {
    if (closed_) return {};
    request_stop();
    return teardown();
}

Status Stream::close() {
    if (close_failure_) return *close_failure_;
    if (closed_) return {};
    auto stopped = stop();
    const auto status = producer_->api().DSClose(handle_);
    if (status != G::GC_ERR_SUCCESS) {
        auto failure = producer_->failure("DSClose", status);
        if (!stopped) {
            close_failure_ = stopped.error();
            close_failure_->detail += "; cleanup: " + failure.message();
        } else close_failure_ = std::move(failure);
        producer_->taint(); // no retry of an ambiguously closed native handle
        return *close_failure_;
    }
    closed_ = true;
    engine_attempted_ = false;
    for (const auto& slot : slots_) producer_->release_quarantine(slot);
    slots_.clear();
    {
        std::lock_guard lock(event_mutex_);
        event_ = nullptr;
        lifecycle_ = Lifecycle::retired;
    }
    {
        std::lock_guard lock(device_->mutex_);
        --device_->streams_;
    }
    return stopped;
}

} // namespace gige_mel::gentl
