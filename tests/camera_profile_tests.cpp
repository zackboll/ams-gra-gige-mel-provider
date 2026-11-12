// SDK-free contract for the shared free-running monochrome camera profile.
// These constructed-snapshot and frame-fact scenarios were previously gated
// behind the optional Aravis tests. Native SDK integration remains separately
// covered by those tests; no SDK or physical-device qualification is implied.
#include "profiles/mono_free_running.hpp"

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace backend = gige_mel::backend;
namespace profile = gige_mel::backend::profiles::mono_free_running;
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

// Explicit test budgets. The profile must not select a backend's host policy.
constexpr std::size_t test_frame_limit = std::size_t{64} << 20;
constexpr std::size_t test_pool_limit = std::size_t{256} << 20;

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

    auto mono12 = n;
    mono12.pixel_format = "Mono12";
    mono12.mono12_available = true;
    CHECK(!profile::capabilities_from(mono12)); // PFNC alone cannot establish endianness
    mono12.pixel_endianness = "Big";
    CHECK(!profile::capabilities_from(mono12));
    mono12.pixel_endianness = "Little";
    const auto twelve = profile::capabilities_from(mono12).value();
    CHECK(twelve.pixel_format == backend::PixelFormat::mono12 && !twelve.mono8);
    CHECK(profile::configuration_from(mono12).value() == profile::configuration_from(n).value());
    CHECK(!profile::acquisition_preflight(mono12, n)); // format is immutable per open
    mono12.pixel_format = "Mono12Packed";
    CHECK(!profile::capabilities_from(mono12));

    // Unsupported profiles fail with a diagnostic; nothing is fabricated.
    auto bad = n;
    bad.pixel_format = "Mono16";
    CHECK(contains(profile::capabilities_from(bad).error().message, "Mono16"));
    CHECK(contains(profile::capabilities_from(bad).error().message, "profile aravis-mono8-free-running-v1"));
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

void pool_planning() {
    const backend::CameraConfiguration c{64, 48, 0, 0, 25, 10ms};
    CHECK(profile::plan_pool(3072, c, 4, test_frame_limit, test_pool_limit, backend::PixelFormat::mono8).value().total_bytes == 4 * 3072);
    CHECK(profile::plan_pool(0, c, 4, test_frame_limit, test_pool_limit, backend::PixelFormat::mono8).error().code == backend::ErrorCode::unsupported);
    CHECK(profile::plan_pool(-1, c, 4, test_frame_limit, test_pool_limit, backend::PixelFormat::mono8).error().code == backend::ErrorCode::unsupported);
    CHECK(contains(profile::plan_pool(3071, c, 4, test_frame_limit, test_pool_limit, backend::PixelFormat::mono8).error().message, "smaller than the 64x48 Mono8 image"));
    CHECK(profile::plan_pool(std::int64_t{65} << 20, c, 4, test_frame_limit, test_pool_limit, backend::PixelFormat::mono8).error().code == backend::ErrorCode::resource_exhausted);
    CHECK(profile::plan_pool(std::numeric_limits<std::int64_t>::max(), c, 4, test_frame_limit, test_pool_limit, backend::PixelFormat::mono8).error().code ==
          backend::ErrorCode::resource_exhausted);
    CHECK(profile::plan_pool(std::int64_t{64} << 20, c, 5, test_frame_limit, test_pool_limit, backend::PixelFormat::mono8).error().code == backend::ErrorCode::resource_exhausted);
    CHECK(profile::plan_pool(std::int64_t{64} << 20, c, 4, test_frame_limit, test_pool_limit, backend::PixelFormat::mono8).value().total_bytes == std::size_t{256} << 20);
    const auto huge = std::numeric_limits<std::size_t>::max();
    CHECK(profile::plan_pool(4096, c, huge, huge, huge, backend::PixelFormat::mono8).error().code == backend::ErrorCode::resource_exhausted);

    // Another backend may choose different budgets for the same camera.
    // The shared arithmetic must honor those inputs without imposing Aravis's
    // four-buffer policy or its resource ceilings.
    const auto pair = profile::plan_pool(3072, c, 2, 3072, 6144, backend::PixelFormat::mono8);
    CHECK(pair && pair.value().buffers == 2 && pair.value().buffer_bytes == 3072 && pair.value().total_bytes == 6144);
    CHECK(profile::plan_pool(3072, c, 2, 3071, 6144, backend::PixelFormat::mono8).error().code == backend::ErrorCode::resource_exhausted);
    CHECK(profile::plan_pool(3072, c, 2, 3072, 6143, backend::PixelFormat::mono8).error().code == backend::ErrorCode::resource_exhausted);
    CHECK(profile::plan_pool(3072, c, 0, 3072, 6144, backend::PixelFormat::mono8).error().code == backend::ErrorCode::invalid_argument);
}

void mono8_frame_validation() {
    const backend::CameraConfiguration c{};
    profile::NativeFrameFacts f;
    f.status = profile::NativeStatus::success;
    f.payload = profile::NativePayload::image;
    f.parts = 1; f.part_is_2d_image = true; f.pixel_format = profile::pfnc_mono8;
    f.width = 64; f.height = 48; f.padding_x = 7;
    f.image_bytes = f.received_bytes = f.capacity_bytes = 3401; // 47 padded rows and one 64-byte row
    f.data_present = true;
    const auto check = [&](const auto& facts) { return profile::validate_frame(facts, c, backend::PixelFormat::mono8); };
    const auto accepted = check(f);
    CHECK(accepted.verdict == profile::FrameVerdict::accept && accepted.row_stride == 71 && accepted.span_bytes == 3401);
    auto full_padding = f;
    full_padding.image_bytes = full_padding.received_bytes = full_padding.capacity_bytes = 3408;
    CHECK(check(full_padding).verdict == profile::FrameVerdict::accept);

    struct Case { const char* name; void (*edit)(profile::NativeFrameFacts&); };
    const Case rejected[] = {
        {"status", [](auto& n) { n.status = profile::NativeStatus::other; }},
        {"multipart", [](auto& n) { n.payload = profile::NativePayload::multipart; }},
        {"chunk payload", [](auto& n) { n.payload = profile::NativePayload::chunk_or_extended; }},
        {"chunks", [](auto& n) { n.has_chunks = true; }},
        {"part count", [](auto& n) { n.parts = 2; }},
        {"non-image part", [](auto& n) { n.part_is_2d_image = false; }},
        {"pixel format", [](auto& n) { n.pixel_format = profile::pfnc_mono12; }},
        {"storage bits", [](auto& n) { n.storage_bits = 16; }},
        {"significant bits", [](auto& n) { n.significant_bits = 12; }},
        {"byte order", [](auto& n) { n.byte_order = backend::ByteOrder::little_endian; }},
        {"width", [](auto& n) { ++n.width; }},
        {"height", [](auto& n) { n.height = 0; }},
        {"offset", [](auto& n) { ++n.offset_x; }},
        {"negative padding", [](auto& n) { n.padding_x = -1; }},
        {"span overflow", [](auto& n) { n.padding_x = INT64_MAX; }},
        {"Y padding", [](auto& n) { n.padding_y = 1; }},
        {"missing data", [](auto& n) { n.data_present = false; }},
        {"short image", [](auto& n) { --n.image_bytes; }},
        {"short receive", [](auto& n) { --n.received_bytes; }},
        {"short capacity", [](auto& n) { --n.capacity_bytes; }},
        {"receive beyond capacity", [](auto& n) { ++n.received_bytes; }},
        {"part offset", [](auto& n) { n.part_offset = 1; }},
        {"part beyond receive", [](auto& n) { n.part_offset = SIZE_MAX; }},
    };
    for (const auto& test : rejected) {
        auto bad = f;
        test.edit(bad);
        if (check(bad).verdict != profile::FrameVerdict::reject)
            throw std::runtime_error(std::string("Mono8 frame should reject ") + test.name);
    }
    auto incomplete = f;
    incomplete.status = profile::NativeStatus::incomplete;
    CHECK(check(incomplete).verdict == profile::FrameVerdict::incomplete);
}

void mono12_frame_validation() {
    const backend::CameraConfiguration c{};
    profile::NativeFrameFacts f;
    f.status = profile::NativeStatus::success; f.payload = profile::NativePayload::image;
    f.parts = 1; f.part_is_2d_image = true; f.pixel_format = profile::pfnc_mono12;
    f.width = 64; f.height = 48; f.padding_x = 7;
    f.image_bytes = f.received_bytes = f.capacity_bytes = 135 * 47 + 128;
    f.data_present = true; f.storage_bits = 16; f.significant_bits = 12;
    f.byte_order = backend::ByteOrder::little_endian;
    const auto check = [&](const auto& facts) { return profile::validate_frame(facts, c, backend::PixelFormat::mono12); };
    CHECK(check(f).verdict == profile::FrameVerdict::accept && check(f).span_bytes == 6473);
    using Edit = std::function<void(profile::NativeFrameFacts&)>;
    const std::vector<Edit> edits{
        [](auto& n) { n.pixel_format = 0x010c0006; }, // Mono12Packed
        [](auto& n) { n.pixel_format = 0x010c0004; }, // Mono10Packed
        [](auto& n) { n.pixel_format = profile::pfnc_mono8; },
        [](auto& n) { n.storage_bits = 12; }, [](auto& n) { n.significant_bits = 16; },
        [](auto& n) { n.byte_order = backend::ByteOrder::big_endian; },
        [](auto& n) { ++n.width; }, [](auto& n) { n.height = 0; },
        [](auto& n) { n.padding_x = -1; }, [](auto& n) { n.padding_x = INT64_MAX; },
        [](auto& n) { --n.image_bytes; }, [](auto& n) { --n.received_bytes; },
        [](auto& n) { --n.capacity_bytes; }, [](auto& n) { ++n.received_bytes; },
        [](auto& n) { n.parts = 2; }, [](auto& n) { n.has_chunks = true; },
        [](auto& n) { n.payload = profile::NativePayload::multipart; },
        [](auto& n) { n.payload = profile::NativePayload::chunk_or_extended; },
        [](auto& n) { n.status = profile::NativeStatus::incomplete; },
    };
    for (const auto& edit : edits) { auto bad = f; edit(bad); CHECK(check(bad).verdict != profile::FrameVerdict::accept); }
    auto full_padding = f;
    full_padding.image_bytes = full_padding.received_bytes = full_padding.capacity_bytes = 135 * 48;
    CHECK(check(full_padding).verdict == profile::FrameVerdict::accept);
    CHECK(!profile::plan_pool(6143, c, 4, test_frame_limit, test_pool_limit, backend::PixelFormat::mono12));
    CHECK(profile::plan_pool(6144, c, 4, test_frame_limit, test_pool_limit, backend::PixelFormat::mono12));
    auto impossible = c; impossible.width = impossible.height = UINT32_MAX;
    CHECK(!profile::plan_pool(INT64_MAX, impossible, 4, SIZE_MAX, SIZE_MAX, backend::PixelFormat::mono12));
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"checked numeric conversions", checked_conversions},
        {"profile translation, prevalidation and write order", profile_translation},
        {"native fact validation and overflow-safe planning", native_fact_validation},
        {"fresh native constraints in prevalidation", fresh_constraints},
        {"acquisition preflight profile table", acquisition_preflight_profile},
        {"fresh ROI maxima by write order", roi_maxima_by_write_order},
        {"explicit source-pool budgets and checked arithmetic", pool_planning},
        {"Mono8 frame facts and padding", mono8_frame_validation},
        {"Mono12 frame facts and padding", mono12_frame_validation},
    };
    for (const auto& [name, run] : tests) {
        try {
            run();
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
        std::cout << "PASS " << name << '\n';
    }
    std::cout << tests.size() << " SDK-free camera profile scenarios passed\n";
    return 0;
}
