// GIGE-MEL-GENTL-002 offline contract: the internal GenTL backend, Aravis
// 0.8.36 GenApi over GCReadPort/GCWritePort and the UNCHANGED camera
// profiles, against the controlled fixture producer and a SIMULATED register
// model. No physical camera, network or vendor SDK is used. Fixture success is
// not evidence for any real producer or for the physical Cognex CIC-5000R.
#include "gentl_backend.hpp"
#include "gentl_genapi.hpp"
#include "profiles/camera_profile.hpp"
#include "profiles/profile_module.hpp"
#include "selection/profile_loader.hpp"
#include "support/gentl_genicam_fixture.hpp"

#include <glib.h>
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace b = gige_mel::backend;
namespace g = gige_mel::backend::gentl_backend;
namespace p = gige_mel::backend::profiles;
namespace f = gige_mel::backend::device_features;
namespace fx = gentl_fixture;
using b::ErrorCode;
using namespace std::chrono_literals;

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__ ":") + std::to_string(__LINE__) + ": " #x); } while (false)

namespace {

const std::string device_a = g::compose_device_id("mock-if-0", "mock-dev-A");

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

template <class T> T take(b::Result<T> result) {
    if (!result) throw std::runtime_error("unexpected error: " + result.error().message);
    return std::move(result).value();
}
template <class T> T take_gentl(gige_mel::gentl::Result<T> result) {
    if (!result) throw std::runtime_error("unexpected GenTL error: " + result.error().message());
    return std::move(result).value();
}

void expect(const b::Status& status, ErrorCode code, std::string_view text) {
    if (status) throw std::runtime_error("expected failure containing '" + std::string(text) + "'");
    if (status.error().code != code || !contains(status.error().message, text))
        throw std::runtime_error("unexpected failure [" + std::to_string(static_cast<int>(status.error().code)) +
                                 "]: " + status.error().message + " (expected '" + std::string(text) + "')");
}
template <class T> void expect(const b::Result<T>& result, ErrorCode code, std::string_view text) {
    if (result) throw std::runtime_error("expected failure containing '" + std::string(text) + "'");
    expect(b::Status(result.error()), code, text);
}

// Per-test log and environment.
class Fixture {
public:
    explicit Fixture(std::vector<std::pair<std::string, std::string>> env = {}) {
        char name[] = "/tmp/gentl-backend-log-XXXXXX";
        const int fd = mkstemp(name);
        CHECK(fd >= 0);
        ::close(fd);
        log_ = name;
        setenv("GENTL_MOCK_LOG", log_.c_str(), 1);
        for (const char* key : {"GENTL_MOCK_FAIL", "GENTL_MOCK_PORT_ENDIAN"}) unsetenv(key);
        for (const auto& [key, value] : env) setenv(key.c_str(), value.c_str(), 1);
    }
    ~Fixture() { std::remove(log_.c_str()); }
    std::vector<std::string> calls() const {
        std::ifstream file(log_);
        std::vector<std::string> lines;
        for (std::string line; std::getline(file, line);) lines.push_back(line);
        return lines;
    }
    std::size_t count(std::string_view prefix) const {
        std::size_t n = 0;
        for (const auto& line : calls()) if (line.rfind(prefix, 0) == 0) ++n;
        return n;
    }
    std::size_t index(std::string_view prefix) const {
        const auto lines = calls();
        for (std::size_t i = 0; i < lines.size(); ++i) if (lines[i].rfind(prefix, 0) == 0) return i;
        return std::string::npos;
    }
    void clear() const { std::ofstream(log_, std::ios::trunc); }
private:
    std::string log_;
};

bool mapped(const char* path) {
    void* handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return false;
    dlclose(handle);
    return true;
}

// Mock producer controls (resolved only while it is mapped, after GCInitLib).
struct Mock {
    void* library = dlopen(GENTL_MOCK, RTLD_NOW | RTLD_NOLOAD);
    Mock() { CHECK(library); }
    Mock(const Mock&) = delete;
    Mock& operator=(const Mock&) = delete;
    ~Mock() { if (library) dlclose(library); }
    template <class T> T symbol(const char* name) {
        auto value = reinterpret_cast<T>(dlsym(library, name));
        CHECK(value);
        return value;
    }
    void port(const fx::Model& m, const std::string& document) {
        auto image = fx::port(m, document);
        CHECK(symbol<int (*)(const unsigned char*, std::size_t, std::size_t, std::size_t)>("MockSetPort")(
                  image.data(), image.size(), fx::xml_address, document.size()) == 0);
    }
    void model(const fx::Model& m) { port(m, fx::xml(m)); }
    void urls(std::vector<const char*> list) {
        static const char* const none[1] = {nullptr}; // non-null: an explicit, empty override
        symbol<void (*)(const char* const*, std::size_t)>("MockSetUrls")(list.empty() ? none : list.data(), list.size());
    }
    void sha1(const std::vector<unsigned char>& digest) {
        symbol<void (*)(const unsigned char*, std::size_t)>("MockSetSha1")(digest.data(), digest.size());
    }
    void fail_read(std::uint64_t address, int code) {
        symbol<void (*)(std::uint64_t, int)>("MockFailReadAt")(address, code);
    }
    void fail(const char* list) { symbol<void (*)(const char*)>("MockSetFailure")(list); }
    std::vector<unsigned char> peek(std::size_t address, std::size_t size) {
        std::vector<unsigned char> out(size);
        CHECK(symbol<int (*)(std::size_t, unsigned char*, std::size_t)>("MockPeekPort")(address, out.data(), size) == 0);
        return out;
    }
    void poke(std::size_t address, std::vector<unsigned char> bytes) {
        CHECK(symbol<int (*)(std::size_t, const unsigned char*, std::size_t)>("MockPokePort")(
                  address, bytes.data(), bytes.size()) == 0);
    }
};

std::vector<unsigned char> be32(std::uint32_t v) {
    return {static_cast<unsigned char>(v >> 24), static_cast<unsigned char>(v >> 16),
            static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v)};
}
std::vector<unsigned char> le32(std::uint32_t v) {
    return {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16),
            static_cast<unsigned char>(v >> 24)};
}

// Cognex module, loaded once per process through the existing loader.
const gige_mel::selection::LoadedProfile& cognex() {
    static const auto loaded = [] {
        auto result = gige_mel::selection::load_profile_module(GIGE_MEL_COGNEX_MODULE);
        if (!result) throw std::runtime_error("Cognex module: " + result.error().message);
        return std::move(result).value();
    }();
    return loaded;
}
std::shared_ptr<const p::ICameraProfile> cognex_profile() { return take(cognex().create()); }

struct Session {
    std::shared_ptr<b::ICameraBackend> backend;
    std::vector<b::DeviceInfo> devices;
};

// Explicit enumerate() loads the producer (GCInitLib resets the fixture); the
// simulated model is then installed before the explicit open().
Session prepare(std::shared_ptr<const p::ICameraProfile> profile, g::DeviceAccess access = g::DeviceAccess::read_only,
                std::uint64_t max_xml = 16u << 20) {
    Session s;
    s.backend = g::make_backend(g::Options{GENTL_MOCK, access, 100ms, max_xml}, std::move(profile));
    s.devices = take(s.backend->enumerate());
    return s;
}


// ---- Construction, identifiers, discovery, exact selection -----------------

void construction_performs_no_io() {
    Fixture fixture;
    {
        auto backend = g::make_backend(g::Options{GENTL_MOCK}, p::make_mono_free_running_profile());
        CHECK(backend->id() == "gentl");
        CHECK(!mapped(GENTL_MOCK));          // producer not loaded by construction
        CHECK(fixture.calls().empty());      // no producer entry point at all
    }
    bool threw = false;
    try { (void)g::make_backend(g::Options{GENTL_MOCK}, nullptr); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw); // no implicit default profile
    CHECK(!mapped(GENTL_MOCK));
}

void composite_identifiers() {
    for (const auto& [iface, device] : std::vector<std::pair<std::string, std::string>>{
             {"mock-if-0", "mock-dev-A"}, {"a:b", ":c"}, {"1:2", "3"}, {std::string("x\0y", 3), "/z/"},
             {"gentl1:5:abc", "def"}}) {
        const auto id = g::compose_device_id(iface, device);
        auto parsed = take(g::parse_device_id(id));
        CHECK(parsed.interface_id == iface && parsed.device_id == device);
    }
    CHECK(g::compose_device_id("ab", "cd") != g::compose_device_id("a", "bcd")); // unambiguous
    for (std::string bad : {"", "mock-dev-A", "gentl1:", "gentl1:x:ab", "gentl1:09:mock-if-0mock-dev-A",
                            "gentl1:0:abc", "gentl1:3:abc", "gentl1:-1:abc", "gentl2:1:ab", "GENTL1:1:ab"})
        expect(g::parse_device_id(bad), ErrorCode::invalid_argument, "composite");
}

void enumerate_and_exact_selection() {
    Fixture fixture;
    auto backend = g::make_backend(g::Options{GENTL_MOCK}, p::make_mono_free_running_profile());
    auto devices = take(backend->enumerate());
    CHECK(devices.size() == 2);
    CHECK(devices[0].backend_id == "gentl" && devices[0].device_id == device_a);
    CHECK(devices[1].device_id == g::compose_device_id("mock-if-0", "mock-dev-B"));
    CHECK(contains(devices[0].display_name, "SN-A") && contains(devices[0].display_name, "mock-if-0"));
    CHECK(fixture.count("IFOpenDevice") == 0); // discovery never opens a device
    fixture.clear();
    // No serial, prefix, case-folded or raw-ID selection; nothing is opened.
    for (const std::string& id : {std::string("mock-dev-A"), std::string("SN-A"),
                                 g::compose_device_id("mock-if-0", "mock-dev"),
                                 g::compose_device_id("mock-if-0", "MOCK-DEV-A"),
                                 g::compose_device_id("mock-if-1", "mock-dev-A")}) {
        auto opened = backend->open(id);
        CHECK(!opened);
        CHECK(opened.error().code == ErrorCode::invalid_argument || opened.error().code == ErrorCode::not_found);
    }
    CHECK(fixture.count("IFOpenDevice") == 0);
}

// The UNCHANGED default profile, through real GenApi register reads.
void mono_default_profile() {
    Fixture fixture;
    auto s = prepare(p::make_mono_free_running_profile());
    Mock mock;
    fx::Model m;
    m.rate_feature = "AcquisitionFrameRate";
    m.exposure_feature = "ExposureTime";
    m.vendor_name = "SIMULATED";
    m.w = 640; m.h = 480; m.x = 16; m.y = 8;
    m.rate_hz = 25; m.exposure_us = 10000;
    mock.model(m);
    auto device = take(s.backend->open(device_a));
    CHECK(contains(fixture.calls()[fixture.index("IFOpenDevice")], "access=2")); // DEVICE_ACCESS_READONLY (2)
    const auto caps = device->capabilities();
    CHECK(caps.sensor_width == 2592 && caps.sensor_height == 1944);
    CHECK(caps.width_increment == 8 && caps.height_increment == 2 && caps.offset_x_increment == 8);
    CHECK(caps.minimum_frame_rate_hz == 1 && caps.maximum_frame_rate_hz == 1000000);
    CHECK(caps.minimum_exposure == 35us && caps.pixel_format == b::PixelFormat::mono8 && !caps.external_trigger);
    const auto config = take(device->configuration());
    CHECK(config == (b::CameraConfiguration{640, 480, 16, 8, 25, 10ms}));
    const auto info = g::inspect(*device);
    CHECK(info && info->xml_url == "Local:mock.xml;10000;" + [&] {
        char t[32]; std::snprintf(t, sizeof t, "%zx", fx::xml(m).size()); return std::string(t); }());
    CHECK(info->schema == "1.1.0" && !info->xml_sha1_verified);
    CHECK(fixture.count("GCWritePort") == 0); // read-only open and readback
    CHECK(device->close());
    CHECK(device->close()); // idempotent
    CHECK(fixture.count("DevClose mock-dev-A") == 1);
}


// ---- The UNCHANGED Cognex CIC-5000R profile (SIMULATED fixture) ------------

b::Result<std::shared_ptr<b::ICameraDevice>> open_cognex(const fx::Model& m, Fixture* fixture = nullptr) {
    auto s = prepare(cognex_profile());
    Mock mock;
    mock.model(m);
    if (fixture) fixture->clear();
    return s.backend->open(device_a);
}

void cognex_matching_identity() {
    Fixture fixture;
    for (bool big : {true, false}) {
        fx::Model m;
        m.big_endian = big; // both register byte orders, each converted exactly once
        auto device = take(open_cognex(m, &fixture));
        CHECK(fixture.count("GCWritePort") == 0);
        const auto caps = device->capabilities();
        CHECK(caps.maximum_frame_rate_hz == 10 && caps.minimum_frame_rate_hz == 1); // Cognex policy envelope
        CHECK(caps.sensor_width == 2592 && caps.sensor_height == 1944 && caps.width_increment == 8);
        const auto config = take(device->configuration());
        CHECK(config == (b::CameraConfiguration{2592, 1944, 0, 0, 10, 35ms}));
        auto info = g::inspect(*device);
        CHECK(info);
        bool identity = false;
        for (const auto& [k, v] : info->profile_diagnostics) identity |= k == "DeviceModelName" && v == "CIC-5000R";
        CHECK(identity);
        CHECK(device->close());
    }
}

void cognex_identity_mismatch() {
    Fixture fixture;
    const std::pair<std::string fx::Model::*, const char*> fields[] = {
        {&fx::Model::vendor_name, "DeviceVendorName"}, {&fx::Model::model_name, "DeviceModelName"},
        {&fx::Model::firmware_version, "DeviceFirmwareVersion"}};
    for (const auto& [field, name] : fields) {
        fx::Model m;
        m.*field += "X";
        auto opened = open_cognex(m, &fixture);
        expect(opened, ErrorCode::unsupported, name);
        CHECK(fixture.count("GCWritePort") == 0);
        CHECK(fixture.count("DevClose mock-dev-A") == 1); // partial open cleaned up once
        // Identity precedes configuration discovery: no sensor register read.
        CHECK(fixture.count("GCReadPort " + std::to_string(fx::sensor_width) + " ") == 0);
    }
}

void cognex_required_features_and_types() {
    Fixture fixture;
    struct Case { const char* name; std::function<void(fx::Model&)> change; const char* text; };
    const Case cases[] = {
        {"missing TriggerMode", [](fx::Model& m) { m.include_trigger_mode = false; }, "TriggerMode"},
        {"missing AcquisitionMode", [](fx::Model& m) { m.include_acquisition_mode = false; }, "AcquisitionMode"},
        {"missing SensorWidth", [](fx::Model& m) { m.include_sensor_width = false; }, "SensorWidth"},
        {"wrong rate name", [](fx::Model& m) { m.rate_feature = "AcquisitionFrameRate"; }, "AcquisitionFrameRateAbs"},
        {"rate enable typed Integer", [](fx::Model& m) { m.rate_enable_is_boolean = false; }, "not a Boolean"},
        {"rate disabled", [](fx::Model& m) { m.rate_enabled = false; }, "AcquisitionFrameRateEnable"},
        {"trigger on", [](fx::Model& m) { m.trigger = "On"; }, "TriggerMode"},
        {"single frame", [](fx::Model& m) { m.mode = "SingleFrame"; }, "AcquisitionMode"},
    };
    for (const auto& c : cases) {
        fx::Model m;
        c.change(m);
        auto opened = open_cognex(m, &fixture);
        if (opened || opened.error().code != ErrorCode::unsupported || !contains(opened.error().message, c.text))
            throw std::runtime_error(std::string("case '") + c.name + "': " +
                                     (opened ? std::string("opened") : opened.error().message));
        CHECK(fixture.count("GCWritePort") == 0);
        CHECK(fixture.count("DevClose mock-dev-A") == 1);
    }
}


std::vector<unsigned char> be64(double value) {
    std::uint64_t raw;
    std::memcpy(&raw, &value, 8);
    std::vector<unsigned char> bytes(8);
    for (std::size_t i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(raw >> (8 * (7 - i)));
    return bytes;
}

// Native bounds/increments preserved; readback never clamped to policy.
void cognex_bounds_and_readback() {
    Fixture fixture;
    fx::Model m;
    m.w = 1024; m.x = 64; m.h = 768; m.y = 2; m.rate_hz = 16; m.exposure_us = 12345.0005;
    auto device = take(open_cognex(m));
    std::optional<f::IntegerFeature> width;
    std::optional<f::FloatFeature> rate, exposure;
    CHECK(g::with_features_for_testing(*device, [&](f::IDeviceFeatures& features) -> b::Status {
        width = take(features.read_integer_feature("Width", f::ReadAccess::read_write));
        rate = take(features.read_float_feature("AcquisitionFrameRateAbs", f::ReadAccess::read_write));
        exposure = take(features.read_float_feature("ExposureTimeAbs", f::ReadAccess::readable));
        return {};
    }));
    CHECK(width->value == 1024 && width->minimum == 64 && width->maximum == 2592 - 64 && width->increment == 8);
    CHECK(rate->value == 16 && rate->minimum == 0.0149 && rate->maximum == 1000000 && !rate->increment);
    CHECK(rate->unit == std::optional<std::string>("Hz") && exposure->unit == std::optional<std::string>("us"));
    // A sub-nanosecond native exposure is reported, not rounded.
    expect(device->configuration(), ErrorCode::unsupported, "ExposureTimeAbs reads back 12345.0005");
    Mock mock;
    mock.poke(fx::exposure, be64(20000)); // native change; readback is fresh (no cache)
    const auto config = take(device->configuration());
    CHECK(config.frame_rate_hz == 16 && config.exposure == 20ms && config.width == 1024 && config.offset_x == 64);
    CHECK(device->close());
}

// Register byte order: the same logical Width reads identically from a
// big- and a little-endian register image, with exactly one conversion.
void register_endianness() {
    Fixture fixture;
    for (bool big : {true, false}) {
        fx::Model m;
        m.big_endian = big;
        m.w = 0x0500;
        auto device = take(open_cognex(m));
        Mock mock;
        const auto raw = mock.peek(fx::width, 4);
        CHECK(raw == (big ? be32(0x0500) : le32(0x0500)));
        CHECK(take(device->configuration()).width == 0x0500);
        // Bytes in the opposite order decode as exactly 0x00050000: one
        // conversion by the register's declared <Endianess>, no second swap,
        // and the readback is reported unclamped.
        mock.poke(fx::width, big ? le32(0x0500) : be32(0x0500));
        CHECK(take(device->configuration()).width == 0x00050000);
        CHECK(device->close());
    }
}

// Native port failures are returned with exact GenTL provenance; no value
// is fabricated and no read is retried.
void native_read_failure_propagation() {
    Fixture fixture;
    auto device = take(open_cognex(fx::Model{}));
    Mock mock;
    mock.fail_read(fx::rate, -1011); // GC_ERR_TIMEOUT on the rate register
    fixture.clear();
    auto failed = device->configuration();
    expect(failed, ErrorCode::timeout, "GC_ERR_TIMEOUT");
    CHECK(contains(failed.error().message, "GCReadPort") && contains(failed.error().message, "AcquisitionFrameRateAbs"));
    CHECK(contains(failed.error().message, "injected register read failure")); // GCGetLastError text
    CHECK(fixture.count("fail GCReadPort " + std::to_string(fx::rate)) == 1); // not retried
    mock.fail_read(fx::vendor, -1010);
    mock.fail_read(0, 0);
    CHECK(device->configuration()); // recovered: nothing cached from the failure
    CHECK(device->close());
}


// ---- GenICam XML selection and validation ----------------------------------

std::string hex(std::size_t v) { char t[32]; std::snprintf(t, sizeof t, "%zx", v); return t; }

// Opens with a custom port setup; returns the open result (device closed).
b::Status open_with(const std::function<void(Mock&)>& setup, std::uint64_t max_xml = 16u << 20) {
    auto s = prepare(cognex_profile(), g::DeviceAccess::read_only, max_xml);
    {
        Mock mock;
        mock.model(fx::Model{});
        setup(mock);
    }
    auto opened = s.backend->open(device_a);
    if (!opened) return opened.error();
    return opened.value()->close();
}

void xml_location_handling() {
    Fixture fixture;
    const auto size = fx::xml(fx::Model{}).size();
    const auto good = "Local:device.xml;" + hex(fx::xml_address) + ";" + hex(size);
    // Unsupported schemes are skipped with reasons; the valid Local one wins.
    CHECK(open_with([&](Mock& m) { m.urls({"File:///tmp/device.xml", "http://camera/device.xml", good.c_str()}); }));
    expect(open_with([](Mock& m) { m.urls({"File:///tmp/device.xml"}); }), ErrorCode::unsupported, "File: scheme");
    expect(open_with([](Mock& m) { m.urls({"http://camera/x.xml"}); }), ErrorCode::unsupported, "http: scheme");
    expect(open_with([](Mock& m) { m.urls({}); }), ErrorCode::unsupported, "among 0 location(s)");
    const auto zip = "Local:device.zip;" + hex(fx::xml_address) + ";" + hex(size);
    expect(open_with([&](Mock& m) { m.urls({zip.c_str()}); }), ErrorCode::unsupported, "ZIP");
    for (const char* bad : {"Local:device.xml;zz;10", "Local:device.xml;10", "Local:;10;20", "Local:device.xml;10;0"})
        expect(open_with([&](Mock& m) { m.urls({bad}); }), ErrorCode::unsupported, "malformed Local: URL");
    expect(open_with([](Mock& m) { m.urls({"Local:device.xml;fffffffe;10"}); }), ErrorCode::unsupported,
           "32-bit device register window");
    expect(open_with([](Mock& m) { m.urls({"Local:device.txt;10000;10"}); }), ErrorCode::unsupported, "not an uncompressed");
    // Size limit is enforced before any allocation or port read.
    fixture.clear();
    expect(open_with([](Mock&) {}, 1024), ErrorCode::unsupported, "exceeds the limit 1024");
    CHECK(fixture.count("GCReadPort " + std::to_string(fx::xml_address)) == 0);
    // Range outside the port: exact GenTL error, no partial document.
    const auto beyond = "Local:device.xml;3fff0;20000";
    expect(open_with([&](Mock& m) { m.urls({beyond}); }), ErrorCode::invalid_argument, "GC_ERR_INVALID_ADDRESS");
}

void xml_content_validation() {
    Fixture fixture;
    // A ZIP signature behind a .xml name is still refused, not parsed.
    expect(open_with([](Mock& m) { m.poke(fx::xml_address, {'P', 'K', 3, 4}); }), ErrorCode::unsupported, "ZIP archive");
    expect(open_with([](Mock& m) { m.poke(fx::xml_address + 40, {0}); }), ErrorCode::io_error, "embedded NUL");
    expect(open_with([](Mock& m) { m.poke(fx::xml_address, {'n', 'o', 't', 'x'}); }), ErrorCode::io_error,
           "does not begin with");
    // Truncated/malformed XML: Aravis rejects it; no fallback features.
    expect(open_with([](Mock& m) {
               const std::string doc = fx::xml(fx::Model{}).substr(0, 600);
               m.port(fx::Model{}, doc);
           }), ErrorCode::io_error, "not well-formed XML");
    expect(open_with([](Mock& m) {
               const std::string doc = "<?xml version=\"1.0\"?><Other><x/></Other>";
               m.port(fx::Model{}, doc);
           }), ErrorCode::io_error, "");
    // Trailing NUL padding beyond the document is accepted (only padding removed).
    CHECK(open_with([](Mock& m) {
        const auto doc = fx::xml(fx::Model{});
        const auto url = "Local:device.xml;" + hex(fx::xml_address) + ";" + hex(doc.size() + 64);
        const char* list[] = {url.c_str()};
        m.urls({list[0]});
    }));
    // Producer SHA-1: a mismatch is refused; the correct digest is verified.
    expect(open_with([](Mock& m) { m.sha1(std::vector<unsigned char>(20, 0xab)); }), ErrorCode::io_error,
           "SHA-1");
    auto s = prepare(cognex_profile());
    {
        Mock mock;
        mock.model(fx::Model{});
        const auto doc = fx::xml(fx::Model{});
        GChecksum* sum = g_checksum_new(G_CHECKSUM_SHA1);
        g_checksum_update(sum, reinterpret_cast<const guchar*>(doc.data()), static_cast<gssize>(doc.size()));
        std::vector<unsigned char> digest(20);
        gsize length = digest.size();
        g_checksum_get_digest(sum, digest.data(), &length);
        g_checksum_free(sum);
        mock.sha1(digest);
    }
    auto device = take(s.backend->open(device_a));
    CHECK(g::inspect(*device)->xml_sha1_verified);
    CHECK(device->close());
}

void undeclared_port_byte_order() {
    for (const char* order : {"none", "both"}) {
        Fixture fixture({{"GENTL_MOCK_PORT_ENDIAN", order}});
        expect(open_with([](Mock&) {}), ErrorCode::unsupported, "byte order");
        CHECK(fixture.count("GCReadPort") == 0);   // refused before any XML/register read
        CHECK(fixture.count("DevClose mock-dev-A") == 1);
    }
}


// ---- Device contract, access mode and offline write semantics --------------

void unsupported_operations_and_readonly() {
    Fixture fixture;
    auto device = take(open_cognex(fx::Model{}));
    fixture.clear();
    const auto before = take(device->configuration());
    auto request = before;
    request.frame_rate_hz = 5;
    expect(device->apply(request), ErrorCode::unsupported, "read-only");
    expect(device->create_stream(), ErrorCode::unsupported, "streaming is out of scope");
    CHECK(fixture.count("GCWritePort") == 0 && fixture.count("DSOpen") == 0 && fixture.count("DevOpenDataStream") == 0);
    CHECK(take(device->configuration()) == before);
    // A read-only session never escalates: even a direct feature write is refused
    // before GCWritePort.
    auto written = g::with_features_for_testing(*device, [](f::IDeviceFeatures& features) {
        return features.write_float("AcquisitionFrameRateAbs", 5);
    });
    expect(written, ErrorCode::unsupported, "opened read-only");
    CHECK(fixture.count("GCWritePort") == 0);
    CHECK(device->close());
    // Stale access after close: no node-map access, no native I/O.
    fixture.clear();
    expect(device->configuration(), ErrorCode::stopped, "closed");
    expect(g::with_features_for_testing(*device, [](f::IDeviceFeatures&) -> b::Status { return {}; }),
           ErrorCode::stopped, "closed");
    CHECK(fixture.calls().empty());
    CHECK(g::genapi_object_counts().devices == 0 && g::genapi_object_counts().genicams == 0);
}

// IDeviceFeatures write semantics against the controlled fixture only, with an
// explicit control-access test session. The public apply() path stays disabled.
void offline_feature_writes() {
    Fixture fixture;
    auto s = prepare(cognex_profile(), g::DeviceAccess::control);
    {
        Mock mock;
        mock.model(fx::Model{});
    }
    auto device = take(s.backend->open(device_a));
    CHECK(contains(fixture.calls()[fixture.index("IFOpenDevice")], "access=3")); // DEVICE_ACCESS_CONTROL (3), as configured
    expect(device->apply(b::CameraConfiguration{}), ErrorCode::unsupported, "read-only");
    Mock mock;
    fixture.clear();
    CHECK(g::with_features_for_testing(*device, [](f::IDeviceFeatures& features) -> b::Status {
        // Ordered coupled ROI writes against live native maxima.
        if (auto r = features.write_integer("Width", 1024); !r) return r;
        if (auto r = features.write_integer("OffsetX", 1568); !r) return r; // 2592-1024
        auto width = features.read_integer_feature("Width", f::ReadAccess::read_write);
        if (!width || width.value().maximum != 2592 - 1568) return b::Error{ErrorCode::io_error, "coupled max"};
        if (auto r = features.write_float("AcquisitionFrameRateAbs", 5); !r) return r;
        return {};
    }));
    CHECK(mock.peek(fx::width, 4) == be32(1024) && mock.peek(fx::offset_x, 4) == be32(1568));
    CHECK(mock.peek(fx::rate, 4) == be32(200000)); // 5 Hz -> 200000 us period, converted and written once
    fixture.clear();
    // Write-time constraints: native maxima, increments and access are rechecked; nothing is clamped.
    auto checks = g::with_features_for_testing(*device, [](f::IDeviceFeatures& features) -> b::Status {
        auto over = features.write_integer("Width", 2592); // exceeds live max 1024
        if (over || over.error().code != ErrorCode::invalid_argument) return b::Error{ErrorCode::io_error, "max"};
        auto ro = features.write_integer("SensorWidth", 100);
        if (ro || ro.error().code != ErrorCode::unsupported) return b::Error{ErrorCode::io_error, "access"};
        auto typed = features.write_integer("AcquisitionFrameRateAbs", 5);
        if (typed || !contains(typed.error().message, "not an integer")) return b::Error{ErrorCode::io_error, "type"};
        auto absent = features.write_float("NoSuchFeature", 1);
        if (absent || !contains(absent.error().message, "not present")) return b::Error{ErrorCode::io_error, "absent"};
        return {};
    });
    CHECK(checks);
    CHECK(fixture.count("GCWritePort") == 0); // every refusal happened before native I/O
    // An uncertain native write is reported with its exact code and never retried.
    mock.fail("GCWritePort=-1011");
    auto uncertain = g::with_features_for_testing(*device, [](f::IDeviceFeatures& features) {
        return features.write_float("AcquisitionFrameRateAbs", 8);
    });
    expect(uncertain, ErrorCode::timeout, "write completion uncertain; not retried");
    CHECK(fixture.count("GCWritePort") == 1);
    CHECK(mock.peek(fx::rate, 4) == be32(125000)); // fixture applied it before failing: truly uncertain
    mock.fail("");
    CHECK(device->close());
}


// ---- Ownership, lifetime, concurrency --------------------------------------

void ownership_and_teardown_order() {
    Fixture fixture;
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto s = prepare(cognex_profile());
        {
            Mock mock;
            mock.model(fx::Model{});
        }
        auto device = take(s.backend->open(device_a));
        CHECK(g::genapi_object_counts().devices == 1 && g::genapi_object_counts().genicams == 1);
        // A second logical open of the same device is busy (no native call).
        fixture.clear();
        expect(s.backend->open(device_a), ErrorCode::busy, "already open");
        CHECK(fixture.count("IFOpenDevice") == 0);
        // The producer stays mapped while the device exists even after the
        // backend itself is gone (children own their parents).
        s.backend.reset();
        CHECK(mapped(GENTL_MOCK));
        CHECK(take(device->configuration()).width == 2592);
        fixture.clear();
        CHECK(device->close());
        CHECK(g::genapi_object_counts().devices == 0 && g::genapi_object_counts().genicams == 0);
        CHECK(g::genapi_object_counts().order_violations == 0);
        CHECK(fixture.index("DevClose mock-dev-A") < fixture.index("IFClose"));
        CHECK(fixture.index("IFClose") < fixture.index("TLClose"));
        device.reset(); // last owner: GCCloseLib, then the library unloads
        CHECK(fixture.count("GCCloseLib") == 1 && !mapped(GENTL_MOCK));
    }
    CHECK(gige_mel::gentl::retained_producers() == 0);
}

// The ArvDevice port methods are refused outside the owning transaction
// thread: no native call, no nested device lock.
void unbound_port_access_refused() {
    namespace gc = gige_mel::gentl;
    Fixture fixture;
    auto producer = take_gentl(gc::Producer::load(GENTL_MOCK));
    {
        Mock mock;
        mock.model(fx::Model{});
    }
    auto system = take_gentl(producer->open_system());
    auto iface = take_gentl(system->open_interface("mock-if-0"));
    CHECK(iface->update_devices(100ms));
    auto device = take_gentl(iface->open_device("mock-dev-A", gc::Access::read_only));
    auto session = take(g::GenApiSession::open(device, gc::Access::read_only));
    CHECK(session->xml().url.rfind("Local:mock.xml;", 0) == 0 && session->xml().schema == "1.1.0");
    fixture.clear();
    expect(g::unbound_read_for_testing(*session), ErrorCode::disconnected, "outside the owning transaction");
    CHECK(fixture.count("GCReadPort") == 0);
    // From another thread while a transaction is active: refused, no deadlock.
    b::Status other;
    CHECK(session->transaction([&](f::IDeviceFeatures& features) -> b::Status {
        std::thread([&] { other = g::unbound_read_for_testing(*session); }).join();
        return features.read_integer("SensorWidth") ? b::Status{} : b::Status(b::Error{ErrorCode::io_error, "read"});
    }));
    expect(other, ErrorCode::disconnected, "outside the owning transaction");
    expect(session->transaction([&](f::IDeviceFeatures&) { return session->transaction([](auto&) { return b::Status{}; }); }),
           ErrorCode::invalid_state, "nested");
    // Release: ArvGc before ArvDevice, then the consumer device is returned.
    auto released = session->release();
    CHECK(released == device && g::genapi_object_counts().genicams == 0 && g::genapi_object_counts().devices == 0);
    expect(session->transaction([](auto&) { return b::Status{}; }), ErrorCode::stopped, "closed");
    CHECK(device->close() && iface->close() && system->close() && producer->close());
}

// Complete profile transactions are serialized; concurrent readers never
// interleave inside one snapshot and never deadlock.
void concurrent_transactions() {
    Fixture fixture;
    auto device = take(open_cognex(fx::Model{}));
    std::atomic<int> inside{0}, overlap{0}, ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 25; ++i) {
                auto s = g::with_features_for_testing(*device, [&](f::IDeviceFeatures& features) -> b::Status {
                    if (inside.fetch_add(1) != 0) ++overlap;
                    auto w = features.read_integer("SensorWidth");
                    inside.fetch_sub(1);
                    if (!w) return w.error();
                    return {};
                });
                if (s) ++ok;
                if (device->configuration()) ++ok;
            }
        });
    for (auto& t : threads) t.join();
    CHECK(overlap == 0 && ok == 200);
    // Nested transaction from the same thread: refused, not deadlocked.
    auto nested = g::with_features_for_testing(*device, [&](f::IDeviceFeatures&) -> b::Status {
        auto inner = device->configuration(); // would re-lock the device mutex
        if (inner || inner.error().code != ErrorCode::invalid_state) return b::Error{ErrorCode::io_error, "reentry"};
        auto closed = device->close();       // never tear down under the active transaction
        if (closed || closed.error().code != ErrorCode::invalid_state) return b::Error{ErrorCode::io_error, "close"};
        return {};
    });
    CHECK(nested);
    CHECK(take(device->configuration()).width == 2592); // still open and consistent
    CHECK(device->close());
}


// The same unchanged module declares Aravis and Pleora compatibility and is
// injected here into the GenTL backend with no profile change (no new module bit).
void profile_portability() {
    const auto& loaded = cognex();
    CHECK(loaded.profile->id() == "cognex_cic_5000r_v1");
    CHECK((loaded.compatible_backends & p::module::aravis) && (loaded.compatible_backends & p::module::pleora));
    CHECK((loaded.compatible_backends & ~(p::module::aravis | p::module::pleora)) == 0);
}

// ---- GIGE-MEL-GENTL-002-R1 controls (conclusive cleanup: no quarantine) -----

// Repeated enumeration with a conclusive teardown each time. A discovery error
// whose cleanup succeeds is reported as-is, without quarantine, and the
// producer stays usable.
void enumeration_cleanup_control() {
    Fixture fixture;
    auto backend = g::make_backend(g::Options{GENTL_MOCK, g::DeviceAccess::read_only, 100ms},
                                   p::make_mono_free_running_profile());
    for (int cycle = 0; cycle < 3; ++cycle) {
        fixture.clear();
        auto devices = take(backend->enumerate());
        CHECK(devices.size() == 2 && devices[0].device_id == device_a);
        CHECK(devices[1].device_id == g::compose_device_id("mock-if-0", "mock-dev-B"));
        CHECK(fixture.count("attempt IFClose") == 1 && fixture.count("IFClose") == 1);
        CHECK(fixture.count("attempt TLClose") == 1 && fixture.count("TLClose") == 1);
        CHECK(fixture.index("IFClose") < fixture.index("TLClose"));
    }
    {
        Mock mock;
        mock.fail("IFUpdateDeviceList");
    }
    fixture.clear();
    auto failed = backend->enumerate();
    expect(failed, ErrorCode::io_error, "IFUpdateDeviceList");
    CHECK(!contains(failed.error().message, "cleanup") && !contains(failed.error().message, "retained"));
    CHECK(fixture.count("IFClose") == 1 && fixture.count("TLClose") == 1);
    {
        Mock mock;
        mock.fail("");
    }
    CHECK(take(backend->enumerate()).size() == 2);
    CHECK(g::quarantined_native_sessions() == 0);
    fixture.clear();
    backend.reset();
    CHECK(fixture.count("GCCloseLib") == 1 && !mapped(GENTL_MOCK)); // conclusive: unloaded
}

// The registry reservation: busy while open, released by a conclusive close
// (explicit or by the destructor). It never blocks an unrelated identity.
void ownership_reservation() {
    Fixture fixture;
    const auto device_b = g::compose_device_id("mock-if-0", "mock-dev-B");
    auto s = prepare(cognex_profile());
    {
        Mock mock;
        mock.model(fx::Model{});
    }
    auto first = take(s.backend->open(device_a));
    fixture.clear();
    expect(s.backend->open(device_a), ErrorCode::busy, "already open");
    CHECK(fixture.calls().empty()); // refused before any native call
    // Device B is not blocked by the registry. The fixture's single TL refuses
    // it natively, and that refusal is conclusive, so nothing is quarantined.
    expect(s.backend->open(device_b), ErrorCode::busy, "TLOpen");
    CHECK(fixture.count("IFOpenDevice") == 0 && g::quarantined_native_sessions() == 0);
    CHECK(first->close());
    first.reset();
    for (int cycle = 0; cycle < 3; ++cycle) {
        fixture.clear();
        auto again = take(s.backend->open(device_a));
        CHECK(fixture.count("IFOpenDevice mock-dev-A") == 1);
        if (cycle == 0) CHECK(again->close());
    } // otherwise destroyed open: the destructor's conclusive close releases it
    auto other = take(s.backend->open(device_b));
    CHECK(other->close());
    CHECK(g::quarantined_native_sessions() == 0);
}

// R1-3: deterministic allocation failure at every point before the transfer.
void allocation_failure_recovery() {
    using A = g::AllocationPoint;
    Fixture fixture;
    auto s = prepare(cognex_profile());
    {
        Mock mock;
        mock.model(fx::Model{});
    }
    // Before any native call: nothing to clean up and nothing reserved.
    for (auto point : {A::native_chain, A::retirement_node, A::registry_entry}) {
        fixture.clear();
        CHECK(g::fail_allocation_for_testing(point) == A::none);
        expect(s.backend->open(device_a), ErrorCode::resource_exhausted, "allocation failed");
        CHECK(fixture.calls().empty());
        CHECK(g::fail_allocation_for_testing(A::none) == A::none); // fired exactly once
    }
    for (auto point : {A::native_chain, A::retirement_node}) {
        fixture.clear();
        g::fail_allocation_for_testing(point);
        expect(s.backend->enumerate(), ErrorCode::resource_exhausted, "allocation failed");
        CHECK(fixture.calls().empty());
    }
    // After the native open, before the transfer: the original owner cleans
    // up exactly once and conclusively, so there is no quarantine.
    for (auto point : {A::inspection, A::device_object}) {
        fixture.clear();
        g::fail_allocation_for_testing(point);
        auto opened = s.backend->open(device_a);
        expect(opened, ErrorCode::resource_exhausted, "allocation failed");
        CHECK(!contains(opened.error().message, "cleanup"));
        CHECK(fixture.count("IFOpenDevice") == 1 && fixture.count("attempt DevClose") == 1);
        CHECK(fixture.count("DevClose mock-dev-A") == 1);
        CHECK(fixture.count("attempt IFClose") == 1 && fixture.count("attempt TLClose") == 1);
        CHECK(g::genapi_object_counts().devices == 0 && g::genapi_object_counts().genicams == 0);
        CHECK(g::fail_allocation_for_testing(A::none) == A::none);
    }
    CHECK(g::quarantined_native_sessions() == 0);
    // No reservation leaked. Normal allocation and destruction (control).
    fixture.clear();
    auto device = take(s.backend->open(device_a));
    CHECK(take(device->configuration()).width == 2592);
    CHECK(device->close());
    device.reset();
    CHECK(fixture.count("attempt DevClose") == 1); // exactly one close, none from the destructor
    CHECK(take(s.backend->enumerate()).size() == 2);
}

// ---- Failure injection (one process per case; see CMake) -------------------

b::Result<std::shared_ptr<b::ICameraDevice>> open_failing(const std::function<void(Mock&)>& inject, Fixture& fixture) {
    auto s = prepare(cognex_profile());
    {
        Mock mock;
        mock.model(fx::Model{});
        inject(mock);
    }
    fixture.clear();
    return s.backend->open(device_a);
}

void case_xml_read_failure() {
    Fixture fixture;
    auto opened = open_failing([](Mock& m) { m.fail_read(fx::xml_address + 4096, -1010); }, fixture);
    expect(opened, ErrorCode::io_error, "GC_ERR_IO");
    CHECK(contains(opened.error().message, "partial data discarded"));
    CHECK(fixture.count("DevClose mock-dev-A") == 1 && g::genapi_object_counts().devices == 0);
}

void case_genapi_creation_failure() {
    Fixture fixture;
    auto opened = open_failing([](Mock& m) { m.port(fx::Model{}, "<?xml version=\"1.0\"?><RegisterDescription>"); },
                               fixture);
    expect(opened, ErrorCode::io_error, "not well-formed XML");
    CHECK(fixture.count("DevClose mock-dev-A") == 1);
    CHECK(g::genapi_object_counts().devices == 0 && g::genapi_object_counts().genicams == 0);
}

void case_profile_admission_failure() {
    Fixture fixture;
    auto opened = open_failing([](Mock& m) { m.fail_read(fx::vendor, -1011); }, fixture);
    expect(opened, ErrorCode::timeout, "DeviceVendorName");
    CHECK(contains(opened.error().message, "GC_ERR_TIMEOUT"));
    CHECK(fixture.count("DevClose mock-dev-A") == 1 && fixture.count("GCWritePort") == 0);
    CHECK(g::genapi_object_counts().devices == 0);
}

// Teardown failures: each close attempted once and reported; the unresolved
// chain and its producer are retained for the process lifetime.
void case_close_failure(const char* function) {
    Fixture fixture;
    auto s = prepare(cognex_profile());
    {
        Mock mock;
        mock.model(fx::Model{});
    }
    auto device = take(s.backend->open(device_a));
    {
        Mock mock;
        mock.fail((std::string(function) + "=-1010").c_str());
    }
    fixture.clear();
    auto closed = device->close();
    expect(closed, ErrorCode::io_error, function);
    CHECK(contains(closed.error().message, "retained for the process lifetime"));
    CHECK(fixture.count(std::string("attempt ") + function) == 1); // attempted exactly once
    CHECK(g::genapi_object_counts().genicams == 0);               // GenApi released without native I/O
    expect(device->close(), ErrorCode::io_error, function);       // same result, no second native call
    // R1: a parent is never closed while its child is unresolved.
    const std::string fn(function);
    if (fn == "DevClose") CHECK(fixture.count("attempt IFClose") == 0 && fixture.count("attempt TLClose") == 0);
    if (fn == "IFClose") CHECK(fixture.count("attempt TLClose") == 0);
    device.reset(); // public object gone: the identity must stay blocked (R1-2)
    { Mock mock; mock.fail(""); } // natively, a reopen would now succeed
    const auto before = fixture.calls().size();
    expect(s.backend->open(device_a), ErrorCode::busy, "unresolved native ownership");
    expect(s.backend->open(g::compose_device_id("mock-if-0", "mock-dev-B")), ErrorCode::busy, "unresolved");
    expect(s.backend->enumerate(), ErrorCode::busy, "unresolved native ownership");
    CHECK(fixture.calls().size() == before && fixture.count("IFOpenDevice") == 0); // no native call at all
    s.backend.reset();
    CHECK(fixture.count(std::string("attempt ") + function) == 1); // no destructor retry
    CHECK(g::quarantined_native_sessions() == 1);
    CHECK(mapped(GENTL_MOCK)); // never unmapped under unresolved native ownership
    // Another backend instance cannot bypass the block: the retained producer
    // is still registered, so it is never loaded or initialized again.
    auto again = g::make_backend(g::Options{GENTL_MOCK}, cognex_profile());
    expect(again->open(device_a), ErrorCode::busy, "already loaded (or retained)");
    expect(again->enumerate(), ErrorCode::busy, "already loaded (or retained)");
    CHECK(fixture.calls().size() == before);
}

void case_gcclose_failure() {
    Fixture fixture;
    {
        auto s = prepare(cognex_profile());
        {
            Mock mock;
            mock.model(fx::Model{});
        }
        auto device = take(s.backend->open(device_a));
        CHECK(device->close());
        Mock mock;
        mock.fail("GCCloseLib=-1010");
    } // last owner: GCCloseLib fails -> producer retained (GENTL-001 quarantine)
    CHECK(gige_mel::gentl::retained_producers() == 1 && mapped(GENTL_MOCK));
}

// ---- GIGE-MEL-GENTL-002-R1 isolated failure cases (one process each) --------

// Every R1 refusal must be made with no native call. The retained producer
// must stay mapped after the backend is destroyed, and a new backend cannot
// reacquire it.
void expect_blocked(Fixture& fixture, std::shared_ptr<b::ICameraBackend>& backend, std::string_view why) {
    const auto before = fixture.calls().size();
    expect(backend->open(device_a), ErrorCode::busy, why);
    expect(backend->enumerate(), ErrorCode::busy, "unresolved");
    CHECK(fixture.calls().size() == before);
    backend.reset();
    CHECK(fixture.calls().size() == before && mapped(GENTL_MOCK)); // no destructor retry, no unload
    CHECK(g::quarantined_native_sessions() == 1);
    auto again = g::make_backend(g::Options{GENTL_MOCK}, p::make_mono_free_running_profile());
    expect(again->enumerate(), ErrorCode::busy, "already loaded (or retained)");
    CHECK(fixture.calls().size() == before);
}

// R1-1: enumeration cleanup. `failures` is injected from GCInitLib onward.
void case_enumeration_cleanup(const char* failures, const std::string& first, const std::string& cleanup,
                              std::size_t ifclose_attempts, std::size_t tlclose_attempts) {
    Fixture fixture({{"GENTL_MOCK_FAIL", failures}});
    std::shared_ptr<b::ICameraBackend> backend =
        g::make_backend(g::Options{GENTL_MOCK, g::DeviceAccess::read_only, 100ms}, p::make_mono_free_running_profile());
    auto listed = backend->enumerate();
    expect(listed, ErrorCode::io_error, first);
    const auto& message = listed.error().message;
    CHECK(message.rfind("GenTL " + first + " returned", 0) == 0); // the FIRST failure is preserved
    if (!cleanup.empty()) CHECK(contains(message, "; cleanup: GenTL " + cleanup + " returned")); // appended
    CHECK(contains(message, "retained for the process lifetime"));
    CHECK(fixture.count("attempt IFClose") == ifclose_attempts); // each handle attempted at most once
    CHECK(fixture.count("attempt TLClose") == tlclose_attempts); // never while a child is unresolved
    CHECK(fixture.count("IFOpenDevice") == 0 && fixture.count("GCCloseLib") == 0);
    expect_blocked(fixture, backend, "unresolved native ownership");
    CHECK(fixture.count("attempt IFClose") == ifclose_attempts && fixture.count("attempt TLClose") == tlclose_attempts);
}

// R1-2: a public device destroyed AFTER its backend still records its failed
// close in the registry that backend created (sanitizers check the lifetime).
void case_destructor_close_failure_after_backend() {
    Fixture fixture;
    auto s = prepare(cognex_profile());
    {
        Mock mock;
        mock.model(fx::Model{});
    }
    auto device = take(s.backend->open(device_a));
    {
        Mock mock;
        mock.fail("DevClose=-1010");
    }
    s.backend.reset();
    fixture.clear();
    device.reset(); // destructor: the first and only DevClose attempt, then retirement
    CHECK(fixture.count("attempt DevClose") == 1 && fixture.count("attempt IFClose") == 0);
    CHECK(fixture.count("attempt TLClose") == 0 && fixture.count("GCCloseLib") == 0);
    CHECK(g::quarantined_native_sessions() == 1 && mapped(GENTL_MOCK));
    auto again = g::make_backend(g::Options{GENTL_MOCK}, cognex_profile());
    expect(again->open(device_a), ErrorCode::busy, "already loaded (or retained)");
    CHECK(fixture.count("IFOpenDevice") == 0 && fixture.count("attempt DevClose") == 1);
}

// R1-2: profile admission fails, then its partial-open DevClose fails.
void case_partial_open_cleanup_failure() {
    Fixture fixture;
    auto s = prepare(cognex_profile());
    {
        Mock mock;
        mock.model(fx::Model{});
        mock.fail_read(fx::vendor, -1011);
        mock.fail("DevClose=-1010");
    }
    fixture.clear();
    auto opened = s.backend->open(device_a);
    expect(opened, ErrorCode::timeout, "DeviceVendorName"); // first failure and its code are kept
    CHECK(contains(opened.error().message, "; cleanup: GenTL DevClose returned GC_ERR_IO"));
    CHECK(fixture.count("attempt DevClose") == 1 && fixture.count("attempt IFClose") == 0);
    CHECK(fixture.count("attempt TLClose") == 0 && g::genapi_object_counts().genicams == 0);
    {
        Mock mock;
        mock.fail_read(0, 0);
        mock.fail("");
    }
    expect_blocked(fixture, s.backend, "unresolved native ownership");
    CHECK(fixture.count("IFOpenDevice") == 1 && fixture.count("attempt DevClose") == 1);
}

// R1-2: the consumer's own cleanup of a just-opened device fails (DevGetPort,
// then DevClose). The producer is tainted and that handle's parents stay open.
void case_consumer_open_cleanup_failure() {
    Fixture fixture;
    auto s = prepare(cognex_profile());
    {
        Mock mock;
        mock.model(fx::Model{});
        mock.fail("DevGetPort,DevClose=-1010");
    }
    fixture.clear();
    auto opened = s.backend->open(device_a);
    expect(opened, ErrorCode::io_error, "DevGetPort");
    CHECK(contains(opened.error().message, "cleanup GenTL DevClose returned GC_ERR_IO"));
    CHECK(contains(opened.error().message, "parent handles were not closed"));
    CHECK(fixture.count("attempt DevClose") == 1 && fixture.count("attempt IFClose") == 0);
    CHECK(fixture.count("attempt TLClose") == 0);
    {
        Mock mock;
        mock.fail("");
    }
    expect_blocked(fixture, s.backend, "unresolved native close");
    CHECK(fixture.count("IFOpenDevice") == 1 && fixture.count("attempt DevClose") == 1);
}

// R1-3: allocation fails after the native open, then the cleanup it triggers
// fails. Neither failure may conceal the other.
void case_allocation_cleanup_failure() {
    Fixture fixture;
    auto s = prepare(cognex_profile());
    {
        Mock mock;
        mock.model(fx::Model{});
        mock.fail("DevClose=-1010");
    }
    fixture.clear();
    g::fail_allocation_for_testing(g::AllocationPoint::device_object);
    auto opened = s.backend->open(device_a);
    expect(opened, ErrorCode::resource_exhausted, "allocation failed"); // first failure kept
    CHECK(contains(opened.error().message, "; cleanup: GenTL DevClose returned GC_ERR_IO"));
    CHECK(contains(opened.error().message, "retained for the process lifetime"));
    CHECK(fixture.count("attempt DevClose") == 1 && fixture.count("attempt IFClose") == 0);
    CHECK(fixture.count("attempt TLClose") == 0 && g::genapi_object_counts().genicams == 0);
    {
        Mock mock;
        mock.fail("");
    }
    expect_blocked(fixture, s.backend, "unresolved native ownership");
    CHECK(fixture.count("IFOpenDevice") == 1 && fixture.count("attempt DevClose") == 1);
}

} // namespace


int main(int argc, char** argv) {
    const std::map<std::string, std::function<void()>> failures{
        {"xml_read_failure", case_xml_read_failure},
        {"genapi_creation_failure", case_genapi_creation_failure},
        {"profile_admission_failure", case_profile_admission_failure},
        {"devclose_failure", [] { case_close_failure("DevClose"); }},
        {"ifclose_failure", [] { case_close_failure("IFClose"); }},
        {"tlclose_failure", [] { case_close_failure("TLClose"); }},
        {"gcclose_failure", case_gcclose_failure},
        // GIGE-MEL-GENTL-002-R1
        {"enum_ifclose_failure", [] { case_enumeration_cleanup("IFClose=-1010", "IFClose", "", 1, 0); }},
        {"enum_tlclose_failure", [] { case_enumeration_cleanup("TLClose=-1010", "TLClose", "", 1, 1); }},
        {"enum_discovery_ifclose_failure",
         [] { case_enumeration_cleanup("IFUpdateDeviceList,IFClose=-1010", "IFUpdateDeviceList", "IFClose", 1, 0); }},
        {"enum_discovery_tlclose_failure",
         [] { case_enumeration_cleanup("IFUpdateDeviceList,TLClose=-1010", "IFUpdateDeviceList", "TLClose", 1, 1); }},
        {"enum_system_discovery_tlclose_failure",
         [] { case_enumeration_cleanup("TLUpdateInterfaceList,TLClose=-1010", "TLUpdateInterfaceList", "TLClose", 0, 1); }},
        {"destructor_close_failure_after_backend", case_destructor_close_failure_after_backend},
        {"partial_open_cleanup_failure", case_partial_open_cleanup_failure},
        {"consumer_open_cleanup_failure", case_consumer_open_cleanup_failure},
        {"allocation_cleanup_failure", case_allocation_cleanup_failure},
    };
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"construction_performs_no_io", construction_performs_no_io},
        {"composite_identifiers", composite_identifiers},
        {"enumerate_and_exact_selection", enumerate_and_exact_selection},
        {"mono_default_profile", mono_default_profile},
        {"cognex_matching_identity", cognex_matching_identity},
        {"cognex_identity_mismatch", cognex_identity_mismatch},
        {"cognex_required_features_and_types", cognex_required_features_and_types},
        {"cognex_bounds_and_readback", cognex_bounds_and_readback},
        {"register_endianness", register_endianness},
        {"native_read_failure_propagation", native_read_failure_propagation},
        {"xml_location_handling", xml_location_handling},
        {"xml_content_validation", xml_content_validation},
        {"undeclared_port_byte_order", undeclared_port_byte_order},
        {"unsupported_operations_and_readonly", unsupported_operations_and_readonly},
        {"offline_feature_writes", offline_feature_writes},
        {"ownership_and_teardown_order", ownership_and_teardown_order},
        {"unbound_port_access_refused", unbound_port_access_refused},
        {"concurrent_transactions", concurrent_transactions},
        {"profile_portability", profile_portability},
        {"enumeration_cleanup_control", enumeration_cleanup_control},
        {"ownership_reservation", ownership_reservation},
        {"allocation_failure_recovery", allocation_failure_recovery},
    };
    try {
        if (argc == 2) {
            const auto found = failures.find(argv[1]);
            if (found == failures.end()) { std::cerr << "unknown case " << argv[1] << "\n"; return 2; }
            found->second();
            std::cout << "PASS " << argv[1] << "\n";
            return 0;
        }
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "PASS " << name << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    return 0;
}

