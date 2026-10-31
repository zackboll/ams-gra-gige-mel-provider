// Direct adapter tests (GIGE-MEL-008) against the REAL Aravis 0.8.36 library
// and its in-process Fake interface (no network, no arv-fake-gv-camera, no
// physical camera). Feature reads/writes run through Aravis's own GenICam
// implementation of the Fake camera. Private hooks only arrange native state
// through that same SDK or observe lifetime; they do not replace SDK I/O.
#include "aravis_fake_support.hpp"
#include "camera_profile.hpp"

#include <arv.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace backend = gige_mel::backend;
namespace aravis = gige_mel::backend::aravis;
namespace profile = gige_mel::backend::aravis_profile;
using namespace std::chrono_literals;

namespace {

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression)) {                                                                         \
            throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) +           \
                                     ": " #expression);                                              \
        }                                                                                            \
    } while (false)

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

std::shared_ptr<backend::ICameraDevice> open_fake(backend::ICameraBackend& camera) {
    auto opened = camera.open(aravis_fake::device_id);
    if (!opened) throw std::runtime_error("open Fake_1 failed: " + opened.error().message);
    return std::move(opened).value();
}

// Native helpers used only to ARRANGE or OBSERVE state through the real SDK.
void native(backend::ICameraDevice& device, const std::function<void(ArvDevice*)>& fn) {
    CHECK(aravis::with_native_device_for_testing(device, fn));
}
std::string native_string(backend::ICameraDevice& device, const char* feature) {
    std::string value;
    native(device, [&](ArvDevice* d) { value = arv_device_get_string_feature_value(d, feature, nullptr); });
    return value;
}
std::int64_t native_integer(backend::ICameraDevice& device, const char* feature) {
    std::int64_t value = 0;
    native(device, [&](ArvDevice* d) { value = arv_device_get_integer_feature_value(d, feature, nullptr); });
    return value;
}
void set_native_string(backend::ICameraDevice& device, const char* feature, const char* value) {
    native(device, [&](ArvDevice* d) {
        GError* error = nullptr;
        arv_device_set_string_feature_value(d, feature, value, &error);
        CHECK(!error);
    });
}
void set_native_integer(backend::ICameraDevice& device, const char* feature, std::int64_t value) {
    native(device, [&](ArvDevice* d) {
        GError* error = nullptr;
        arv_device_set_integer_feature_value(d, feature, value, &error);
        CHECK(!error);
    });
}
void impose_exposure_max(ArvDevice* d, double maximum) {
    GError* error = nullptr;
    arv_gc_float_impose_max(ARV_GC_FLOAT(arv_device_get_feature(d, "ExposureTimeAbs")), maximum, &error);
    CHECK(!error);
}
void sdk_set_string(ArvDevice* d, const char* feature, const char* value) {
    GError* error = nullptr;
    arv_device_set_string_feature_value(d, feature, value, &error);
    if (error) { g_error_free(error); throw std::runtime_error(std::string("could not arrange ") + feature); }
}
// Native integer limits changed through the SDK's own property nodes; the
// readback through the SDK proves the arrangement before it is relied upon.
void impose_integer_min(ArvDevice* d, const char* feature, std::int64_t minimum) {
    auto* integer = ARV_GC_INTEGER(arv_device_get_feature(d, feature));
    GError* error = nullptr;
    arv_gc_integer_impose_min(integer, minimum, &error);
    CHECK(!error);
    CHECK(arv_gc_integer_get_min(integer, nullptr) == minimum);
}
void set_integer_grid(ArvDevice* d, const char* feature, std::int64_t minimum, std::int64_t increment) {
    auto* node = ARV_DOM_NODE(arv_device_get_feature(d, feature));
    bool set = false;
    for (ArvDomNode* child = arv_dom_node_get_first_child(node); child; child = arv_dom_node_get_next_sibling(child)) {
        if (!ARV_IS_GC_PROPERTY_NODE(child) ||
            arv_gc_property_node_get_node_type(ARV_GC_PROPERTY_NODE(child)) != ARV_GC_PROPERTY_NODE_TYPE_INCREMENT)
            continue;
        GError* error = nullptr;
        arv_gc_property_node_set_int64(ARV_GC_PROPERTY_NODE(child), increment, &error);
        CHECK(!error);
        set = true;
    }
    CHECK(set);
    impose_integer_min(d, feature, minimum);
    CHECK(arv_gc_integer_get_inc(ARV_GC_INTEGER(node), nullptr) == increment);
}

// ---- GenICam tree arrangement through Aravis's PUBLIC DOM API (test-only).
// Nodes are appended to the open device's own ArvGc document; every later
// read still runs through the SDK's GenICam implementation, never a mock.
ArvDomNode* gc_add(ArvDevice* d, ArvDomNode* parent, const char* tag, const char* name = nullptr,
                   const char* text = nullptr) {
    auto* document = ARV_DOM_DOCUMENT(arv_device_get_genicam(d));
    if (!parent) parent = ARV_DOM_NODE(arv_dom_document_get_document_element(document));
    auto* node = ARV_DOM_NODE(arv_dom_document_create_element(document, tag));
    if (!node || !arv_dom_node_append_child(parent, node))
        throw std::runtime_error(std::string("could not add GenICam <") + tag + ">");
    // Named after insertion, so the node registers with its owner document.
    if (name) arv_dom_element_set_attribute(ARV_DOM_ELEMENT(node), "Name", name);
    if (text) arv_dom_node_append_child(node, ARV_DOM_NODE(arv_dom_document_create_text_node(document, text)));
    return node;
}
void gc_add_float(ArvDevice* d, const char* name, const char* value) {
    auto* node = gc_add(d, nullptr, "Float", name);
    gc_add(d, node, "Value", nullptr, value);
    gc_add(d, node, "Min", nullptr, "10.0");
    gc_add(d, node, "Max", nullptr, "1000000.0");
}
void gc_add_boolean(ArvDevice* d, const char* name, bool value) {
    const std::string storage = std::string("GigeMelTestValueOf") + name;
    gc_add(d, gc_add(d, nullptr, "Integer", storage.c_str()), "Value", nullptr, value ? "1" : "0");
    gc_add(d, gc_add(d, nullptr, "Boolean", name), "pValue", nullptr, storage.c_str());
}
// Fixed <Max> on an integer feature through the SDK's property node; the SDK
// readback proves the arrangement before it is relied upon.
void gc_set_integer_max(ArvDevice* d, const char* feature, std::int64_t maximum) {
    gc_add(d, ARV_DOM_NODE(arv_device_get_feature(d, feature)), "Max", nullptr, std::to_string(maximum).c_str());
    GError* error = nullptr;
    CHECK(arv_gc_integer_get_max(ARV_GC_INTEGER(arv_device_get_feature(d, feature)), &error) == maximum);
    CHECK(!error);
}
// Explicit <Inc> on a float feature. Returns the RAW value the pinned SDK
// getter reports for it, after asserting that the query raised no GError.
double gc_set_float_inc(ArvDevice* d, const char* feature, const char* text) {
    auto* real = ARV_GC_FLOAT(arv_device_get_feature(d, feature));
    gc_add(d, ARV_DOM_NODE(real), "Inc", nullptr, text);
    GError* error = nullptr;
    const double raw = arv_gc_float_get_inc(real, &error);
    if (error) { g_error_free(error); throw std::runtime_error(std::string("Inc query failed for ") + feature); }
    return raw;
}
double sdk_float_inc(ArvDevice* d, const char* feature, bool& failed) {
    GError* error = nullptr;
    const double raw = arv_gc_float_get_inc(ARV_GC_FLOAT(arv_device_get_feature(d, feature)), &error);
    failed = error != nullptr;
    if (error) g_error_free(error);
    return raw;
}

// The SDK SUCCESSFULLY reports `feature` as not implemented (no GError).
void gc_set_unimplemented(ArvDevice* d, const char* feature) {
    const std::string flag = std::string("GigeMelTestZeroFor") + feature;
    gc_add(d, gc_add(d, nullptr, "Integer", flag.c_str()), "Value", nullptr, "0");
    gc_add(d, ARV_DOM_NODE(arv_device_get_feature(d, feature)), "pIsImplemented", nullptr, flag.c_str());
    GError* error = nullptr;
    CHECK(!arv_gc_feature_node_is_implemented(ARV_GC_FEATURE_NODE(arv_device_get_feature(d, feature)), &error));
    CHECK(!error);
}
// Makes the SDK's implementation query for `feature` FAIL: its pIsImplemented
// references an IntReg whose pPort names no node, so Aravis raises
// ARV_GC_ERROR_NODE_NOT_FOUND while evaluating it. Proves the real query
// returns a non-null GError, and returns the SDK's own message.
std::string break_implementation_query(ArvDevice* d, const char* feature) {
    const std::string reg = std::string("GigeMelTestBrokenImplementedFor") + feature;
    auto* broken = gc_add(d, nullptr, "IntReg", reg.c_str());
    gc_add(d, broken, "Address", nullptr, "0x9000");
    gc_add(d, broken, "Length", nullptr, "4");
    gc_add(d, broken, "pPort", nullptr, "GigeMelTestNonexistentPort");
    ArvGcNode* node = arv_device_get_feature(d, feature);
    if (!node) throw std::runtime_error(std::string("no GenICam node ") + feature);
    gc_add(d, ARV_DOM_NODE(node), "pIsImplemented", nullptr, reg.c_str());
    GError* error = nullptr;
    (void)arv_gc_feature_node_is_implemented(ARV_GC_FEATURE_NODE(node), &error);
    if (!error) throw std::runtime_error(std::string("arrangement did not make the SDK query fail for ") + feature);
    std::string message = error->message;
    g_error_free(error);
    return message;
}

// Unrelated native state that apply() must never touch.
struct Unrelated {
    std::string pixel, trigger_selector, trigger_mode, acquisition_mode, gain_auto;
    std::int64_t gain{0}, binning_h{0}, binning_v{0};
    bool operator==(const Unrelated&) const = default;
};
Unrelated unrelated(backend::ICameraDevice& d) {
    return {native_string(d, "PixelFormat"), native_string(d, "TriggerSelector"), native_string(d, "TriggerMode"),
            native_string(d, "AcquisitionMode"), native_string(d, "GainAuto"), native_integer(d, "GainRaw"),
            native_integer(d, "BinningHorizontal"), native_integer(d, "BinningVertical")};
}

// Test-only interface whose discovery succeeds with zero devices.
ArvDevice* empty_open(ArvInterface*, const char*, GError**) { return nullptr; }
void empty_update(ArvInterface*, GArray*) {}
void empty_class_init(gpointer klass, gpointer) {
    auto* interface_class = static_cast<ArvInterfaceClass*>(klass);
    interface_class->update_device_list = empty_update;
    interface_class->open_device = empty_open;
    interface_class->protocol = "TestEmpty";
}
ArvInterface* empty_interface() {
    static ArvInterface* instance = [] {
        const GType type = g_type_register_static_simple(ARV_TYPE_INTERFACE, "GigeMelTestEmptyInterface",
                                                         sizeof(ArvInterfaceClass), empty_class_init,
                                                         sizeof(ArvInterface), nullptr, GTypeFlags{});
        return static_cast<ArvInterface*>(g_object_new(type, nullptr));
    }();
    return instance;
}
ArvInterface* missing_interface() { return nullptr; }

// GObject weak-reference observer: never keeps the measured object alive.
struct Retired {
    std::atomic<int> count{0};
    static void notify(gpointer data, GObject*) { static_cast<Retired*>(data)->count.fetch_add(1); }
    void watch(gpointer object) { g_object_weak_ref(G_OBJECT(object), &Retired::notify, this); }
};

// Counts native opens and attaches a weak reference to every opened device.
struct NativeOpens {
    std::atomic<int> opened{0};
    Retired retired;
    aravis::TestHooks hooks() {
        aravis::TestHooks h;
        h.after_native_open = [this](ArvDevice* d) { opened.fetch_add(1); retired.watch(d); };
        return h;
    }
    int live() const { return opened.load() - retired.count.load(); }
};

std::atomic<int> interface_requests{0};
ArvInterface* counting_fake() {
    interface_requests.fetch_add(1);
    return arv_fake_interface_get_instance();
}

const backend::CameraConfiguration fake_initial{512, 512, 0, 0, 25, 10'000'000ns};

// ===========================================================================
void checked_conversions() {
    CHECK(profile::frame_rate_from_native(25.0).value() == 25);
    CHECK(profile::frame_rate_from_native(1000.0).value() == 1000);
    // Representation error only: the next double above 25 is still 25 Hz.
    CHECK(profile::frame_rate_from_native(std::nextafter(25.0, 26.0)).value() == 25);
    // The pinned Fake's actual 30 Hz readback is a real fraction: never rounded.
    const auto thirty = profile::frame_rate_from_native(30.000300003000032);
    CHECK(!thirty && thirty.error().code == backend::ErrorCode::unsupported);
    CHECK(contains(thirty.error().message, "30.000300003") && contains(thirty.error().message, "AcquisitionFrameRate"));
    CHECK(!profile::frame_rate_from_native(3.0000030000029998));
    CHECK(profile::frame_rate_from_native(0.5).error().code == backend::ErrorCode::unsupported);
    CHECK(profile::frame_rate_from_native(0.0).error().code == backend::ErrorCode::unsupported);
    CHECK(profile::frame_rate_from_native(5e9).error().code == backend::ErrorCode::unsupported); // > uint32
    for (double bad : {std::nan(""), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -1.0})
        CHECK(profile::frame_rate_from_native(bad).error().code == backend::ErrorCode::io_error);

    CHECK(profile::exposure_from_native(1250.0).value() == 1'250'000ns);
    CHECK(profile::exposure_from_native(1250.5).value() == 1'250'500ns);
    CHECK(profile::exposure_from_native(0.001).value() == 1ns);
    const auto half = profile::exposure_from_native(0.0005, "ExposureTime");
    CHECK(!half && half.error().code == backend::ErrorCode::unsupported && contains(half.error().message, "ExposureTime"));
    CHECK(profile::exposure_from_native(0.0).error().code == backend::ErrorCode::unsupported);
    CHECK(profile::exposure_from_native(1e300).error().code == backend::ErrorCode::unsupported); // overflow, no UB cast
    CHECK(profile::exposure_from_native(9.3e15).error().code == backend::ErrorCode::unsupported);
    for (double bad : {std::nan(""), std::numeric_limits<double>::infinity(), -0.5})
        CHECK(profile::exposure_from_native(bad).error().code == backend::ErrorCode::io_error);

    CHECK(profile::frame_rate_to_native(0).error().code == backend::ErrorCode::invalid_argument);
    CHECK(profile::frame_rate_to_native(4294967295U).value() == 4294967295.0);
    CHECK(profile::exposure_to_native(0ns).error().code == backend::ErrorCode::invalid_argument);
    CHECK(profile::exposure_to_native(-5ns).error().code == backend::ErrorCode::invalid_argument);
    CHECK(profile::exposure_to_native(1500ns).value() == 1.5);
    CHECK(profile::exposure_to_native(std::chrono::nanoseconds{(std::int64_t{1} << 53) + 1}).error().code ==
          backend::ErrorCode::invalid_argument);
}

profile::NativeSnapshot snapshot_model() {
    profile::NativeSnapshot n;
    n.sensor_width = 2048;
    n.sensor_height = 2048;
    n.width = {512, 1, 2048, 1};
    n.height = {512, 1, 2048, 1};
    n.offset_x = {0, 0, 2048, 1};
    n.offset_y = {0, 0, 2048, 1};
    n.frame_rate_hz = {25.0, 0.1, 1000.0, std::nullopt, std::nullopt};
    n.exposure_us = {10000.0, 10.0, 1e7, std::nullopt, std::nullopt};
    n.exposure_feature = "ExposureTimeAbs";
    n.pixel_format = "Mono8";
    n.mono8_available = true;
    n.trigger_selector = "FrameStart";
    n.trigger_mode = "Off";
    return n;
}

std::vector<std::string> order(const std::vector<profile::NativeWrite>& plan) {
    std::vector<std::string> names;
    for (const auto& write : plan) names.push_back(write.feature);
    return names;
}

void profile_translation() {
    auto n = snapshot_model();
    const auto caps = profile::capabilities_from(n).value();
    CHECK(caps.sensor_width == 2048 && caps.sensor_height == 2048 && caps.width_increment == 1 && caps.mono8);
    CHECK(caps.minimum_frame_rate_hz == 1 && caps.maximum_frame_rate_hz == 1000 && caps.minimum_exposure == 10'000ns);
    CHECK(!caps.external_trigger && !caps.external_buffer_acquisition);

    // Unsupported profiles fail with a diagnostic; nothing is fabricated.
    auto bad = n;
    bad.pixel_format = "Mono16";
    CHECK(contains(profile::capabilities_from(bad).error().message, "Mono16"));
    bad = n;
    bad.trigger_mode = "On";
    CHECK(contains(profile::capabilities_from(bad).error().message, "TriggerMode"));
    bad = n;
    bad.exposure_auto = "Continuous";
    CHECK(contains(profile::capabilities_from(bad).error().message, "ExposureAuto"));
    bad = n;
    bad.frame_rate_enable = false;
    CHECK(contains(profile::capabilities_from(bad).error().message, "AcquisitionFrameRateEnable"));
    bad = n;
    bad.width.increment = 0;
    CHECK(profile::capabilities_from(bad).error().code == backend::ErrorCode::unsupported);
    bad = n;
    bad.width = {6, 2, 2048, 4}; // minimum off its own increment grid
    CHECK(contains(profile::capabilities_from(bad).error().message, "increment"));
    bad = n;
    bad.frame_rate_hz.minimum = 0.2;
    bad.frame_rate_hz.maximum = 0.8;
    CHECK(contains(profile::capabilities_from(bad).error().message, "no integer rate"));
    bad = n;
    bad.exposure_us.unit = "ms";
    CHECK(contains(profile::capabilities_from(bad).error().message, "unit 'ms'"));
    bad = n;
    bad.sensor_width = 0;
    CHECK(!profile::capabilities_from(bad));

    // Bounds and increment rejection before any write.
    CHECK(profile::plan_writes({4096, 64, 0, 0, 25, 1ms}, n, n).error().code == backend::ErrorCode::invalid_argument);
    CHECK(contains(profile::plan_writes({64, 64, 2000, 0, 25, 1ms}, n, n).error().message, "OffsetX"));
    CHECK(contains(profile::plan_writes({64, 64, 0, 0, 2000, 1ms}, n, n).error().message, "AcquisitionFrameRate"));
    CHECK(contains(profile::plan_writes({64, 64, 0, 0, 25, 5us}, n, n).error().message, "ExposureTimeAbs"));
    // The grid is taken from the CURRENT (fresh) snapshot. Here open-time and
    // fresh facts agree; a differing increment is tested in fresh_constraints.
    auto grid = n;
    grid.width.increment = 8;
    grid.width.minimum = 8;
    CHECK(contains(profile::plan_writes({100, 64, 0, 0, 25, 1ms}, grid, grid).error().message, "native grid"));
    CHECK(profile::plan_writes({96, 64, 0, 0, 25, 1ms}, grid, grid));
    auto stepped = n;
    stepped.exposure_us.increment = 100.0;
    CHECK(contains(profile::plan_writes({64, 64, 0, 0, 25, 1'050'000ns}, stepped, stepped).error().message, "increment"));
    CHECK(profile::plan_writes({64, 64, 0, 0, 25, 1'010'000ns}, stepped, stepped)); // 10 us + 10 * 100 us

    // ROI order: large near-origin -> small far offset writes size first.
    auto large = n;
    large.width.value = 2048;
    large.height.value = 2048;
    CHECK((order(profile::plan_writes({64, 32, 1900, 2000, 25, 1ms}, large, n).value()) ==
           std::vector<std::string>{"Width", "OffsetX", "Height", "OffsetY", "ExposureTimeAbs", "AcquisitionFrameRate"}));
    // Small far offset -> large near origin writes offset first.
    auto small = n;
    small.width.value = 64;
    small.height.value = 32;
    small.offset_x.value = 1900;
    small.offset_y.value = 2000;
    CHECK((order(profile::plan_writes({2048, 2048, 0, 0, 10, 1ms}, small, n).value()) ==
           std::vector<std::string>{"OffsetX", "Width", "OffsetY", "Height", "AcquisitionFrameRate", "ExposureTimeAbs"}));
    // Unchanged integers are not rewritten.
    CHECK((order(profile::plan_writes({512, 512, 0, 0, 25, 10ms}, n, n).value()) ==
           std::vector<std::string>{"ExposureTimeAbs", "AcquisitionFrameRate"}));
}

void expect_rejected(const backend::Result<std::vector<profile::NativeWrite>>& planned, backend::ErrorCode code,
                     std::string_view part) {
    CHECK(!planned && planned.error().code == code);
    if (!contains(planned.error().message, part))
        throw std::runtime_error("expected '" + std::string(part) + "' in: " + planned.error().message);
}

// Representative malformed limits, geometry and current ROI (snapshots only).
void native_fact_validation_malformed(const profile::NativeSnapshot& n, const backend::CameraConfiguration& request) {
    constexpr auto int64_min = std::numeric_limits<std::int64_t>::min();
    constexpr auto int64_max = std::numeric_limits<std::int64_t>::max();
    using Arrange = void (*)(profile::NativeSnapshot&);
    using backend::ErrorCode;
    const struct { Arrange arrange; std::string_view part; ErrorCode code; } malformed[] = {
        {[](auto& m) { m.sensor_width = int64_max; }, "SensorWidth reads back", ErrorCode::unsupported},
        {[](auto& m) { m.sensor_height = -1; }, "SensorHeight reads back -1", ErrorCode::unsupported},
        {[](auto& m) { m.width.increment = -8; }, "Width advertises increment -8", ErrorCode::unsupported},
        {[](auto& m) { m.offset_y.increment = std::int64_t{1} << 40; }, "OffsetY advertises increment", ErrorCode::unsupported},
        {[](auto& m) { m.height = {512, 64, 32, 1}; }, "Height advertises minimum 64 above its maximum 32", ErrorCode::unsupported},
        {[](auto& m) { m.width.minimum = 4096; m.width.maximum = 8192; }, "outside the supported grid origins",
         ErrorCode::unsupported},
        {[](auto& m) { m.offset_x.minimum = 2048; }, "OffsetX advertises minimum 2048", ErrorCode::unsupported},
        {[](auto& m) { m.offset_x.minimum = -4; }, "OffsetX advertises minimum -4", ErrorCode::unsupported},
        {[](auto& m) { m.offset_x.value = int64_min; }, "OffsetX reads back -9223372036854775808", ErrorCode::unsupported},
        {[](auto& m) { m.offset_y.value = 1600; }, "OffsetY reads back 1600 with Height 512", ErrorCode::unsupported},
        {[](auto& m) { m.height.value = 0; }, "Height reads back 0", ErrorCode::unsupported},
        {[](auto& m) { m.frame_rate_hz.maximum = std::nan(""); }, "AcquisitionFrameRate advertises invalid", ErrorCode::unsupported},
        {[](auto& m) { m.exposure_us.maximum = 1e300; }, "ExposureTimeAbs advertises invalid or unrepresentable",
         ErrorCode::unsupported},
        {[](auto& m) { m.frame_rate_hz.value = std::numeric_limits<double>::infinity(); },
         "AcquisitionFrameRate returned an invalid native value", ErrorCode::io_error},
    };
    for (const auto& c : malformed) {
        auto s = n;
        c.arrange(s);
        expect_rejected(profile::plan_writes(request, s, s), c.code, c.part);
        // GIGE-MEL-009-R1 (finding C): the acquisition preflight applies the
        // SAME structural/current-geometry checks to a fresh snapshot.
        const auto preflight = profile::acquisition_preflight(s, s);
        CHECK(!preflight && preflight.error().code == c.code);
        if (!contains(preflight.error().message, c.part))
            throw std::runtime_error("preflight: expected '" + std::string(c.part) + "' in: " + preflight.error().message);
    }
}

// Valid boundaries and the separation of safety from truthful readback.
void native_fact_validation_boundaries(const profile::NativeSnapshot& n) {
    CHECK(profile::plan_writes({2048, 2048, 0, 0, 25, 1ms}, n, n));  // full sensor
    CHECK(profile::plan_writes({1, 1, 2047, 2047, 25, 1ms}, n, n));  // one far-corner pixel
    CHECK(!profile::plan_writes({2, 1, 2047, 2047, 25, 1ms}, n, n)); // one pixel past the sensor
    auto corner = n;                                                 // offset grid origin at sensor - 1
    corner.offset_x = {0, 2047, 2047, 1};
    CHECK(profile::capabilities_from(corner));
    CHECK(profile::plan_writes({1, 512, 2047, 0, 25, 1ms}, corner, corner));
    auto off_grid = n; // current values need not be on the grid to plan a return to it
    off_grid.width = {515, 8, 2048, 8};
    off_grid.offset_x.value = 3;
    CHECK(profile::plan_writes({512, 512, 0, 0, 25, 1ms}, off_grid, off_grid));
    // A fractional current rate is unrepresentable but recoverable.
    auto fractional = n;
    fractional.frame_rate_hz.value = 30.000300003000032;
    CHECK(!profile::configuration_from(fractional));
    CHECK(profile::plan_writes({800, 600, 8, 4, 40, 2ms}, fractional, fractional));
    // A current 10 ms exposure above a lowered 500 us maximum is still
    // reported truthfully and is recoverable.
    auto outside = n;
    outside.exposure_us.maximum = 500.0;
    CHECK(profile::configuration_from(outside).value().exposure == 10ms);
    CHECK(profile::plan_writes({512, 512, 0, 0, 25, 400us}, outside, outside));
}

// R1 (finding A): native-snapshot/profile regressions. Malformed values are
// constructed in snapshots; no physical camera produced them. Each must be an
// explicit error with no signed overflow (UBSan halts on one in the sanitizer
// build).
void native_fact_validation() {
    const auto n = snapshot_model();
    constexpr auto int64_min = std::numeric_limits<std::int64_t>::min();
    constexpr auto int64_max = std::numeric_limits<std::int64_t>::max();
    const backend::CameraConfiguration request{640, 480, 16, 8, 25, 1ms};

    // 1. INT64_MIN native ROI minimum: formerly reached requested - minimum.
    auto s = n;
    s.width.minimum = int64_min;
    expect_rejected(profile::plan_writes(request, s, s), backend::ErrorCode::unsupported,
                    "Width advertises minimum -9223372036854775808");
    CHECK(contains(profile::capabilities_from(s).error().message, "Width advertises minimum"));
    // The planner never trusts a caller to have validated the snapshot: a
    // valid open-time envelope does not make an unsafe fresh snapshot usable.
    expect_rejected(profile::plan_writes(request, s, n), backend::ErrorCode::unsupported, "Width advertises minimum");

    // 2. INT64_MAX current ROI size with a nonzero requested offset: formerly
    //    reached requested_offset + current_width.
    s = n;
    s.width.value = int64_max;
    expect_rejected(profile::plan_writes(request, s, n), backend::ErrorCode::unsupported,
                    "Width reads back 9223372036854775807");

    // 3. Zero requested size, including a native minimum that would permit it.
    auto permissive = n;
    permissive.width.minimum = 0;
    permissive.height.minimum = 0;
    CHECK(profile::capabilities_from(permissive)); // a structurally valid grid
    expect_rejected(profile::plan_writes({0, 64, 0, 0, 25, 1ms}, permissive, permissive), backend::ErrorCode::invalid_argument,
                    "Width 0 is not a positive ROI size");
    expect_rejected(profile::plan_writes({64, 0, 0, 0, 25, 1ms}, permissive, permissive), backend::ErrorCode::invalid_argument,
                    "Height 0 is not a positive ROI size");
    expect_rejected(profile::plan_writes({0, 64, 0, 0, 25, 1ms}, n, n), backend::ErrorCode::invalid_argument, "Width 0");

    native_fact_validation_malformed(n, request);
    native_fact_validation_boundaries(n);
}

// R1 (finding B): prevalidation uses the FRESH snapshot's limits; the
// open-time envelope only detects an incompatible profile change.
// GIGE-MEL-009-R1 (finding C), SDK-FREE table: the read-only acquisition
// preflight over constructed snapshots. Each open-envelope fact uses the SAME
// comparison and "close and reopen" diagnostic as apply(); dynamic maxima,
// truthful readback and the existing recovery cases are not turned into
// rejections. Real SDK arrangements live in aravis_stream_contract.
void acquisition_preflight_profile() {
    const auto open_time = snapshot_model();
    CHECK(profile::acquisition_preflight(open_time, open_time).value() ==
          profile::configuration_from(open_time).value());
    using Arrange = void (*)(profile::NativeSnapshot&);
    const struct { Arrange arrange; std::string_view part; } reopen[] = {
        {[](auto& m) { m.sensor_width = 1024; }, "SensorWidth changed from 2048 at open to 1024"},
        {[](auto& m) { m.sensor_height = 1024; }, "SensorHeight changed from 2048 at open to 1024"},
        // Structurally valid new grids (minimum on the new increment), so only
        // the envelope comparison can explain the rejection.
        {[](auto& m) { m.width.minimum = 2; m.width.increment = 2; }, "Width increment changed from 1 at open to 2"},
        {[](auto& m) { m.height.minimum = 2; m.height.increment = 2; }, "Height increment changed from 1 at open to 2"},
        {[](auto& m) { m.offset_x.increment = 2; }, "OffsetX increment changed from 1 at open to 2"},
        {[](auto& m) { m.offset_y.increment = 2; }, "OffsetY increment changed from 1 at open to 2"},
        {[](auto& m) { m.exposure_feature = "ExposureTime"; }, "selected exposure feature changed"},
    };
    for (const auto& c : reopen) {
        auto fresh = open_time;
        c.arrange(fresh);
        const auto refused = profile::acquisition_preflight(fresh, open_time);
        CHECK(!refused && refused.error().code == backend::ErrorCode::unsupported);
        if (!contains(refused.error().message, c.part) || !contains(refused.error().message, "close and reopen"))
            throw std::runtime_error("preflight reopen: expected '" + std::string(c.part) + "' in: " +
                                     refused.error().message);
        // Same definition as apply(): plan_writes() rejects it identically.
        CHECK(profile::plan_writes(profile::configuration_from(open_time).value(), fresh, open_time).error().message ==
              refused.error().message);
    }
    // Not reopen conditions: changed dynamic maxima (and a current exposure
    // above a lowered maximum, still reported truthfully), and an off-grid
    // current value - none was ever a profile-envelope fact.
    auto dynamic = open_time;
    dynamic.width.maximum = 1024;
    dynamic.exposure_us.maximum = 500.0;
    CHECK(profile::acquisition_preflight(dynamic, open_time).value().exposure == 10ms);
    auto off_grid = open_time;
    off_grid.width = {515, 8, 2048, 1};
    CHECK(profile::acquisition_preflight(off_grid, open_time).value().width == 515);
    // Representability stays the final check, with its existing diagnostic;
    // configuration_from() itself remains representability-only.
    auto fractional = open_time;
    fractional.frame_rate_hz.value = 30.000300003000032;
    const auto unrepresentable = profile::acquisition_preflight(fractional, open_time);
    CHECK(!unrepresentable && contains(unrepresentable.error().message, "current configuration is not representable"));
    auto outside = open_time; // ROI outside the sensor: configuration_from() alone accepts it
    outside.height.value = 512;
    outside.offset_y.value = 1600;
    CHECK(profile::configuration_from(outside));
    CHECK(contains(profile::acquisition_preflight(outside, open_time).error().message,
                   "OffsetY reads back 1600 with Height 512, outside the sensor extent 2048"));
}

void fresh_constraints() {
    const auto open_time = snapshot_model();
    // Fresh exposure maximum below the request with the rate unchanged:
    // rejected before writes, although the open-time envelope allowed 1 ms.
    auto lowered = open_time;
    lowered.exposure_us.value = 400.0;
    lowered.exposure_us.maximum = 500.0;
    expect_rejected(profile::plan_writes({640, 480, 16, 8, 25, 1ms}, lowered, open_time), backend::ErrorCode::invalid_argument,
                    "ExposureTimeAbs 1000 us is outside the native bounds [10, 500]");
    // Fresh frame-rate bounds are final as well.
    auto rate_limited = open_time;
    rate_limited.frame_rate_hz.maximum = 100.0;
    expect_rejected(profile::plan_writes({512, 512, 0, 0, 200, 1ms}, rate_limited, open_time),
                    backend::ErrorCode::invalid_argument, "AcquisitionFrameRate 200 Hz is outside the native bounds [0.1, 100]");
    // Supported coupling: when the period lengthens first, the exposure
    // maximum may legitimately rise with it, so the SDK enforces it.
    CHECK((order(profile::plan_writes({512, 512, 0, 0, 20, 1ms}, lowered, open_time).value()) ==
           std::vector<std::string>{"AcquisitionFrameRate", "ExposureTimeAbs"}));

    // Fresh minimum with an unchanged increment: validated on the fresh grid.
    auto raised_min = open_time;
    raised_min.width.minimum = 16;
    expect_rejected(profile::plan_writes({8, 512, 0, 0, 25, 1ms}, raised_min, open_time), backend::ErrorCode::invalid_argument,
                    "Width 8 is outside [16, 2048]");
    // A fresh increment differing from the published capabilities is an
    // explicit profile change, never acceptance on the obsolete 1 + k*1 grid.
    auto regrid = open_time;
    regrid.width = {512, 8, 2048, 8};
    const auto changed = profile::plan_writes({641, 512, 0, 0, 25, 1ms}, regrid, open_time);
    expect_rejected(changed, backend::ErrorCode::unsupported, "Width increment changed from 1 at open to 8");
    CHECK(contains(changed.error().message, "reopen") && contains(changed.error().message, "nothing was written"));
    auto resized = open_time;
    resized.sensor_width = 1024;
    resized.width.maximum = 1024;
    resized.offset_x.maximum = 1024;
    expect_rejected(profile::plan_writes({512, 512, 0, 0, 25, 1ms}, resized, open_time), backend::ErrorCode::unsupported,
                    "SensorWidth changed from 2048");
    auto renamed = open_time;
    renamed.exposure_feature = "ExposureTime";
    expect_rejected(profile::plan_writes({512, 512, 0, 0, 25, 1ms}, renamed, open_time), backend::ErrorCode::unsupported,
                    "selected exposure feature changed");

    // Fresh scalar increments, including malformed ones.
    auto stepped = open_time;
    stepped.exposure_us.increment = 100.0;
    expect_rejected(profile::plan_writes({512, 512, 0, 0, 25, 1'050'000ns}, stepped, open_time),
                    backend::ErrorCode::invalid_argument, "not on the advertised increment 100");
    CHECK(profile::plan_writes({512, 512, 0, 0, 25, 1'010'000ns}, stepped, open_time));
    for (const double bad : {0.0, -100.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
        auto malformed = open_time;
        malformed.frame_rate_hz.increment = bad;
        expect_rejected(profile::plan_writes({512, 512, 0, 0, 25, 1ms}, malformed, open_time), backend::ErrorCode::unsupported,
                        "AcquisitionFrameRate advertises invalid increment");
    }

    // Coupled current ROI maxima (maximum = sensor - other feature) are not
    // final bounds: both transition directions still plan.
    auto small = open_time;
    small.width = {64, 1, 2048 - 1900, 1};
    small.offset_x = {1900, 0, 2048 - 64, 1};
    CHECK((order(profile::plan_writes({2048, 512, 0, 0, 25, 1ms}, small, open_time).value()) ==
           std::vector<std::string>{"OffsetX", "Width", "ExposureTimeAbs", "AcquisitionFrameRate"}));
    auto large = open_time;
    large.width = {2048, 1, 2048, 1};
    large.offset_x = {0, 0, 0, 1};
    CHECK((order(profile::plan_writes({64, 512, 1900, 0, 25, 1ms}, large, open_time).value()) ==
           std::vector<std::string>{"Width", "OffsetX", "ExposureTimeAbs", "AcquisitionFrameRate"}));
}

// R2 (finding A): a fresh ROI maximum is enforced before any write unless an
// EARLIER write of the same plan DECREASES its companion (the only supported
// way the coupled maximum can be raised). Snapshot-level profile regressions.
void roi_maxima_by_write_order() {
    const auto open_time = snapshot_model(); // 2048x2048 sensor, 512x512 @ (0,0)
    // A1: fresh Height maximum 512, OffsetY unchanged: Height 640 rejected
    //     (R1 accepted Width=640, Height=640, ExposureTimeAbs, AcquisitionFrameRate).
    auto a1 = open_time;
    a1.height.maximum = 512;
    expect_rejected(profile::plan_writes({640, 640, 0, 0, 25, 1ms}, a1, open_time), backend::ErrorCode::invalid_argument,
                    "Height 640 is above the fresh native maximum 512; no earlier write of this request decreases OffsetY");
    CHECK(profile::plan_writes({640, 512, 0, 0, 25, 1ms}, a1, open_time)); // at the maximum: allowed
    // A2: fresh OffsetY maximum 4, Height unchanged: OffsetY 8 rejected
    //     (R1 accepted Width=640, OffsetY=8, ExposureTimeAbs, AcquisitionFrameRate).
    auto a2 = open_time;
    a2.offset_y.maximum = 4;
    expect_rejected(profile::plan_writes({640, 512, 0, 8, 25, 1ms}, a2, open_time), backend::ErrorCode::invalid_argument,
                    "OffsetY 8 is above the fresh native maximum 4; no earlier write of this request decreases Height");
    CHECK(profile::plan_writes({640, 512, 0, 4, 25, 1ms}, a2, open_time));
    // A known-invalid Y request never yields a plan containing the X writes.
    CHECK(!profile::plan_writes({1024, 640, 16, 0, 25, 1ms}, a1, open_time));

    // A companion change that cannot relax the bound gets no exemption:
    // increasing OffsetY (written first, since it fits) cannot raise Height's maximum.
    auto rising = open_time;
    rising.height.maximum = 512;
    rising.offset_y.value = 0;
    expect_rejected(profile::plan_writes({512, 640, 0, 16, 25, 1ms}, rising, open_time), backend::ErrorCode::invalid_argument,
                    "Height 640 is above the fresh native maximum 512");
    // ... nor does a companion DECREASE that is written AFTER the bounded
    // feature: OffsetY 8 fits the current Height, so it is written first and
    // the Height decrease (512 -> 256) only follows it.
    auto later = open_time;
    later.offset_y.maximum = 4;
    expect_rejected(profile::plan_writes({512, 256, 0, 8, 25, 1ms}, later, open_time), backend::ErrorCode::invalid_argument,
                    "OffsetY 8 is above the fresh native maximum 4; no earlier write of this request decreases Height");

    // Deferred ONLY where a preceding write decreases the companion. The plan
    // makes that preceding bound-relaxing write visible.
    auto coupled = open_time; // 64 wide at OffsetX 1900: Width.max = 148, OffsetX.max = 1984
    coupled.width = {64, 1, 2048 - 1900, 1};
    coupled.offset_x = {1900, 0, 2048 - 64, 1};
    const auto grow = profile::plan_writes({2048, 512, 0, 0, 25, 1ms}, coupled, open_time);
    CHECK((order(grow.value()) == std::vector<std::string>{"OffsetX", "Width", "ExposureTimeAbs", "AcquisitionFrameRate"}));
    CHECK(grow.value()[0].integer == 0 && grow.value()[1].integer == 2048); // OffsetX 1900 -> 0 precedes Width 2048 > 148
    auto wide = open_time; // 2048 wide at 0: OffsetX.max = 0, Width.max = 2048
    wide.width = {2048, 1, 2048, 1};
    wide.offset_x = {0, 0, 0, 1};
    const auto shrink = profile::plan_writes({64, 512, 1900, 0, 25, 1ms}, wide, open_time);
    CHECK((order(shrink.value()) == std::vector<std::string>{"Width", "OffsetX", "ExposureTimeAbs", "AcquisitionFrameRate"}));
    CHECK(shrink.value()[0].integer == 64 && shrink.value()[1].integer == 1900); // Width 2048 -> 64 precedes OffsetX 1900 > 0

    // Current-state facts stay separate from request legality: a current value
    // above a newly lowered maximum is reported and recoverable, and an
    // unchanged feature is not judged against its new maximum.
    auto above = open_time;
    above.height = {1024, 1, 512, 1}; // current 1024 > fresh maximum 512
    CHECK(profile::configuration_from(above).value().height == 1024);
    CHECK((order(profile::plan_writes({640, 512, 0, 0, 25, 1ms}, above, open_time).value()) ==
           std::vector<std::string>{"Width", "Height", "ExposureTimeAbs", "AcquisitionFrameRate"}));
    CHECK(profile::plan_writes({640, 1024, 0, 0, 25, 1ms}, above, open_time)); // Height unchanged: not written
}

// Bounded gate: the owner opens it on every unwind path.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false}, opened{false};
    void enter_and_wait() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        if (!changed.wait_for(lock, 10s, [&] { return opened; })) throw std::runtime_error("gate expired");
    }
    void wait_entered() {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 5s, [&] { return entered; }));
    }
    void open() {
        { std::lock_guard lock(mutex); opened = true; }
        changed.notify_all();
    }
};

// ===========================================================================
void construction_and_discovery() {
    const int before = interface_requests.load();
    auto camera = aravis::make_backend_for_interface({"Fake", &counting_fake});
    CHECK(camera && camera->id() == "aravis");
    CHECK(interface_requests.load() == before); // construction: no interface, discovery or open
    auto gige = aravis::make_backend();          // ordinary GigE factory: also no I/O
    CHECK(gige && gige->id() == "aravis");

    auto first = camera->enumerate();
    CHECK(first && interface_requests.load() == before + 1);
    CHECK(first.value().size() == 1);
    const auto& entry = first.value().front();
    CHECK(entry.backend_id == "aravis" && entry.device_id == "Fake_1");
    CHECK(contains(entry.display_name, "Aravis") && contains(entry.display_name, "Fake"));
    const std::string kept_id = entry.device_id, kept_name = entry.display_name;
    // Another refresh frees Aravis's previous strings; the copies stay valid
    // (ASan would report a borrowed pointer here).
    auto second = camera->enumerate();
    CHECK(second && interface_requests.load() == before + 1); // interface retained, not re-requested
    CHECK(entry.device_id == kept_id && entry.display_name == kept_name);
    CHECK(second.value().front().device_id == kept_id);

    // Empty successful enumeration differs from an SDK failure.
    auto empty = aravis::make_backend_for_interface({"TestEmpty", &empty_interface})->enumerate();
    CHECK(empty && empty.value().empty());
    auto missing = aravis::make_backend_for_interface({"TestMissing", &missing_interface})->enumerate();
    CHECK(!missing && missing.error().code == backend::ErrorCode::io_error);
    CHECK(contains(missing.error().message, "TestMissing"));
    auto missing_open = aravis::make_backend_for_interface({"TestMissing", &missing_interface})->open("x");
    CHECK(!missing_open && missing_open.error().code == backend::ErrorCode::io_error);
}

void exact_selection() {
    auto observed = std::make_shared<NativeOpens>();
    auto camera = aravis_fake::make_backend(observed->hooks());
    const auto empty = camera->open("");
    CHECK(!empty && empty.error().code == backend::ErrorCode::invalid_argument);
    // Unknown IDs, the Fake's serial ("1") and address are not substituted.
    for (const char* other : {"Fake_2", "fake_1", "1", "0.0.0.0", "Aravis-Fake-1", "Fake_1 "}) {
        const auto opened = camera->open(other);
        CHECK(!opened && opened.error().code == backend::ErrorCode::not_found);
        CHECK(contains(opened.error().message, std::string("'") + other + "'"));
        CHECK(contains(opened.error().message, "no other device or alias was tried"));
    }
    CHECK(observed->opened == 0); // nothing was natively opened
    auto device = open_fake(*camera);
    CHECK(observed->opened == 1 && observed->live() == 1);
    CHECK(device->configuration().value() == fake_initial);
    CHECK(device->close());
    CHECK(observed->live() == 0); // logical close released the native device
}

void ownership_and_failed_open() {
    auto observed = std::make_shared<NativeOpens>();
    auto a = aravis_fake::make_backend(observed->hooks());
    auto b = aravis_fake::make_backend(observed->hooks());
    auto device = open_fake(*a);
    // Same adapter and a separate adapter instance are both refused.
    for (auto* camera : {a.get(), b.get()}) {
        const auto again = camera->open(aravis_fake::device_id);
        CHECK(!again && again.error().code == backend::ErrorCode::busy);
        CHECK(contains(again.error().message, "already open"));
    }
    CHECK(observed->opened == 1); // the refused opens never reached the SDK
    CHECK(device->close());
    auto reopened = open_fake(*b); // released on close: another adapter may open
    CHECK(reopened->close() && observed->live() == 0);

    // A throwing step after reservation releases it (exception safety).
    aravis::TestHooks throwing;
    throwing.after_reservation = [](std::string_view) { throw std::runtime_error("injected after reservation"); };
    auto thrower = aravis_fake::make_backend(throwing);
    bool threw = false;
    try { (void)thrower->open(aravis_fake::device_id); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    // A failed profile validation (real native Mono16 arranged through the
    // SDK before validation) releases native reference and reservation.
    auto unsupported_hooks = observed->hooks();
    unsupported_hooks.after_native_open = [observed](ArvDevice* d) {
        observed->opened.fetch_add(1);
        observed->retired.watch(d);
        GError* error = nullptr;
        arv_device_set_string_feature_value(d, "PixelFormat", "Mono16", &error);
        if (error) { g_error_free(error); throw std::runtime_error("could not arrange Mono16"); }
    };
    auto refusing = aravis_fake::make_backend(unsupported_hooks);
    const auto refused = refusing->open(aravis_fake::device_id);
    CHECK(!refused && refused.error().code == backend::ErrorCode::unsupported);
    CHECK(contains(refused.error().message, "Mono16") && contains(refused.error().message, "unsupported camera profile"));
    CHECK(observed->live() == 0);
    auto recovered = open_fake(*b); // subsequent successful open (fresh Fake device: Mono8)
    CHECK(recovered->configuration().value() == fake_initial);
    CHECK(recovered->close() && observed->live() == 0);
}

// Two adapters race to open the same ID. Evidence, at two boundaries:
//  1. Operation overlap: the first opener is held by a gate INSIDE open(),
//     after its ownership reservation and with the per-interface lock held.
//  2. Lock contention: the competitor's open performed a real try_lock of
//     that same lock and it FAILED (observed through a private hook placed at
//     the locking boundary), i.e. the competitor reached the lock while the
//     first open was still mid-open. Only then is the first opener released.
// No sleep is used as evidence; every wait is bounded. Exclusivity: one
// owner, the competitor is busy, and only one native open happens.
void concurrent_open_is_serialized() {
    Gate held_in_open, contended;
    auto opens = std::make_shared<NativeOpens>();
    aravis::TestHooks first_hooks = opens->hooks();
    first_hooks.after_reservation = [&held_in_open](std::string_view) { held_in_open.enter_and_wait(); };
    aravis::TestHooks second_hooks = opens->hooks();
    second_hooks.interface_lock_contended = [&contended](std::string_view) {
        { std::lock_guard lock(contended.mutex); contended.entered = true; }
        contended.changed.notify_all(); // observation only; never blocks the competitor
    };
    auto first = aravis_fake::make_backend(first_hooks);
    auto second = aravis_fake::make_backend(second_hooks);
    using Pending = std::future<backend::Result<std::shared_ptr<backend::ICameraDevice>>>;
    Pending pending, competitor;
    // Every unwind path releases the gate BEFORE waiting on the futures (and
    // before the backends, whose devices the futures may own, are destroyed).
    struct Release { Gate& g; Pending& a; Pending& b;
                     ~Release() { g.open(); if (a.valid()) a.wait(); if (b.valid()) b.wait(); } } cleanup{held_in_open, pending, competitor};
    pending = std::async(std::launch::async, [&] { return first->open(aravis_fake::device_id); });
    held_in_open.wait_entered(); // (1) the first opener is inside open(), holding the lock
    competitor = std::async(std::launch::async, [&] { return second->open(aravis_fake::device_id); });
    contended.wait_entered();    // (2) the competitor's real try_lock failed
    CHECK(pending.wait_for(0s) == std::future_status::timeout); // the first is still mid-open
    CHECK(opens->opened == 0);                                   // no native open has happened yet
    held_in_open.open();
    CHECK(pending.wait_for(10s) == std::future_status::ready);
    auto winner = pending.get();
    CHECK(winner);
    CHECK(competitor.wait_for(10s) == std::future_status::ready);
    const auto loser = competitor.get();
    CHECK(!loser && loser.error().code == backend::ErrorCode::busy);
    CHECK(opens->opened == 1); // the competitor never reached the native open
    CHECK(winner.value()->close());
}

void capabilities_and_configuration() {
    auto camera = aravis_fake::make_backend();
    auto device = open_fake(*camera);
    const auto caps = device->capabilities(); // real native reads during open
    CHECK(caps.sensor_width == 2048 && caps.sensor_height == 2048);
    CHECK(caps.width_increment == 1 && caps.height_increment == 1 && caps.offset_x_increment == 1 && caps.offset_y_increment == 1);
    CHECK(caps.minimum_frame_rate_hz == 1 && caps.maximum_frame_rate_hz == 1000); // native [0.1, 1000] Hz
    CHECK(caps.minimum_exposure == 10'000ns && caps.mono8 && !caps.external_trigger);
    CHECK(device->configuration().value() == fake_initial);
    const auto before = unrelated(*device);

    // Two valid configurations: ROI size and both offsets change.
    const backend::CameraConfiguration a{640, 480, 16, 8, 40, 1'250'000ns};
    const backend::CameraConfiguration b{1024, 768, 0, 2, 50, 2'000'000ns};
    CHECK(device->apply(a).value() == a);
    CHECK(device->configuration().value() == a);
    CHECK(native_integer(*device, "OffsetX") == 16 && native_integer(*device, "AcquisitionFramePeriod") == 25'000);
    CHECK(device->apply(b).value() == b);
    CHECK(device->configuration().value() == b);
    // Small/far-offset <-> large/near-origin in both directions.
    const backend::CameraConfiguration far{64, 32, 1984, 2016, 25, 1ms};
    const backend::CameraConfiguration full{2048, 2048, 0, 0, 1, 1ms};
    CHECK(device->apply(far).value() == far);
    CHECK(device->apply(full).value() == full);
    CHECK(device->apply(far).value() == far);
    CHECK(unrelated(*device) == before); // no trigger/format/gain/binning side effects

    // Bounds and increment violations write nothing.
    const auto period = native_integer(*device, "AcquisitionFramePeriod");
    for (const backend::CameraConfiguration& invalid :
         {backend::CameraConfiguration{2049, 64, 0, 0, 25, 1ms}, backend::CameraConfiguration{64, 64, 1985, 0, 25, 1ms},
          backend::CameraConfiguration{0, 64, 0, 0, 25, 1ms}, backend::CameraConfiguration{64, 64, 0, 0, 0, 1ms},
          backend::CameraConfiguration{64, 64, 0, 0, 1001, 1ms}, backend::CameraConfiguration{64, 64, 0, 0, 25, 9us},
          backend::CameraConfiguration{64, 64, 0, 0, 25, 10'000'001'000ns}}) {
        const auto rejected = device->apply(invalid);
        CHECK(!rejected && rejected.error().code == backend::ErrorCode::invalid_argument);
        CHECK(contains(rejected.error().message, "nothing was written"));
        CHECK(device->configuration().value() == far);
        CHECK(native_integer(*device, "AcquisitionFramePeriod") == period);
    }

    // apply() requires the manual/free-running profile and never normalizes
    // it: with TriggerMode On (arranged through the SDK) it writes nothing.
    set_native_string(*device, "TriggerMode", "On");
    const auto triggered = device->apply(a);
    CHECK(!triggered && triggered.error().code == backend::ErrorCode::unsupported);
    CHECK(contains(triggered.error().message, "TriggerMode is 'On'"));
    CHECK(native_string(*device, "TriggerMode") == "On" && native_integer(*device, "Width") == 64);
    CHECK(!device->configuration()); // profile no longer satisfied: explicit error
    set_native_string(*device, "TriggerMode", "Off");
    CHECK(device->configuration().value() == far);
    CHECK(device->close());
}

// Pinned Aravis 0.8.36 Fake behavior: AcquisitionFrameRate = 1e6 / integer
// period (us); ExposureTimeAbs is an integer microsecond register.
void quantization_and_truthful_readback() {
    auto camera = aravis_fake::make_backend();
    auto device = open_fake(*camera);
    for (std::uint32_t exact : {25U, 40U, 50U, 1U, 1000U}) {
        const backend::CameraConfiguration c{512, 512, 0, 0, exact, 1ms};
        CHECK(device->apply(c).value() == c);
    }
    // Exposure quantization: 1250.5 us is accepted natively but stored as
    // 1250 us. The actual value is returned, not the request.
    const backend::CameraConfiguration fine{512, 512, 0, 0, 25, 1'250'500ns};
    const auto quantized = device->apply(fine);
    CHECK(quantized && quantized.value().exposure == 1'250'000ns && quantized.value() != fine);
    CHECK(device->configuration().value().exposure == 1'250'000ns);

    // 30 Hz: period 33333 us reads back as 30.000300003000032 Hz, which is
    // unrepresentable. Explicit error naming the native value and feature.
    const auto thirty = device->apply({800, 600, 8, 4, 30, 2ms});
    CHECK(!thirty && thirty.error().code == backend::ErrorCode::unsupported);
    CHECK(contains(thirty.error().message, "AcquisitionFrameRate reads back 30.00030000300003"));
    CHECK(native_integer(*device, "AcquisitionFramePeriod") == 33'333); // the writes did happen
    CHECK(native_integer(*device, "Width") == 800);
    const auto readback = device->configuration();
    CHECK(!readback && contains(readback.error().message, "30.00030000300003"));

    // State arranged directly through the SDK: 7 Hz -> 142857 us period.
    set_native_integer(*device, "AcquisitionFramePeriod", 142'857);
    CHECK(contains(device->configuration().error().message, "7.0000070000"));
    // Explicit recovery with an exactly representable configuration.
    const backend::CameraConfiguration recovery{800, 600, 8, 4, 40, 2ms};
    CHECK(device->apply(recovery).value() == recovery);
    CHECK(device->configuration().value() == recovery);
    CHECK(device->close());
}

// Names the attempted native writes when the "no write" contract is broken.
void expect_no_writes(const std::vector<std::string>& attempted) {
    if (attempted.empty()) return;
    std::string list;
    for (const auto& feature : attempted) list += (list.empty() ? "" : ", ") + feature;
    throw std::runtime_error("expected no native write attempt, but attempted: " + list);
}

// Genuine late failure after earlier writes succeeded. Prevalidation passes
// against the fresh snapshot; only AFTER it, immediately before the exposure
// write, a private native-boundary hook lowers the Fake's ExposureTimeAbs
// maximum through the real SDK (arv_gc_float_impose_max), modelling a native
// constraint that changes after prevalidation. The SDK's own range check then
// rejects the write. This is an arranged SDK constraint change, not a physical
// camera fault.
void late_write_failure_and_recovery() {
    std::atomic<bool> armed{false};
    std::vector<std::string> attempted;
    aravis::TestHooks hooks;
    hooks.before_write = [&](std::string_view feature, ArvDevice* d) {
        attempted.emplace_back(feature);
        if (feature == "ExposureTimeAbs" && armed.exchange(false)) impose_exposure_max(d, 500.0);
    };
    auto camera = aravis_fake::make_backend(hooks);
    auto device = open_fake(*camera);
    const auto before = unrelated(*device);
    armed = true;
    // Rate 20 Hz (period lengthens: rate first), then exposure 2 ms.
    const auto failed = device->apply({320, 240, 32, 16, 20, 2ms});
    CHECK(!failed && failed.error().code == backend::ErrorCode::invalid_argument);
    const auto& message = failed.error().message;
    CHECK(contains(message, "ExposureTimeAbs") && contains(message, "greater than allowed maximum")); // original SDK error
    CHECK(contains(message, "earlier successful writes: OffsetX=32, Width=320, OffsetY=16, Height=240"));
    CHECK(contains(message, "AcquisitionFrameRate=20") && contains(message, "no rollback was attempted"));
    CHECK(contains(message, "actual native configuration: 320x240@(32,16) 20 Hz 10000000 ns"));
    CHECK((attempted == std::vector<std::string>{"OffsetX", "Width", "OffsetY", "Height", "AcquisitionFrameRate", "ExposureTimeAbs"}));
    // Actual native state remains observable and is NOT the request.
    const backend::CameraConfiguration partial{320, 240, 32, 16, 20, 10ms};
    CHECK(device->configuration().value() == partial);
    CHECK(unrelated(*device) == before);
    // The reduced maximum is now part of every fresh snapshot: a retry that
    // violates it fails in prevalidation, before ANY additional write attempt.
    attempted.clear();
    const auto retry = device->apply({320, 240, 32, 16, 20, 1ms});
    expect_no_writes(attempted);
    CHECK(!retry && retry.error().code == backend::ErrorCode::invalid_argument);
    CHECK(contains(retry.error().message, "ExposureTimeAbs 1000 us is outside the native bounds [10, 500]"));
    CHECK(contains(retry.error().message, "nothing was written"));
    CHECK(device->configuration().value() == partial);
    // Explicit recovery to a representable configuration within it.
    const backend::CameraConfiguration recovery{320, 240, 32, 16, 20, 400us};
    CHECK(device->apply(recovery).value() == recovery);
    CHECK(device->close());
}

// R1 (finding B) through the real SDK: constraints already visible in the
// fresh native snapshot are enforced before the FIRST write attempt.
struct WriteAttempts {
    std::vector<std::string> attempted;
    aravis::TestHooks hooks() {
        aravis::TestHooks h;
        h.before_write = [this](std::string_view feature, ArvDevice*) { attempted.emplace_back(feature); };
        return h;
    }
};

// 1. Already-known exposure limit. Open-time bounds allow 1 ms; then the
//    current exposure is set to 400 us and the maximum lowered to 500 us
//    through the SDK. The rate stays 25 Hz, so no rate/exposure coupling can
//    explain the rejection.
void fresh_exposure_limit_before_writes() {
    WriteAttempts writes;
    auto& attempted = writes.attempted;
    auto camera = aravis_fake::make_backend(writes.hooks());
    {
        auto device = open_fake(*camera);
        CHECK(device->capabilities().minimum_exposure == 10'000ns);
        const backend::CameraConfiguration current{512, 512, 0, 0, 25, 400us};
        CHECK(device->apply(current).value() == current);
        native(*device, [](ArvDevice* d) { impose_exposure_max(d, 500.0); });
        const auto before = unrelated(*device);
        const auto period = native_integer(*device, "AcquisitionFramePeriod");
        attempted.clear();
        const auto rejected = device->apply({640, 480, 16, 8, 25, 1ms}); // different ROI, same rate
        expect_no_writes(attempted); // checked first: names any write that happened
        CHECK(!rejected && rejected.error().code == backend::ErrorCode::invalid_argument);
        CHECK(contains(rejected.error().message, "ExposureTimeAbs 1000 us is outside the native bounds [10, 500]"));
        CHECK(device->configuration().value() == current);
        CHECK(native_integer(*device, "AcquisitionFramePeriod") == period);
        CHECK(native_integer(*device, "ExposureTimeAbsRegister") == 400); // integer us register behind the Float
        CHECK(unrelated(*device) == before);
        CHECK(device->close());
    }
}

// 2. Fresh Width grid 8 + k*8 (current 512), opened with 1 + k*1. Width 641 is
//    not accepted on the obsolete grid: explicit profile-change error.
void fresh_width_grid_before_writes() {
    WriteAttempts writes;
    auto& attempted = writes.attempted;
    const auto hooks = writes.hooks();
    auto camera = aravis_fake::make_backend(hooks);
    {
        auto device = open_fake(*camera);
        CHECK(device->capabilities().width_increment == 1);
        native(*device, [](ArvDevice* d) { set_integer_grid(d, "Width", 8, 8); });
        CHECK(native_integer(*device, "Width") == 512);
        const auto before = unrelated(*device);
        attempted.clear();
        const auto rejected = device->apply({641, 512, 0, 0, 25, 1ms});
        expect_no_writes(attempted);
        CHECK(!rejected && rejected.error().code == backend::ErrorCode::unsupported);
        CHECK(contains(rejected.error().message, "Width increment changed from 1 at open to 8"));
        CHECK(device->configuration().value() == fake_initial);
        CHECK(unrelated(*device) == before);
        // The published capabilities remain the open-time snapshot (immutable).
        CHECK(device->capabilities().width_increment == 1);
        CHECK(device->close());
    }
    // A reopen publishes the fresh grid; 641 is then rejected on it.
    {
        aravis::TestHooks regrid = hooks;
        regrid.after_native_open = [](ArvDevice* d) { set_integer_grid(d, "Width", 8, 8); };
        auto reopened_camera = aravis_fake::make_backend(regrid);
        auto device = open_fake(*reopened_camera);
        CHECK(device->capabilities().width_increment == 8);
        attempted.clear();
        const auto rejected = device->apply({641, 512, 0, 0, 25, 1ms});
        expect_no_writes(attempted);
        CHECK(!rejected && contains(rejected.error().message, "Width 641 is not on the native grid 8 + k*8"));
        const backend::CameraConfiguration on_grid{640, 512, 0, 0, 25, 1ms};
        CHECK(device->apply(on_grid).value() == on_grid);
        CHECK(device->close());
    }
}

// 3. Unsafe native ROI facts arranged through the SDK: zero writes.
void unsafe_native_roi_before_writes() {
    WriteAttempts writes;
    auto& attempted = writes.attempted;
    auto camera = aravis_fake::make_backend(writes.hooks());
    {
        auto device = open_fake(*camera);
        native(*device, [](ArvDevice* d) { impose_integer_min(d, "Width", std::numeric_limits<std::int64_t>::min()); });
        attempted.clear();
        const auto rejected = device->apply({640, 480, 16, 8, 25, 1ms});
        expect_no_writes(attempted);
        CHECK(!rejected && rejected.error().code == backend::ErrorCode::unsupported);
        CHECK(contains(rejected.error().message, "Width advertises minimum -9223372036854775808"));
        CHECK(device->configuration().value() == fake_initial);
        CHECK(device->close());
    }
}

// Adapter whose every native open is counted/weak-referenced and then
// arranged through the real SDK before profile validation.
std::shared_ptr<backend::ICameraBackend> arranged_backend(const std::shared_ptr<NativeOpens>& observed,
                                                          std::function<void(ArvDevice*)> arrange,
                                                          std::vector<std::string>* attempted = nullptr) {
    aravis::TestHooks hooks = observed->hooks();
    hooks.after_native_open = [observed, arrange = std::move(arrange)](ArvDevice* d) {
        observed->opened.fetch_add(1);
        observed->retired.watch(d);
        arrange(d);
    };
    if (attempted) hooks.before_write = [attempted](std::string_view f, ArvDevice*) { attempted->emplace_back(f); };
    return aravis_fake::make_backend(hooks);
}

// R2 (finding A) through the real SDK: a known fresh ROI maximum, with an
// unrelated Width change in the same request, is rejected before the FIRST
// write. Each case uses a freshly opened Fake device (a new GenICam tree).
void known_roi_maxima_before_writes() {
    struct Case {
        const char* bounded;
        std::int64_t maximum;
        backend::CameraConfiguration request;
        const char* message;
    };
    const Case cases[] = {
        {"Height", 512, {640, 640, 0, 0, 25, 1ms}, "Height 640 is above the fresh native maximum 512"},     // A1
        {"OffsetY", 4, {640, 512, 0, 8, 25, 1ms}, "OffsetY 8 is above the fresh native maximum 4"},         // A2
        {"Height", 512, {640, 640, 0, 16, 25, 1ms}, "Height 640 is above the fresh native maximum 512"},    // OffsetY rises: no relaxation
    };
    for (const auto& c : cases) {
        WriteAttempts writes;
        auto camera = aravis_fake::make_backend(writes.hooks());
        auto device = open_fake(*camera);
        native(*device, [&](ArvDevice* d) { gc_set_integer_max(d, c.bounded, c.maximum); });
        // Actual SDK current values and maximum before the request.
        CHECK(native_integer(*device, "Height") == 512 && native_integer(*device, "OffsetY") == 0);
        CHECK(native_integer(*device, "Width") == 512);
        const auto before = unrelated(*device);
        const auto period = native_integer(*device, "AcquisitionFramePeriod");
        const auto rejected = device->apply(c.request);
        expect_no_writes(writes.attempted); // not even the unrelated Width write
        CHECK(!rejected && rejected.error().code == backend::ErrorCode::invalid_argument);
        if (!contains(rejected.error().message, c.message))
            throw std::runtime_error("expected '" + std::string(c.message) + "' in: " + rejected.error().message);
        CHECK(device->configuration().value() == fake_initial);
        CHECK(native_integer(*device, "AcquisitionFramePeriod") == period);
        CHECK(native_integer(*device, "ExposureTimeAbsRegister") == 10'000);
        CHECK(unrelated(*device) == before);
        CHECK(device->close());
    }
}

// R2 (finding B) through the real SDK: an explicit invalid <Inc> on the
// exposure feature is preserved by extraction and rejected; the documented
// absence value is not. Every arrangement first proves the RAW SDK result.
void float_increment_extraction() {
    // Genuine absence: the Fake's ExposureTimeAbs has no <Inc>, and its linked
    // converter is not linear, so the SDK returns exactly G_MINDOUBLE.
    {
        auto camera = aravis_fake::make_backend();
        auto device = open_fake(*camera);
        bool failed = true;
        native(*device, [&](ArvDevice* d) { CHECK(sdk_float_inc(d, "ExposureTimeAbs", failed) == G_MINDOUBLE); });
        CHECK(!failed);
        const backend::CameraConfiguration fine{512, 512, 0, 0, 25, 1'250'500ns}; // no grid: SDK quantizes to 1250 us
        CHECK(device->apply(fine).value().exposure == 1'250'000ns);
        CHECK(device->close());
    }
    // Invalid explicit increments rejected during open; cleanup, then a clean open.
    // Pinned-SDK facts (asserted, not assumed): <Inc>0</Inc> -> 0, -5 -> -5,
    // nan -> NaN, inf -> +inf, each with no GError (g_ascii_strtod parsing).
    for (const char* text : {"0", "-5", "nan", "inf"}) {
        auto observed = std::make_shared<NativeOpens>();
        double raw = 1.0;
        auto camera = arranged_backend(observed, [&](ArvDevice* d) { raw = gc_set_float_inc(d, "ExposureTimeAbs", text); });
        const auto refused = camera->open(aravis_fake::device_id);
        const std::string t(text);
        const bool expected_raw = t == "nan"   ? std::isnan(raw)
                                  : t == "inf" ? raw == std::numeric_limits<double>::infinity()
                                               : raw == std::stod(t);
        CHECK(expected_raw); // the pinned SDK returned exactly this value, with no GError
        CHECK(!refused && refused.error().code == backend::ErrorCode::unsupported);
        if (!contains(refused.error().message, "ExposureTimeAbs advertises invalid increment"))
            throw std::runtime_error(std::string("Inc ") + text + ": " + refused.error().message);
        CHECK(observed->live() == 0);
        auto clean = open_fake(*aravis_fake::make_backend()); // reservation released
        CHECK(clean->close());
    }
    // Already-open apply after a fresh invalid increment: the same exposure
    // feature stays selected, so only the increment explains the rejection.
    {
        WriteAttempts writes;
        auto camera = aravis_fake::make_backend(writes.hooks());
        auto device = open_fake(*camera);
        double raw = 1.0;
        native(*device, [&](ArvDevice* d) { raw = gc_set_float_inc(d, "ExposureTimeAbs", "0"); });
        CHECK(raw == 0.0);
        const auto rejected = device->apply({640, 480, 16, 8, 40, 2ms});
        expect_no_writes(writes.attempted);
        CHECK(!rejected && rejected.error().code == backend::ErrorCode::unsupported);
        CHECK(contains(rejected.error().message, "ExposureTimeAbs advertises invalid increment 0 us"));
        CHECK(!contains(rejected.error().message, "changed from"));
        CHECK(native_integer(*device, "Width") == 512 && native_integer(*device, "AcquisitionFramePeriod") == 40'000);
        CHECK(device->close());
    }
    // A valid explicit increment reaches the profile and its grid is enforced.
    {
        WriteAttempts writes;
        auto camera = aravis_fake::make_backend(writes.hooks());
        auto device = open_fake(*camera);
        double raw = 0.0;
        native(*device, [&](ArvDevice* d) { raw = gc_set_float_inc(d, "ExposureTimeAbs", "100"); });
        CHECK(raw == 100.0);
        const auto off_grid = device->apply({512, 512, 0, 0, 25, 1'050'000ns}); // 10 + k*100 us grid
        expect_no_writes(writes.attempted);
        CHECK(!off_grid && contains(off_grid.error().message, "not on the advertised increment 100"));
        const backend::CameraConfiguration on_grid{512, 512, 0, 0, 25, 1'010'000ns};
        CHECK(device->apply(on_grid).value() == on_grid);
        CHECK(device->close());
    }
}

// Optional Boolean (AcquisitionFrameRateEnable): implemented is read; a failed
// query is an error on the already-open read path; a successfully reported
// "not implemented" is supported absence.
void optional_boolean_query_errors() {
    auto observed = std::make_shared<NativeOpens>();
    {
        auto device = open_fake(*arranged_backend(observed, [](ArvDevice* d) { gc_add_boolean(d, "AcquisitionFrameRateEnable", true); }));
        CHECK(device->configuration().value() == fake_initial); // enabled: supported
        native(*device, [](ArvDevice* d) { (void)break_implementation_query(d, "AcquisitionFrameRateEnable"); });
        const auto read = device->configuration();
        CHECK(!read && contains(read.error().message, "optional feature 'AcquisitionFrameRateEnable'"));
        CHECK(device->close());
    }
    const auto disabled = arranged_backend(observed, [](ArvDevice* d) { gc_add_boolean(d, "AcquisitionFrameRateEnable", false); })
                              ->open(aravis_fake::device_id);
    CHECK(!disabled && contains(disabled.error().message, "AcquisitionFrameRateEnable is false"));
    {
        auto device = open_fake(*arranged_backend(observed, [](ArvDevice* d) {
            gc_add_boolean(d, "AcquisitionFrameRateEnable", false);
            gc_set_unimplemented(d, "AcquisitionFrameRateEnable"); // SDK: not implemented, no GError
        }));
        CHECK(device->configuration().value() == fake_initial);
        CHECK(device->close());
    }
    CHECK(observed->live() == 0);
}

// Preferred exposure feature: a failed ExposureTime query must not fall back to
// ExposureTimeAbs. Implemented and successfully unimplemented ExposureTime
// work. Also: a node of an unsuitable type, and read-only inspection.
void exposure_selection_query_errors() {
    auto observed = std::make_shared<NativeOpens>();
    const auto broken = arranged_backend(observed, [](ArvDevice* d) {
        gc_add_float(d, "ExposureTime", "1000.0");
        (void)break_implementation_query(d, "ExposureTime");
    })->open(aravis_fake::device_id);
    CHECK(!broken && contains(broken.error().message, "optional feature 'ExposureTime'"));
    {
        auto device = open_fake(*arranged_backend(observed, [](ArvDevice* d) { gc_add_float(d, "ExposureTime", "1000.0"); }));
        CHECK(device->configuration().value().exposure == 1ms); // ExposureTime, not ExposureTimeAbs (10 ms)
        CHECK(device->close());
    }
    {
        auto device = open_fake(*arranged_backend(observed, [](ArvDevice* d) {
            gc_add_float(d, "ExposureTime", "1000.0");
            gc_set_unimplemented(d, "ExposureTime");
        }));
        CHECK(device->configuration().value() == fake_initial); // falls back only after a successful "no"
        CHECK(device->close());
    }

    // An existing node of an unsuitable type is diagnosed, not absent.
    const auto wrong_type = arranged_backend(observed, [](ArvDevice* d) {
        gc_add(d, gc_add(d, nullptr, "Integer", "ExposureAuto"), "Value", nullptr, "0");
    })->open(aravis_fake::device_id);
    CHECK(!wrong_type && contains(wrong_type.error().message, "'ExposureAuto' is not an enumeration or string"));

    // Read-only inspection reports a failed identity query as a problem.
    auto inspected = arranged_backend(observed, [](ArvDevice* d) { (void)break_implementation_query(d, "DeviceModelName"); });
    const auto inspection = aravis::inspect(*inspected, aravis_fake::device_id);
    CHECK(inspection);
    bool reported = false;
    for (const auto& problem : inspection.value().problems)
        reported = reported || contains(problem, "optional feature 'DeviceModelName'");
    CHECK(reported);
    CHECK(observed->live() == 0);
}

// R1 (finding C): a failed implementation query is an error, never absence.
// Every failure is a genuine GError from the pinned SDK evaluating its own
// GenICam tree (pIsImplemented -> IntReg -> nonexistent pPort); each
// arrangement first proves that arv_gc_feature_node_is_implemented() fails.
void optional_feature_query_errors() {
    auto observed = std::make_shared<NativeOpens>();
    std::vector<std::string> attempted;
    std::string sdk_message;

    // Mode/profile at open: TriggerMode = On, then its query is broken.
    auto trigger = arranged_backend(observed, [&](ArvDevice* d) {
        sdk_set_string(d, "TriggerMode", "On");
        sdk_message = break_implementation_query(d, "TriggerMode");
    }, &attempted);
    const auto refused = trigger->open(aravis_fake::device_id);
    CHECK(contains(sdk_message, "Port not found")); // the real SDK query failed
    CHECK(!refused);
    CHECK(contains(refused.error().message, "query implementation of optional feature 'TriggerMode'"));
    CHECK(contains(refused.error().message, sdk_message)); // translated SDK diagnostic retained
    CHECK(attempted.empty());                              // no configuration write
    CHECK(observed->live() == 0);                          // native object retired
    auto clean = aravis_fake::make_backend();              // reservation released
    auto reopened = open_fake(*clean);
    CHECK(reopened->configuration().value() == fake_initial && reopened->close());

    // Already-open read/apply path (optional string feature).
    {
        auto device = open_fake(*clean);
        native(*device, [](ArvDevice* d) { (void)break_implementation_query(d, "TriggerMode"); });
        const auto read = device->configuration();
        CHECK(!read && contains(read.error().message, "optional feature 'TriggerMode'"));
        const auto applied = device->apply({640, 480, 16, 8, 25, 1ms});
        CHECK(!applied && contains(applied.error().message, "optional feature 'TriggerMode'"));
        CHECK(native_integer(*device, "Width") == 512 && native_integer(*device, "OffsetX") == 0);
        CHECK(device->close());
    }
    CHECK(observed->live() == 0);
}

void close_destruction_and_streaming() {
    auto observed = std::make_shared<NativeOpens>();
    auto camera = aravis_fake::make_backend(observed->hooks());
    auto device = open_fake(*camera);
    const auto caps = device->capabilities();
    // GIGE-MEL-009: create_stream() returns a lightweight wrapper; it starts
    // no native acquisition (the Fake acquisition register stays 0) and the
    // wrapper may outlive close (its later start() is rejected). Full
    // streaming coverage lives in aravis_stream_contract.
    auto stream = device->create_stream();
    CHECK(stream && stream.value());
    native(*device, [](ArvDevice* d) {
        CHECK(arv_fake_camera_get_acquisition_status(arv_fake_device_get_fake_camera(ARV_FAKE_DEVICE(d))) == 0);
    });
    CHECK(observed->live() == 1);

    // Idempotent logical close; later I/O is rejected; capabilities() returns
    // the validated snapshot without device I/O.
    CHECK(device->close());
    CHECK(observed->live() == 0); // native device finalized at close (weak ref fired)
    CHECK(device->close());
    CHECK(device->configuration().error().code == backend::ErrorCode::disconnected);
    CHECK(device->apply(fake_initial).error().code == backend::ErrorCode::disconnected);
    CHECK(device->create_stream().error().code == backend::ErrorCode::disconnected);
    CHECK(stream.value()->start(1).error().code == backend::ErrorCode::disconnected);
    stream.value().reset();
    CHECK(!aravis::with_native_device_for_testing(*device, [](ArvDevice*) {}));
    const auto after = device->capabilities();
    CHECK(after.sensor_width == caps.sensor_width && after.maximum_frame_rate_hz == caps.maximum_frame_rate_hz);
    device.reset();

    // Destruction without close also retires native resources and the
    // reservation (a new open succeeds).
    auto second = open_fake(*camera);
    CHECK(observed->live() == 1);
    second.reset();
    CHECK(observed->live() == 0);
    auto third = open_fake(*camera);
    // A device may outlive its backend; the backend drops only its own
    // interface reference and never calls arv_shutdown().
    camera.reset();
    CHECK(third->configuration().value() == fake_initial);
    third.reset();
    CHECK(observed->live() == 0 && observed->opened == 3);
    CHECK(aravis_fake::make_backend()->enumerate()); // SDK still usable: no global shutdown
}

} // namespace

int main(int argc, char** argv) {
    std::cout << "Aravis runtime " << aravis::runtime_version() << " (compiled " << aravis::compiled_version()
              << ") loaded from " << aravis_fake::loaded_library_path() << '\n';
    if (aravis::runtime_version() != "0.8.36" || aravis::compiled_version() != "0.8.36") {
        std::cerr << "FAIL: the qualified Aravis 0.8.36 library is not the one loaded\n";
        return 1;
    }
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"checked numeric conversions", checked_conversions},
        {"profile translation, prevalidation and write order", profile_translation},
        {"native fact validation and overflow-safe planning", native_fact_validation},
        {"fresh native constraints in prevalidation", fresh_constraints},
        {"acquisition preflight profile table (SDK-free)", acquisition_preflight_profile},
        {"fresh ROI maxima by write order", roi_maxima_by_write_order},
        {"construction without I/O and copied discovery", construction_and_discovery},
        {"exact device selection", exact_selection},
        {"ownership and failed-open cleanup", ownership_and_failed_open},
        {"concurrent open across adapters", concurrent_open_is_serialized},
        {"real capabilities and two valid configurations", capabilities_and_configuration},
        {"SDK quantization and truthful readback", quantization_and_truthful_readback},
        {"late write failure and explicit recovery", late_write_failure_and_recovery},
        {"fresh SDK exposure limit enforced before the first write", fresh_exposure_limit_before_writes},
        {"fresh SDK Width grid enforced before the first write", fresh_width_grid_before_writes},
        {"unsafe SDK ROI facts rejected before the first write", unsafe_native_roi_before_writes},
        {"known SDK ROI maxima rejected before the first write", known_roi_maxima_before_writes},
        {"SDK float increment absence versus invalid values", float_increment_extraction},
        {"optional string feature query error is not absence", optional_feature_query_errors},
        {"optional Boolean feature query error is not absence", optional_boolean_query_errors},
        {"exposure feature query error does not fall back", exposure_selection_query_errors},
        {"close, destruction and lightweight stream wrapper", close_destruction_and_streaming},
    };
    std::size_t passed = 0;
    for (const auto& [name, run] : tests) {
        if (argc == 2 && std::string(name).find(argv[1]) == std::string::npos) continue;
        try {
            run();
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
        ++passed;
        std::cout << "PASS " << name << std::endl;
    }
    std::cout << passed << " Aravis adapter scenarios passed\n";
    return 0;
}
