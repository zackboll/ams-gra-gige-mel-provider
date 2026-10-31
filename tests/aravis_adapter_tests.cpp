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
    auto grid = n;
    grid.width.increment = 8;
    grid.width.minimum = 8;
    CHECK(contains(profile::plan_writes({100, 64, 0, 0, 25, 1ms}, n, grid).error().message, "native grid"));
    auto stepped = n;
    stepped.exposure_us.increment = 100.0;
    CHECK(contains(profile::plan_writes({64, 64, 0, 0, 25, 1'050'000ns}, n, stepped).error().message, "increment"));
    CHECK(profile::plan_writes({64, 64, 0, 0, 25, 1'010'000ns}, n, stepped)); // 10 us + 10 * 100 us

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

// Two adapters race to open the same ID. The first is held inside open (after
// its reservation) by a gate; the competitor's result is observed while the
// first is provably mid-open, so no sleep is used as evidence.
void concurrent_open_is_serialized() {
    Gate gate;
    aravis::TestHooks hooks;
    hooks.after_reservation = [&gate](std::string_view) { gate.enter_and_wait(); };
    auto first = aravis_fake::make_backend(hooks);
    auto second = aravis_fake::make_backend();
    auto pending = std::async(std::launch::async, [&] { return first->open(aravis_fake::device_id); });
    struct Opener { Gate& g; std::future<backend::Result<std::shared_ptr<backend::ICameraDevice>>>& f;
                    ~Opener() { g.open(); if (f.valid()) f.wait(); } } cleanup{gate, pending};
    gate.wait_entered();
    // The second adapter shares the interface lock, so its open blocks until
    // the first completes. Launch it, release the gate, then require busy.
    auto competitor = std::async(std::launch::async, [&] { return second->open(aravis_fake::device_id); });
    gate.open();
    CHECK(pending.wait_for(10s) == std::future_status::ready);
    auto winner = pending.get();
    CHECK(winner);
    CHECK(competitor.wait_for(10s) == std::future_status::ready);
    const auto loser = competitor.get();
    CHECK(!loser && loser.error().code == backend::ErrorCode::busy);
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

// Late failure after earlier writes succeeded. Prevalidation passes against
// the open-time envelope; immediately before the exposure write, a private
// native-boundary hook lowers the Fake's ExposureTimeAbs maximum through the
// real SDK (arv_gc_float_impose_max), modelling a dynamic native constraint.
// The SDK's own range check then rejects the write. This is an arranged SDK
// constraint change, not a physical camera fault.
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
    // The dynamic constraint stays; a request violating it is rejected again
    // by the SDK (prevalidation uses the advertised envelope only).
    CHECK(!device->apply({320, 240, 32, 16, 20, 1ms}));
    CHECK(device->configuration().value() == partial);
    // Explicit recovery to a representable configuration within it.
    const backend::CameraConfiguration recovery{320, 240, 32, 16, 20, 400us};
    CHECK(device->apply(recovery).value() == recovery);
    CHECK(device->close());
}

void close_destruction_and_streaming() {
    auto observed = std::make_shared<NativeOpens>();
    auto camera = aravis_fake::make_backend(observed->hooks());
    auto device = open_fake(*camera);
    const auto caps = device->capabilities();
    // Explicitly unsupported acquisition: no native stream object is created.
    const auto stream = device->create_stream();
    CHECK(!stream && stream.error().code == backend::ErrorCode::unsupported);
    CHECK(contains(stream.error().message, "acquisition is not implemented"));
    CHECK(observed->live() == 1);

    // Idempotent logical close; later I/O is rejected; capabilities() returns
    // the validated snapshot without device I/O.
    CHECK(device->close());
    CHECK(observed->live() == 0); // native device finalized at close (weak ref fired)
    CHECK(device->close());
    CHECK(device->configuration().error().code == backend::ErrorCode::disconnected);
    CHECK(device->apply(fake_initial).error().code == backend::ErrorCode::disconnected);
    CHECK(device->create_stream().error().code == backend::ErrorCode::disconnected);
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
        {"construction without I/O and copied discovery", construction_and_discovery},
        {"exact device selection", exact_selection},
        {"ownership and failed-open cleanup", ownership_and_failed_open},
        {"concurrent open across adapters", concurrent_open_is_serialized},
        {"real capabilities and two valid configurations", capabilities_and_configuration},
        {"SDK quantization and truthful readback", quantization_and_truthful_readback},
        {"late write failure and explicit recovery", late_write_failure_and_recovery},
        {"close, destruction and unsupported streaming", close_destruction_and_streaming},
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
