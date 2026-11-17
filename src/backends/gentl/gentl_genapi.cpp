// GIGE-MEL-GENTL-002: GenTL-backed ArvDevice and the Aravis 0.8.36 GenApi
// feature service. Verified against the pinned 0.8.36 source:
//  * ArvDevice is an abstract derivable GObject; ArvDeviceClass has the
//    create_stream/get_genicam_xml/get_genicam/read_memory/write_memory/
//    read_register/write_register virtuals (arvdevice.h).
//  * arv_gc_new(device, xml, size) parses with libxml2 and stores the device
//    as a BORROWED pointer (arvgc.c: priv->device = device; no reference).
//  * ArvGcPort uses read_register/write_register (with a GigE byte swap) only
//    for ARV_IS_GV_DEVICE; every other device gets raw read_memory/
//    write_memory bytes, and ArvGcRegisterNode then applies the register's own
//    <Endianess> exactly once (arvgcport.c, arvgcregisternode.c).
//  * The DOM parser keeps a function-static SAX state (arvdomparser.c), so
//    parsing is serialized process-wide here.
#include "gentl_genapi.hpp"

#include <arv.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

// Plain C instance/class layout; registered once with g_type_register_static_simple.
extern "C" {
struct GigeMelGenTLArvDevice {
    ArvDevice parent;
    ArvGc* genicam;   // sole owner; released first in finalize
    char* xml;        // copy served by get_genicam_xml
    gsize xml_size;
    gpointer binding; // Binding*, set only inside a transaction (atomic access)
};
struct GigeMelGenTLArvDeviceClass {
    ArvDeviceClass parent_class;
};
}

namespace gige_mel::backend::gentl_backend {
namespace {

namespace reg = device_registers;
namespace features = device_features;

Error fail(ErrorCode code, std::string message) { return Error{code, std::move(message)}; }
Error from(const gentl::Failure& failure) { return Error{failure.code, failure.message()}; }
std::string copy(const char* text) { return text ? std::string(text) : std::string(); }

std::atomic<long> live_devices{0}, live_genicams{0}, order_violations{0};

// One feature operation's view of the native port. Lives on the transaction
// thread's stack; the ArvDevice points to it only for that duration.
struct Binding {
    gentl::Device& device;
    bool control_access;
    std::thread::id owner;
    std::optional<Error> native; // first native failure of the current operation

    void record(Error error) { if (!native) native = std::move(error); }
    Status read(std::uint64_t address, std::span<std::byte> bytes) {
        if (auto s = device.read_port(address, bytes); !s) return from(s.error());
        return {};
    }
    Status write(std::uint64_t address, std::span<const std::byte> bytes) {
        // Never escalate: a read-only session performs no native write.
        if (!control_access)
            return fail(ErrorCode::unsupported, "GenTL device was opened read-only; no native write was attempted");
        auto s = device.with_registers([&](reg::IDeviceRegisters& registers) {
            return registers.write_memory(address, bytes);
        });
        if (s) return {};
        const auto& failure = s.error();
        return fail(failure.diagnostic.code,
                    failure.diagnostic.message + (failure.write_completion == reg::WriteCompletion::uncertain
                                                      ? "; write completion uncertain; not retried"
                                                      : "; no native write was attempted"));
    }
};

gpointer device_parent_class = nullptr;

GigeMelGenTLArvDevice* self_of(ArvDevice* device) { return reinterpret_cast<GigeMelGenTLArvDevice*>(device); }

void set_error(GError** error, ArvDeviceError code, const std::string& message) {
    g_set_error_literal(error, ARV_DEVICE_ERROR, code, message.c_str());
}

// The bound transaction for THIS thread, or null (refused, no native call).
Binding* bound(ArvDevice* device, GError** error) {
    auto* binding = static_cast<Binding*>(g_atomic_pointer_get(&self_of(device)->binding));
    if (binding && binding->owner == std::this_thread::get_id()) return binding;
    set_error(error, ARV_DEVICE_ERROR_NOT_CONNECTED,
              "GenTL GenApi register access outside the owning transaction was refused; no native call was made");
    return nullptr;
}

ArvDeviceError arv_code(ErrorCode code) {
    return code == ErrorCode::timeout ? ARV_DEVICE_ERROR_TIMEOUT : ARV_DEVICE_ERROR_TRANSFER_ERROR;
}

// C callbacks: no exception may escape. The exact native failure is kept in
// the binding so the feature layer can return it unchanged.
template <class Body> gboolean guarded(ArvDevice* device, GError** error, Body body) noexcept {
    try {
        auto* binding = bound(device, error);
        if (!binding) return FALSE;
        Status status = body(*binding);
        if (status) return TRUE;
        set_error(error, arv_code(status.error().code), status.error().message);
        binding->record(status.error());
        return FALSE;
    } catch (...) {
        g_set_error_literal(error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_UNKNOWN,
                            "GenTL GenApi port access failed with an unexpected exception");
        return FALSE;
    }
}

gboolean vm_read_memory(ArvDevice* device, guint64 address, guint32 size, void* buffer, GError** error) {
    return guarded(device, error, [&](Binding& b) {
        return b.read(address, std::span(static_cast<std::byte*>(buffer), size));
    });
}

gboolean vm_write_memory(ArvDevice* device, guint64 address, guint32 size, const void* buffer, GError** error) {
    return guarded(device, error, [&](Binding& b) {
        return b.write(address, std::span(static_cast<const std::byte*>(buffer), size));
    });
}

std::string completion(const reg::Failure& failure) {
    return failure.diagnostic.message + (failure.write_completion == reg::WriteCompletion::uncertain
                                             ? "; write completion uncertain; not retried"
                                             : "; no native write was attempted");
}

// Not used by ArvGc for this (non-GigE) device class. A direct caller gets one
// 4-byte word converted once with the port's DECLARED byte order.
gboolean vm_read_register(ArvDevice* device, guint64 address, guint32* value, GError** error) {
    return guarded(device, error, [&](Binding& b) -> Status {
        auto s = b.device.with_registers([&](reg::IDeviceRegisters& registers) -> reg::Status {
            auto word = registers.read_register(address);
            if (!word) return word.error();
            *value = word.value();
            return {};
        });
        if (!s) return s.error().diagnostic;
        return {};
    });
}

gboolean vm_write_register(ArvDevice* device, guint64 address, guint32 value, GError** error) {
    return guarded(device, error, [&](Binding& b) -> Status {
        if (!b.control_access)
            return fail(ErrorCode::unsupported, "GenTL device was opened read-only; no native write was attempted");
        auto s = b.device.with_registers([&](reg::IDeviceRegisters& registers) {
            return registers.write_register(address, value);
        });
        if (s) return {};
        return fail(s.error().diagnostic.code, completion(s.error()));
    });
}

ArvGc* vm_get_genicam(ArvDevice* device) { return self_of(device)->genicam; }

const char* vm_get_genicam_xml(ArvDevice* device, size_t* size) {
    if (size) *size = self_of(device)->xml_size;
    return self_of(device)->xml;
}

ArvStream* vm_create_stream(ArvDevice*, ArvStreamCallback, void*, GDestroyNotify, GError** error) {
    set_error(error, ARV_DEVICE_ERROR_NO_STREAM_CHANNEL,
              "GenTL GenApi device has no Aravis stream; acquisition is out of scope for GIGE-MEL-GENTL-002");
    return nullptr;
}

void genicam_gone(gpointer data, GObject*) {
    static_cast<GigeMelGenTLArvDevice*>(data)->genicam = nullptr;
    --live_genicams;
}

void vm_finalize(GObject* object) {
    auto* self = reinterpret_cast<GigeMelGenTLArvDevice*>(object);
    // Release the ArvGc (which borrows this device) BEFORE this device's own
    // storage is torn down; the weak notify clears self->genicam on success.
    if (auto* genicam = self->genicam) {
        g_object_unref(genicam);
        if (self->genicam) { // retained elsewhere: it would outlive this device
            ++order_violations;
            g_object_weak_unref(G_OBJECT(self->genicam), genicam_gone, self);
            --live_genicams;
            self->genicam = nullptr;
        }
    }
    g_free(self->xml);
    self->xml = nullptr;
    --live_devices;
    G_OBJECT_CLASS(device_parent_class)->finalize(object);
}

void class_init(gpointer klass, gpointer) {
    device_parent_class = g_type_class_peek_parent(klass);
    G_OBJECT_CLASS(klass)->finalize = vm_finalize;
    auto* device = ARV_DEVICE_CLASS(klass);
    device->create_stream = vm_create_stream;
    device->get_genicam_xml = vm_get_genicam_xml;
    device->get_genicam = vm_get_genicam;
    device->read_memory = vm_read_memory;
    device->write_memory = vm_write_memory;
    device->read_register = vm_read_register;
    device->write_register = vm_write_register;
}

GType device_type() {
    static const GType type = g_type_register_static_simple(
        ARV_TYPE_DEVICE, "GigeMelGenTLArvDevice", sizeof(GigeMelGenTLArvDeviceClass), class_init,
        sizeof(GigeMelGenTLArvDevice), nullptr, GTypeFlags(0));
    return type;
}

// Serializes Aravis's non-reentrant DOM parser (function-static SAX state).
std::mutex& parser_mutex() {
    static std::mutex mutex;
    return mutex;
}

struct ErrorSlot {
    GError* raw{nullptr};
    ErrorSlot() = default;
    ErrorSlot(const ErrorSlot&) = delete;
    ErrorSlot& operator=(const ErrorSlot&) = delete;
    ~ErrorSlot() { if (raw) g_error_free(raw); }
    GError** out() noexcept { return &raw; }
    [[nodiscard]] bool failed() const noexcept { return raw != nullptr; }
};


ErrorCode translate_code(const GError& error) {
    if (error.domain == arv_device_error_quark()) {
        switch (error.code) {
        case ARV_DEVICE_ERROR_TIMEOUT: return ErrorCode::timeout;
        case ARV_DEVICE_ERROR_NOT_CONNECTED: return ErrorCode::disconnected;
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

// IDeviceFeatures over the session's ArvGc, borrowed for one transaction.
// Same node semantics as the native Aravis adapter (AravisDeviceFeatures), so
// one profile sees identical facts. A GenTL port failure beneath an Aravis
// GError is returned with its exact GenTL code and text; never retried.
class GenTLFeatures final : public features::IDeviceFeatures {
public:
    GenTLFeatures(ArvGc* genicam, Binding& binding) : genicam_(genicam), binding_(binding) {}

    Result<bool> implemented(std::string_view name) override {
        begin();
        const std::string feature(name);
        ArvGcNode* node = arv_gc_get_node(genicam_, feature.c_str());
        if (!node) return false;
        if (!ARV_IS_GC_FEATURE_NODE(node))
            return fail(ErrorCode::unsupported, "optional feature '" + feature + "' exists but is not a GenICam feature node");
        ErrorSlot error;
        const bool implemented = arv_gc_feature_node_is_implemented(ARV_GC_FEATURE_NODE(node), error.out());
        if (error.failed()) return translate("query implementation of optional feature", name, *error.raw);
        return implemented;
    }

    Result<std::int64_t> read_integer(std::string_view name) override {
        auto node = feature_node(name, false);
        if (!node) return node.error();
        if (!ARV_IS_GC_INTEGER(node.value()))
            return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not an integer feature");
        ErrorSlot error;
        const gint64 value = arv_gc_integer_get_value(ARV_GC_INTEGER(node.value()), error.out());
        if (error.failed()) return translate("read", name, *error.raw);
        return static_cast<std::int64_t>(value);
    }

    Result<features::IntegerFeature> read_integer_feature(std::string_view name, features::ReadAccess access) override {
        auto node = feature_node(name, access == features::ReadAccess::read_write);
        if (!node) return node.error();
        if (!ARV_IS_GC_INTEGER(node.value()))
            return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not an integer feature");
        auto* integer = ARV_GC_INTEGER(node.value());
        features::IntegerFeature f;
        ErrorSlot error;
        f.value = arv_gc_integer_get_value(integer, error.out());
        if (!error.failed()) f.minimum = arv_gc_integer_get_min(integer, error.out());
        if (!error.failed()) f.maximum = arv_gc_integer_get_max(integer, error.out());
        if (!error.failed()) f.increment = arv_gc_integer_get_inc(integer, error.out());
        if (error.failed()) return translate("read value/bounds/increment of", name, *error.raw);
        return f;
    }

    Result<features::FloatFeature> read_float_feature(std::string_view name, features::ReadAccess access) override {
        auto node = feature_node(name, access == features::ReadAccess::read_write);
        if (!node) return node.error();
        if (!ARV_IS_GC_FLOAT(node.value()))
            return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not a float feature");
        auto* real = ARV_GC_FLOAT(node.value());
        features::FloatFeature f;
        ErrorSlot error;
        f.value = arv_gc_float_get_value(real, error.out());
        if (!error.failed()) f.minimum = arv_gc_float_get_min(real, error.out());
        if (!error.failed()) f.maximum = arv_gc_float_get_max(real, error.out());
        double increment = 0;
        if (!error.failed()) increment = arv_gc_float_get_inc(real, error.out());
        if (error.failed()) return translate("read value/bounds of", name, *error.raw);
        // Aravis 0.8.36 returns exactly G_MINDOUBLE for "no <Inc> defined";
        // only that value is absence. No increment is ever invented.
        if (increment != G_MINDOUBLE) f.increment = increment;
        if (const char* unit = arv_gc_float_get_unit(real); unit && *unit) f.unit = unit;
        return f;
    }

    Result<std::string> read_string_or_enum(std::string_view name) override {
        auto node = feature_node(name, false);
        if (!node) return node.error();
        ErrorSlot error;
        const char* value = nullptr;
        if (ARV_IS_GC_ENUMERATION(node.value()))
            value = arv_gc_enumeration_get_string_value(ARV_GC_ENUMERATION(node.value()), error.out());
        else if (ARV_IS_GC_STRING(node.value()))
            value = arv_gc_string_get_value(ARV_GC_STRING(node.value()), error.out());
        else
            return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not an enumeration or string");
        if (error.failed()) return translate("read", name, *error.raw);
        if (!value) return fail(ErrorCode::io_error, "GenTL GenApi read of '" + std::string(name) + "' returned no value");
        return copy(value); // copied before any later GenApi call can invalidate it
    }

    Result<bool> read_boolean(std::string_view name) override {
        auto node = feature_node(name, false);
        if (!node) return node.error();
        if (!ARV_IS_GC_BOOLEAN(node.value()))
            return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not a Boolean feature");
        ErrorSlot error;
        const bool value = arv_gc_boolean_get_value(ARV_GC_BOOLEAN(node.value()), error.out());
        if (error.failed()) return translate("read", name, *error.raw);
        return value;
    }

    Result<bool> enum_entry_available(std::string_view name, std::string_view entry) override {
        auto node = feature_node(name, false);
        if (!node) return node.error();
        if (!ARV_IS_GC_ENUMERATION(node.value()))
            return fail(ErrorCode::unsupported, "feature '" + std::string(name) + "' is not an enumeration");
        ErrorSlot error;
        guint count = 0;
        const char** entries =
            arv_gc_enumeration_dup_available_string_values(ARV_GC_ENUMERATION(node.value()), &count, error.out());
        if (error.failed()) {
            g_free(entries);
            return translate("query available entries of", name, *error.raw);
        }
        bool available = false;
        for (guint i = 0; i < count && !available; ++i) available = entries[i] && entry == entries[i];
        g_free(entries);
        return available;
    }

    // Exactly one native feature write; Aravis re-evaluates range and access
    // at the write (per-session ENABLE policies). No retry, no rollback.
    Status write_integer(std::string_view name, std::int64_t value) override {
        auto node = feature_node(name, true);
        if (!node) return node.error();
        if (!ARV_IS_GC_INTEGER(node.value()))
            return fail(ErrorCode::unsupported, std::string(name) + " is not an integer feature");
        ErrorSlot error;
        arv_gc_integer_set_value(ARV_GC_INTEGER(node.value()), value, error.out());
        if (error.failed()) return translate("write", name, *error.raw);
        return {};
    }

    Status write_float(std::string_view name, double value) override {
        auto node = feature_node(name, true);
        if (!node) return node.error();
        if (!ARV_IS_GC_FLOAT(node.value()))
            return fail(ErrorCode::unsupported, std::string(name) + " is not a float feature");
        ErrorSlot error;
        arv_gc_float_set_value(ARV_GC_FLOAT(node.value()), value, error.out());
        if (error.failed()) return translate("write", name, *error.raw);
        return {};
    }

private:
    void begin() { binding_.native.reset(); }

    Error translate(std::string_view operation, std::string_view feature, const GError& error) {
        std::string message = "GenTL GenApi " + std::string(operation) + " '" + std::string(feature) + "': ";
        if (binding_.native) // exact port failure: keep its code and GenTL text
            return fail(binding_.native->code, message + binding_.native->message);
        message += copy(error.message) + " [" + copy(g_quark_to_string(error.domain)) + " " +
                   std::to_string(error.code) + "]";
        return fail(translate_code(error), std::move(message));
    }

    // Absent, unimplemented, unavailable, inaccessible and locked features are
    // distinct diagnostics; each query failure keeps its native provenance.
    Result<ArvGcFeatureNode*> feature_node(std::string_view name, bool writable) {
        begin();
        const std::string feature(name);
        ArvGcNode* node = arv_gc_get_node(genicam_, feature.c_str());
        if (!node || !ARV_IS_GC_FEATURE_NODE(node))
            return fail(ErrorCode::unsupported, "required feature '" + feature + "' is not present");
        auto* feature_node = ARV_GC_FEATURE_NODE(node);
        ErrorSlot error;
        const bool implemented = arv_gc_feature_node_is_implemented(feature_node, error.out());
        if (error.failed()) return translate("query implementation of", name, *error.raw);
        if (!implemented) return fail(ErrorCode::unsupported, "required feature '" + feature + "' is not implemented");
        const bool available = arv_gc_feature_node_is_available(feature_node, error.out());
        if (error.failed()) return translate("query availability of", name, *error.raw);
        if (!available) return fail(ErrorCode::unsupported, "required feature '" + feature + "' is not available");
        const auto mode = arv_gc_feature_node_get_actual_access_mode(feature_node);
        const bool readable = mode == ARV_GC_ACCESS_MODE_RO || mode == ARV_GC_ACCESS_MODE_RW;
        if (!readable || (writable && mode != ARV_GC_ACCESS_MODE_RW))
            return fail(ErrorCode::unsupported, "feature '" + feature + "' has access mode " +
                                                    copy(arv_gc_access_mode_to_string(mode)) +
                                                    (writable ? "; read/write access is required" : "; read access is required"));
        if (writable) {
            const bool locked = arv_gc_feature_node_is_locked(feature_node, error.out());
            if (error.failed()) return translate("query lock of", name, *error.raw);
            if (locked) return fail(ErrorCode::unsupported, "feature '" + feature + "' is locked; write access is unavailable");
        }
        return feature_node;
    }

    ArvGc* const genicam_;
    Binding& binding_;
};


// ---- GenICam XML selection and validation ----------------------------------

bool starts_with_ci(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) return false;
    return true;
}

bool ends_with_ci(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) return false;
    return starts_with_ci(text.substr(text.size() - suffix.size()), suffix);
}

// Raw retrieved bytes -> exact uncompressed XML text, or an explicit refusal.
// Only trailing NUL padding is removed (device files are often padded to a
// register boundary). Embedded NULs, a ZIP signature or non-XML content are
// refused; nothing is repaired or guessed.
Result<std::string> classify(std::string bytes) {
    if (bytes.size() >= 4 && bytes.compare(0, 4, "PK\x03\x04", 4) == 0)
        return fail(ErrorCode::unsupported, "device XML is a ZIP archive; compressed GenICam XML is unsupported in "
                                            "GIGE-MEL-GENTL-002 (see design: bounded ZIP support)");
    while (!bytes.empty() && bytes.back() == '\0') bytes.pop_back();
    if (bytes.empty()) return fail(ErrorCode::io_error, "device XML is empty (only NUL padding)");
    if (bytes.find('\0') != std::string::npos)
        return fail(ErrorCode::io_error, "device XML contains an embedded NUL byte; refused rather than truncated");
    std::size_t start = 0;
    if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) start = 3; // UTF-8 BOM
    while (start < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[start]))) ++start;
    if (start >= bytes.size() || bytes[start] != '<')
        return fail(ErrorCode::io_error, "device XML does not begin with an XML declaration or element");
    return bytes;
}

struct Location {
    std::uint32_t index;
    std::string url;
    gentl::LocalUrl local;
};

const std::string* text_of(const std::vector<gentl::Property>& location, std::string_view name) {
    const auto* property = gentl::find(location, name);
    return property && property->text ? &property->text.value() : nullptr;
}

// Lowest-index Local location that is an explicitly supported, consistent,
// uncompressed register-map file. Every other location is recorded with its
// reason; there is no File:/http: retrieval and no fallback to invented XML.
Result<Location> select_location(const std::vector<std::vector<gentl::Property>>& locations, XmlLimits limits,
                                 std::string& skipped) {
    const auto skip = [&](std::uint32_t index, const std::string& url, const std::string& why) {
        skipped += (skipped.empty() ? "" : "; ") + std::string("[") + std::to_string(index) + "] '" + url + "': " + why;
    };
    for (std::uint32_t i = 0; i < locations.size(); ++i) {
        const auto* url = text_of(locations[i], "URL_INFO_URL");
        if (!url) { skip(i, "", "URL_INFO_URL query failed"); continue; }
        if (!starts_with_ci(*url, "local:")) {
            skip(i, *url, starts_with_ci(*url, "file:") ? "File: scheme is unsupported (no host file access)"
                          : starts_with_ci(*url, "http") ? "http: scheme is unsupported (no network download)"
                                                         : "unsupported URL scheme");
            continue;
        }
        if (const auto* scheme = text_of(locations[i], "URL_INFO_SCHEME"); scheme && *scheme != "0") {
            skip(i, *url, "URL_INFO_SCHEME " + *scheme + " contradicts the Local: URL");
            continue;
        }
        auto local = gentl::parse_local_url(*url);
        if (!local) { skip(i, *url, "malformed Local: URL (filename;hex-address;hex-length required)"); continue; }
        const auto path = std::string_view(local->filename);
        const auto file = path.substr(0, path.find('?'));
        if (ends_with_ci(file, ".zip")) { skip(i, *url, "compressed ZIP GenICam XML is unsupported"); continue; }
        if (!ends_with_ci(file, ".xml")) { skip(i, *url, "filename is not an uncompressed .xml file"); continue; }
        if (local->length > limits.max_bytes) {
            skip(i, *url, "declared length " + std::to_string(local->length) + " exceeds the limit " +
                              std::to_string(limits.max_bytes));
            continue;
        }
        if (local->address > reg::maximum_address || local->length - 1 > reg::maximum_address - local->address) {
            skip(i, *url, "address range exceeds the 32-bit device register window");
            continue;
        }
        // URL_INFO_FILE_REGISTER_ADDRESS / FILE_SIZE, when supplied, must agree.
        if (const auto* address = text_of(locations[i], "URL_INFO_FILE_REGISTER_ADDRESS");
            address && *address != std::to_string(local->address)) {
            skip(i, *url, "URL_INFO_FILE_REGISTER_ADDRESS " + *address + " contradicts the URL");
            continue;
        }
        if (const auto* size = text_of(locations[i], "URL_INFO_FILE_SIZE");
            size && *size != std::to_string(local->length)) {
            skip(i, *url, "URL_INFO_FILE_SIZE " + *size + " contradicts the URL");
            continue;
        }
        return Location{i, *url, *local};
    }
    return fail(ErrorCode::unsupported, "no supported uncompressed Local: GenICam XML location among " +
                                            std::to_string(locations.size()) + " location(s)" +
                                            (skipped.empty() ? std::string() : ": " + skipped));
}


// Independent well-formedness check. Demonstrated limitation: with libxml2
// < 2.11 (Debian 2.9.14 here) Aravis 0.8.36 parses through
// xmlSAXUserParseMemory and treats only NEGATIVE results as errors, so a
// truncated document returns a partial ArvGc (validation record). libxml2 is
// already Aravis's own parser dependency. No network, no DTD loading, no
// entity substitution; the root must be a GenICam RegisterDescription.
Status well_formed(const std::string& xml) {
    xmlDocPtr document = xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "device.xml", nullptr,
                                       XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!document) {
        const xmlError* error = xmlGetLastError();
        std::string detail = error && error->message ? error->message : "unknown libxml2 error";
        while (!detail.empty() && detail.back() == '\n') detail.pop_back();
        if (error) detail += " (line " + std::to_string(error->line) + ")";
        xmlResetLastError();
        return fail(ErrorCode::io_error, "not well-formed XML: " + detail + "; no features are fabricated");
    }
    const xmlNode* root = xmlDocGetRootElement(document);
    const bool genicam = root && root->name && std::strcmp(reinterpret_cast<const char*>(root->name), "RegisterDescription") == 0;
    xmlFreeDoc(document);
    if (!genicam) return fail(ErrorCode::io_error, "root element is not a GenICam RegisterDescription");
    return {};
}

// Retrieves the complete declared length in bounded GCReadPort chunks; any
// failure or short transfer discards everything (no partial document).
Result<std::string> retrieve(gentl::Device& device, const gentl::LocalUrl& local) {
    constexpr std::uint64_t chunk = 64 * 1024;
    std::string bytes;
    try {
        bytes.resize(static_cast<std::size_t>(local.length));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "cannot allocate " + std::to_string(local.length) + " bytes for device XML");
    }
    for (std::uint64_t offset = 0; offset < local.length; offset += chunk) {
        const auto size = std::min(chunk, local.length - offset);
        auto span = std::span(reinterpret_cast<std::byte*>(bytes.data()) + offset, static_cast<std::size_t>(size));
        if (auto s = device.read_port(local.address + offset, span); !s)
            return fail(s.error().code, "device XML retrieval at offset " + std::to_string(offset) + ": " +
                                            s.error().message() + "; partial data discarded");
    }
    return bytes;
}

// The producer may publish a SHA-1 (20-byte BUFFER, rendered by the consumer
// as "buffer[20] <hex>"). Present => verified over the exact declared bytes;
// absent or failed query => not verified (recorded), never assumed.
Result<bool> verify_sha1(const std::vector<gentl::Property>& location, const std::string& bytes) {
    const auto* digest = text_of(location, "URL_INFO_FILE_SHA1_HASH");
    if (!digest) return false;
    constexpr std::string_view prefix = "buffer[20] ";
    if (digest->rfind(prefix, 0) != 0 || digest->size() != prefix.size() + 40)
        return fail(ErrorCode::io_error, "URL_INFO_FILE_SHA1_HASH is not a 20-byte buffer: " + *digest);
    GChecksum* checksum = g_checksum_new(G_CHECKSUM_SHA1);
    if (!checksum) return fail(ErrorCode::resource_exhausted, "cannot create a SHA-1 checksum");
    g_checksum_update(checksum, reinterpret_cast<const guchar*>(bytes.data()), static_cast<gssize>(bytes.size()));
    const std::string actual = g_checksum_get_string(checksum);
    g_checksum_free(checksum);
    if (actual != digest->substr(prefix.size()))
        return fail(ErrorCode::io_error, "device XML SHA-1 " + actual + " does not match the producer's " +
                                             digest->substr(prefix.size()));
    return true;
}

// Owned GObject reference released on every early return unless committed.
struct DeviceRef {
    GigeMelGenTLArvDevice* device{nullptr};
    DeviceRef() = default;
    DeviceRef(const DeviceRef&) = delete;
    DeviceRef& operator=(const DeviceRef&) = delete;
    ~DeviceRef() { if (device) g_object_unref(device); }
};

} // namespace

struct GenApiSession::Native {
    GigeMelGenTLArvDevice* device{nullptr}; // sole reference; owns genicam
    Native() = default;
    Native(const Native&) = delete;
    Native& operator=(const Native&) = delete;
    ~Native() { if (device) g_object_unref(device); }
};

GenApiObjectCounts genapi_object_counts() noexcept {
    return {live_devices.load(), live_genicams.load(), order_violations.load()};
}

Result<std::unique_ptr<GenApiSession>> GenApiSession::open(std::shared_ptr<gentl::Device> device, gentl::Access access,
                                                           XmlLimits limits) try {
    if (!device) return fail(ErrorCode::invalid_argument, "GenApi requires an opened GenTL device");
    if (limits.max_bytes == 0 || limits.max_bytes > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        return fail(ErrorCode::invalid_argument, "XML limit must be 1..INT_MAX bytes (Aravis parser bound)");
    // The shared register transport requires a declared port byte order; an
    // undeclared or contradictory declaration refuses GenApi entirely.
    if (auto order = device->port_byte_order(); !order) return from(order.error());
    auto locations = device->xml_locations();
    if (!locations) return from(locations.error());
    std::unique_ptr<GenApiSession> session(new GenApiSession());
    auto selected = select_location(locations.value(), limits, session->xml_.skipped);
    if (!selected) return selected.error();
    const auto& chosen = selected.value();
    auto bytes = retrieve(*device, chosen.local);
    if (!bytes) return bytes.error();
    auto verified = verify_sha1(locations.value()[chosen.index], bytes.value());
    if (!verified) return verified.error();
    auto text = classify(std::move(bytes).value());
    if (!text) return fail(text.error().code, "device XML '" + chosen.url + "': " + text.error().message);
    {
        std::lock_guard parse(parser_mutex()); // libxml2 global error state
        if (auto ok = well_formed(text.value()); !ok)
            return fail(ok.error().code, "device XML '" + chosen.url + "' is " + ok.error().message);
    }
    session->xml_.url = chosen.url;
    session->xml_.location_index = chosen.index;
    session->xml_.address = chosen.local.address;
    session->xml_.length = chosen.local.length;
    session->xml_.parsed_length = text.value().size();
    session->xml_.sha1_verified = verified.value();

    DeviceRef owner;
    owner.device = static_cast<GigeMelGenTLArvDevice*>(g_object_new(device_type(), nullptr));
    if (!owner.device) return fail(ErrorCode::resource_exhausted, "cannot create the GenTL GenApi device");
    ++live_devices;
    const auto& xml = text.value();
    owner.device->xml = static_cast<char*>(g_malloc(xml.size() + 1));
    std::memcpy(owner.device->xml, xml.data(), xml.size());
    owner.device->xml[xml.size()] = '\0';
    owner.device->xml_size = xml.size();
    ArvGc* genicam = nullptr;
    {
        std::lock_guard parse(parser_mutex());
        genicam = arv_gc_new(ARV_DEVICE(owner.device), owner.device->xml, owner.device->xml_size);
    }
    if (!genicam || !ARV_IS_GC(genicam))
        return fail(ErrorCode::io_error, "device XML '" + chosen.url + "' is not a well-formed GenICam document "
                                         "(Aravis 0.8.36 arv_gc_new failed); no features are fabricated");
    owner.device->genicam = genicam;
    ++live_genicams;
    g_object_weak_ref(G_OBJECT(genicam), genicam_gone, owner.device);
    auto* root = arv_dom_document_get_document_element(ARV_DOM_DOCUMENT(genicam));
    if (!root || !ARV_IS_GC_REGISTER_DESCRIPTION_NODE(root))
        return fail(ErrorCode::io_error, "device XML root element is not a GenICam RegisterDescription");
    auto* description = ARV_GC_REGISTER_DESCRIPTION_NODE(root);
    session->xml_.schema = std::to_string(arv_gc_register_description_node_get_schema_major_version(description)) +
                           "." + std::to_string(arv_gc_register_description_node_get_schema_minor_version(description)) +
                           "." + std::to_string(arv_gc_register_description_node_get_schema_subminor_version(description));
    session->xml_.legacy_schema = arv_gc_register_description_node_compare_schema_version(description, 1, 1, 0) < 0;
    // Per-session client policy: uncached reads (no permanent mutable facts);
    // Aravis rechecks range and access at each write.
    arv_gc_set_register_cache_policy(genicam, ARV_REGISTER_CACHE_POLICY_DISABLE);
    arv_gc_set_range_check_policy(genicam, ARV_RANGE_CHECK_POLICY_ENABLE);
    arv_gc_set_access_check_policy(genicam, ARV_ACCESS_CHECK_POLICY_ENABLE);
    session->native_ = std::make_unique<Native>();
    session->native_->device = std::exchange(owner.device, nullptr);
    session->device_ = std::move(device);
    session->control_ = access != gentl::Access::read_only;
    return session;
} catch (const std::bad_alloc&) {
    return fail(ErrorCode::resource_exhausted, "GenApi session allocation failed");
}

GenApiSession::~GenApiSession() { (void)release(); }

std::shared_ptr<gentl::Device> GenApiSession::release() noexcept {
    // A same-thread release from inside a transaction would destroy the
    // ArvGc under Aravis's own stack frames: refuse (keep everything alive).
    if (owner_.load() == std::this_thread::get_id()) return nullptr;
    std::lock_guard lock(mutex_);
    native_.reset(); // ArvGc first (in the device finalizer), then ArvDevice
    return std::move(device_);
}

Status GenApiSession::transaction(const Transaction& callback) {
    if (!callback) return fail(ErrorCode::invalid_argument, "GenApi transaction callback is empty");
    if (owner_.load() == std::this_thread::get_id())
        return fail(ErrorCode::invalid_state, "nested GenApi transaction refused (would self-deadlock)");
    std::lock_guard lock(mutex_);
    if (!native_ || !native_->device || !native_->device->genicam || !device_)
        return fail(ErrorCode::stopped, "GenTL GenApi session is closed; no stale node-map access is permitted");
    struct Scope {
        GenApiSession& session;
        GigeMelGenTLArvDevice* device;
        ~Scope() {
            g_atomic_pointer_set(&device->binding, nullptr);
            session.owner_.store(std::thread::id{});
        }
    };
    Binding binding{*device_, control_, std::this_thread::get_id(), std::nullopt};
    owner_.store(binding.owner);
    g_atomic_pointer_set(&native_->device->binding, &binding);
    Scope scope{*this, native_->device};
    GenTLFeatures features(native_->device->genicam, binding);
    try {
        return callback(features);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "GenApi transaction allocation failed");
    }
}

#ifdef GIGE_MEL_GENTL_TESTING
// Models a stray Aravis call (e.g. from another thread) that holds NO project
// lock: the caller guarantees the session is not released concurrently.
Status unbound_read_for_testing(GenApiSession& session) {
    if (!session.native_ || !session.native_->device) return fail(ErrorCode::stopped, "session closed");
    ArvDevice* device = ARV_DEVICE(session.native_->device);
    g_object_ref(device);
    std::array<std::byte, 4> word{};
    ErrorSlot error;
    const bool ok = arv_device_read_memory(device, 0, 4, word.data(), error.out());
    g_object_unref(device);
    if (ok) return {};
    return fail(translate_code(*error.raw), copy(error.raw->message));
}

Result<std::string> classify_xml_for_testing(std::string bytes) { return classify(std::move(bytes)); }
#endif

} // namespace gige_mel::backend::gentl_backend

