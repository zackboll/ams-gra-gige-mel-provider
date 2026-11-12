// Direct adapter tests (GIGE-MEL-008) against the REAL Aravis 0.8.36 library
// and its in-process Fake interface (no network, no arv-fake-gv-camera, no
// physical camera). Feature reads/writes run through Aravis's own GenICam
// implementation of the Fake camera. Private hooks only arrange native state
// through that same SDK or observe lifetime; they do not replace SDK I/O.
#include "aravis_fake_support.hpp"

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
    CHECK(!opened.value()->capabilities().known_empty_defect_map);
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
// GIGE-MEL-019: the SDK-free receive-buffer assessment, with exact values.
// Linux reports twice the granted byte count (socket(7)).
void receive_buffer_assessment() {
    using S = aravis::ReceiveBufferState;
    const std::int64_t mib4 = aravis::gvsp_receive_buffer_request_bytes;
    CHECK(mib4 == 4194304);
    // Sufficient: the 4 MiB grant measured on the physical host at rmem_max 32 MiB.
    auto o = aravis::assess_receive_buffer(7, mib4, mib4, 8388608, 33554432, "");
    CHECK(o.state == S::sufficient && o.granted_bytes == mib4 && o.kernel_reported_bytes == 8388608);
    CHECK(o.stream_generation == 7 && o.rmem_max_bytes == 33554432 && o.sdk_property_bytes == mib4);
    CHECK(aravis::receive_buffer_warning(o).empty());
    // Exactly at the boundary is sufficient; one byte short is not.
    CHECK(aravis::assess_receive_buffer(1, 1000, 1000, 2000, std::nullopt, "").state == S::sufficient);
    CHECK(aravis::assess_receive_buffer(1, 1000, 1000, 1999, std::nullopt, "").state == S::insufficient);
    // A kernel value that only EQUALS the request is a half grant: insufficient.
    CHECK(aravis::assess_receive_buffer(1, mib4, mib4, mib4, 33554432, "").state == S::insufficient);
    // Insufficient: the clamp measured on the unchanged host (rmem_max 212992).
    o = aravis::assess_receive_buffer(3, mib4, mib4, 425984, 212992, "");
    CHECK(o.state == S::insufficient && o.granted_bytes == 212992);
    CHECK(contains(o.detail, "net.core.rmem_max 212992 is below the request"));
    const auto warning = aravis::receive_buffer_warning(o);
    CHECK(contains(warning, "GVSP receive buffer insufficient for stream generation 3"));
    CHECK(contains(warning, "requested 4194304 bytes, granted 212992 bytes (kernel SO_RCVBUF 425984)"));
    CHECK(contains(warning, "net.core.rmem_max 212992") && contains(warning, "Acquisition continues"));
    CHECK(warning.find('\n') == std::string::npos); // one line
    // Insufficient with an unreadable rmem_max: still insufficient, limit "unknown".
    o = aravis::assess_receive_buffer(4, mib4, mib4, 425984, std::nullopt, "");
    CHECK(o.state == S::insufficient && contains(aravis::receive_buffer_warning(o), "net.core.rmem_max unknown"));
    // Unknown: no value, a zero/negative value, or no request. Never a guess.
    for (const std::optional<std::int64_t> reported : {std::optional<std::int64_t>{}, std::optional<std::int64_t>{0},
                                                       std::optional<std::int64_t>{-1}}) {
        o = aravis::assess_receive_buffer(5, mib4, mib4, reported, 33554432, "no UDP IPv4 socket of this process");
        CHECK(o.state == S::unknown && !o.kernel_reported_bytes && !o.granted_bytes);
        CHECK(contains(o.detail, "could not be observed: no UDP IPv4 socket"));
        CHECK(contains(aravis::receive_buffer_warning(o), "GVSP receive buffer unknown"));
        CHECK(contains(aravis::receive_buffer_warning(o), "granted unknown bytes"));
    }
    o = aravis::assess_receive_buffer(6, 0, std::nullopt, 8388608, 33554432, "");
    CHECK(o.state == S::unknown && !o.granted_bytes);
    // Not applicable and pending never warn.
    aravis::ReceiveBufferObservation quiet;
    CHECK(quiet.state == S::not_applicable && aravis::receive_buffer_warning(quiet).empty());
    quiet.state = S::pending;
    CHECK(aravis::receive_buffer_warning(quiet).empty());
    CHECK(aravis::receive_buffer_state_name(S::sufficient) == "sufficient");
    CHECK(aravis::receive_buffer_state_name(S::insufficient) == "insufficient");
    CHECK(aravis::receive_buffer_state_name(S::unknown) == "unknown");
    CHECK(aravis::receive_buffer_state_name(S::not_applicable) == "not_applicable");
    CHECK(aravis::receive_buffer_state_name(S::pending) == "pending");
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

// GIGE-MEL-018 (observed on a Cognex CIC-5000R, firmware V3.7-1): only the
// legacy float AcquisitionFrameRateAbs [Hz] is implemented, and
// DeviceSerialNumber is a GenICam Integer node. Reproduced on the real SDK
// Fake GenICam tree: the native AcquisitionFrameRate node is made SDK-
// unimplemented and an Abs float with <Unit>Hz</Unit> is added beside it.
void gc_add_rate_abs(ArvDevice* d, const char* value) {
    auto* node = gc_add(d, nullptr, "Float", "AcquisitionFrameRateAbs");
    gc_add(d, node, "Value", nullptr, value);
    gc_add(d, node, "Min", nullptr, "0.5");
    gc_add(d, node, "Max", nullptr, "500.0");
    gc_add(d, node, "Unit", nullptr, "Hz");
}
void legacy_frame_rate_and_integer_identity() {
    auto observed = std::make_shared<NativeOpens>();
    std::vector<std::string> attempted;
    {
        // SFNC name implemented: it is selected even when the Abs name exists.
        auto device = open_fake(*arranged_backend(observed, [](ArvDevice* d) { gc_add_rate_abs(d, "7.0"); }));
        CHECK(device->configuration().value() == fake_initial); // 25 Hz from AcquisitionFrameRate
        CHECK(device->close());
    }
    auto legacy = arranged_backend(
        observed,
        [](ArvDevice* d) {
            gc_add_rate_abs(d, "7.0");
            gc_set_unimplemented(d, "AcquisitionFrameRate"); // SDK: not implemented, no GError
        },
        &attempted);
    {
        auto device = open_fake(*legacy);
        const auto caps = device->capabilities();
        CHECK(caps.minimum_frame_rate_hz == 1 && caps.maximum_frame_rate_hz == 500); // the Abs bounds
        auto expected = fake_initial;
        expected.frame_rate_hz = 7;
        CHECK(device->configuration().value() == expected);
        // apply() writes the selected legacy feature, never the absent name.
        // 7 -> 3 Hz lengthens the period, so the rate is written first; floats
        // are always written (existing policy).
        auto request = expected;
        request.frame_rate_hz = 3;
        CHECK(device->apply(request).value() == request);
        CHECK((attempted == std::vector<std::string>{"AcquisitionFrameRateAbs", "ExposureTimeAbs"}));
        const auto rejected = device->apply({512, 512, 0, 0, 501, 10ms});
        CHECK(!rejected && contains(rejected.error().message, "AcquisitionFrameRateAbs 501 Hz is outside the native bounds"));
        CHECK(attempted.size() == 2); // nothing more written
        CHECK(device->close());
    }
    {
        // Abs declaring a non-Hz unit is unsupported with the selected name.
        const auto wrong_unit = arranged_backend(observed, [](ArvDevice* d) {
            auto* node = gc_add(d, nullptr, "Float", "AcquisitionFrameRateAbs");
            gc_add(d, node, "Value", nullptr, "7.0");
            gc_add(d, node, "Unit", nullptr, "us");
            gc_set_unimplemented(d, "AcquisitionFrameRate");
        })->open(aravis_fake::device_id);
        CHECK(!wrong_unit && contains(wrong_unit.error().message, "AcquisitionFrameRateAbs declares unit 'us'"));
    }
    {
        // A failed SFNC implementation query never falls back to Abs.
        const auto broken = arranged_backend(observed, [](ArvDevice* d) {
            gc_add_rate_abs(d, "7.0");
            (void)break_implementation_query(d, "AcquisitionFrameRate");
        })->open(aravis_fake::device_id);
        CHECK(!broken && contains(broken.error().message, "optional feature 'AcquisitionFrameRate'"));
        // Neither implemented: explicit unsupported diagnostic.
        const auto neither = arranged_backend(observed, [](ArvDevice* d) {
            gc_set_unimplemented(d, "AcquisitionFrameRate");
        })->open(aravis_fake::device_id);
        CHECK(!neither && contains(neither.error().message, "neither AcquisitionFrameRate nor AcquisitionFrameRateAbs"));
    }
    {
        // Integer identity node: exact decimal readback, labelled; inspection
        // and the timing context both report it instead of a type error.
        auto camera = arranged_backend(observed, [](ArvDevice* d) {
            gc_add(d, gc_add(d, nullptr, "Integer", "DeviceSerialNumber"), "Value", nullptr, "21874533");
        });
        const auto inspection = aravis::inspect(*camera, aravis_fake::device_id);
        CHECK(inspection && inspection.value().problems.empty());
        bool serial = false;
        for (const auto& [name, value] : inspection.value().native)
            serial = serial || (name == "DeviceSerialNumber" && value == "21874533 (GenICam Integer node)");
        CHECK(serial);
        auto device = open_fake(*camera);
        const auto context = aravis::read_timing_context(*device);
        CHECK(context);
        serial = false;
        for (const auto& [name, value] : context.value().identity)
            serial = serial || (name == "DeviceSerialNumber" && value == "21874533 (GenICam Integer node)");
        CHECK(serial);
        CHECK(device->close());
    }
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
        {"GVSP receive-buffer assessment and warning text (SDK-free, GIGE-MEL-019)", receive_buffer_assessment},
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
        {"legacy AcquisitionFrameRateAbs and Integer identity (GIGE-MEL-018)", legacy_frame_rate_and_integer_identity},
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
