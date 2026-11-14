// Actual eBUS/GenApi runtime tests. The original XML/local IPort fixture
// exercises the SAME native typed feature implementation used by production.
// No camera, NIC, stream receive, MEL publication or timestamp is qualified by
// this executable; those belong to the explicitly enabled hardware contract.
#include "pleora_features.hpp"
#include "pleora_private.hpp"
#include "pleora_node_map.hpp"
#include "profiles/mono_free_running_device.hpp"

#include <PvDeviceGEV.h>
#include <PvGenParameterArray.h>
#include <PvResult.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace backend = gige_mel::backend;
namespace features = backend::device_features;
namespace pleora = backend::pleora;
namespace profile = backend::profiles::mono_free_running;
namespace fixture = pleora_node_map;
namespace native = fixture::native;
using features::ReadAccess;
using namespace std::chrono_literals;

namespace {

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression))                                                                           \
            throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) +          \
                                     ": " #expression);                                             \
    } while (false)

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

template <class T> T take(backend::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

template <class T> void rejected(const backend::Result<T>& result, backend::ErrorCode code,
                                 std::string_view message = {}) {
    CHECK(!result);
    CHECK(result.error().code == code);
    CHECK(contains(result.error().message, message));
}

const profile::ConnectionCheck connected = [] { return backend::Status{}; };
const profile::BeforeWrite ready = [](std::string_view) { return backend::Status{}; };

std::vector<std::int64_t> writes(const fixture::Port& port) {
    std::vector<std::int64_t> result;
    for (const auto& call : port.calls) if (call.write) result.push_back(call.address);
    return result;
}

void actual_ebus_parameter_array_and_result_translation() {
    // Uses a real eBUS-owned parameter array and its PUBLIC GetNodeMap(), the
    // production constructor path. Communication parameters exist before
    // connection; changing these local values does not touch a device.
    PvDeviceGEV device;
    CHECK(device.GetCommunicationParameters());
    pleora::Features view(*device.GetCommunicationParameters());
    CHECK(take(view.implemented("CommandRetryCount")));
    const auto retries = take(view.read_integer_feature("CommandRetryCount", ReadAccess::read_write));
    CHECK(retries.increment > 0 && retries.minimum <= retries.value && retries.value <= retries.maximum);
    CHECK(view.write_integer("CommandRetryCount", retries.value));
    CHECK(take(view.read_integer("CommandRetryCount")) == retries.value);
    CHECK(take(view.implemented("ForcedCommandPortEnabled")));
    (void)take(view.read_boolean("ForcedCommandPortEnabled"));
    CHECK(!take(view.implemented("GigeMelNodeThatDoesNotExist")));
    rejected(view.read_float_feature("CommandRetryCount", ReadAccess::readable), backend::ErrorCode::unsupported);
    CHECK(device.GetCommunicationParameters()->SetBooleanValue("LinkRecoveryEnabled", true).IsOK());
    CHECK(take(view.read_boolean("LinkRecoveryEnabled")));
    CHECK(pleora::configure_device_policy(device));
    CHECK(!take(view.read_boolean("LinkRecoveryEnabled"))); // no automatic reconnection

    const std::pair<std::uint32_t, backend::ErrorCode> errors[] = {
        {PvResult::Code::TIMEOUT, backend::ErrorCode::timeout},
        {PvResult::Code::PENDING, backend::ErrorCode::timeout},
        {PvResult::Code::NOT_CONNECTED, backend::ErrorCode::disconnected},
        {PvResult::Code::BUSY, backend::ErrorCode::busy},
        {PvResult::Code::INVALID_PARAMETER, backend::ErrorCode::invalid_argument},
        {PvResult::Code::ABORTED, backend::ErrorCode::stopped},
        {PvResult::Code::NO_LICENSE, backend::ErrorCode::unsupported},
        {PvResult::Code::NETWORK_ERROR, backend::ErrorCode::io_error}};
    for (const auto& [code, expected] : errors) {
        const PvResult result(code);
        const auto error = pleora::native_error(result, "read Width");
        CHECK(error.code == expected);
        CHECK(contains(error.message, "Width"));
        CHECK(contains(error.message, result.GetCodeString().GetAscii()));
    }
}

void integer_type_access_and_live_grid() {
    fixture::Fixture camera;
    pleora::Features view(*camera.map._Ptr);
    const auto width = take(view.read_integer_feature("Width", ReadAccess::read_write));
    CHECK(width.value == 320 && width.minimum == 2 && width.maximum == 640 && width.increment == 2);
    rejected(view.read_integer("AcquisitionFrameRateEnable"), backend::ErrorCode::unsupported);
    rejected(view.read_integer_feature("ReadOnly", ReadAccess::read_write), backend::ErrorCode::unsupported);
    CHECK(take(view.read_integer_feature("ReadOnly", ReadAccess::readable)).value == 7);
    rejected(view.read_integer("WriteOnly"), backend::ErrorCode::unsupported);
    rejected(view.read_integer(""), backend::ErrorCode::invalid_argument);
    rejected(view.implemented(std::string_view{"Width\0ignored", 13}), backend::ErrorCode::invalid_argument);
    camera.port.calls.clear();
    rejected(view.write_integer("Width", 321), backend::ErrorCode::invalid_argument, "grid");
    rejected(view.write_integer("Width", 642), backend::ErrorCode::invalid_argument, "bounds");
    rejected(view.write_integer("ReadOnly", 8), backend::ErrorCode::unsupported);
    CHECK(camera.port.write_attempts() == 0);
    CHECK(view.write_integer("Width", 318));
    CHECK(camera.port.integer(fixture::width) == 318);
    CHECK(camera.port.write_attempts(fixture::width) == 1);
    camera.port.integer(fixture::width_locked, 1);
    CHECK(take(view.read_integer_feature("Width", ReadAccess::readable)).value == 318);
    rejected(view.read_integer_feature("Width", ReadAccess::read_write), backend::ErrorCode::unsupported);
    rejected(view.write_integer("Width", 316), backend::ErrorCode::unsupported);
    CHECK(camera.port.write_attempts(fixture::width) == 1);

    CHECK(view.write_integer("SignedInteger", std::numeric_limits<std::int64_t>::max()));
    CHECK(take(view.read_integer("SignedInteger")) == std::numeric_limits<std::int64_t>::max());
    CHECK(view.write_integer("SignedInteger", std::numeric_limits<std::int64_t>::min()));
    CHECK(take(view.read_integer("SignedInteger")) == std::numeric_limits<std::int64_t>::min());
}

void float_native_increment_modes_and_checked_writes() {
    fixture::Fixture camera;
    pleora::Features view(*camera.map._Ptr);
    const auto rate = take(view.read_float_feature("AcquisitionFrameRate", ReadAccess::read_write));
    CHECK(rate.value == 50 && rate.minimum == 1 && rate.maximum == 200);
    CHECK(!rate.increment && rate.unit == "Hz");
    CHECK(native::CFloatPtr(camera.map._GetNode("AcquisitionFrameRate"))->GetIncMode() == native::noIncrement);
    const auto fixed = take(view.read_float_feature("FixedFloat", ReadAccess::read_write));
    CHECK(fixed.value == 1 && fixed.minimum == 0.5 && fixed.maximum == 10.5);
    CHECK(fixed.increment == 0.25 && fixed.unit == "us");
    CHECK(native::CFloatPtr(camera.map._GetNode("FixedFloat"))->GetIncMode() == native::fixedIncrement);
    CHECK(native::CIntegerPtr(camera.map._GetNode("ListInteger"))->GetIncMode() == native::listIncrement);
    CHECK(native::CFloatPtr(camera.map._GetNode("ListFloat"))->GetIncMode() == native::listIncrement);
    rejected(view.read_integer_feature("ListInteger", ReadAccess::readable), backend::ErrorCode::unsupported, "list");
    rejected(view.read_float_feature("ListFloat", ReadAccess::readable), backend::ErrorCode::unsupported, "list");
    camera.port.calls.clear();
    rejected(view.write_float("FixedFloat", 1.1), backend::ErrorCode::invalid_argument, "grid");
    rejected(view.write_float("FixedFloat", 12.0), backend::ErrorCode::invalid_argument, "bounds");
    rejected(view.write_float("FixedFloat", std::numeric_limits<double>::quiet_NaN()), backend::ErrorCode::invalid_argument);
    rejected(view.write_float("FixedFloat", std::numeric_limits<double>::infinity()), backend::ErrorCode::invalid_argument);
    rejected(view.write_float("Width", 4), backend::ErrorCode::unsupported);
    rejected(view.write_float("ListFloat", 2), backend::ErrorCode::unsupported, "list");
    CHECK(camera.port.write_attempts() == 0);
    CHECK(view.write_float("FixedFloat", 1.25));
    CHECK(camera.port.real(fixture::fixed_float) == 1.25);
    CHECK(camera.port.write_attempts(fixture::fixed_float) == 1);
}

void absence_native_query_errors_and_alias_selection() {
    fixture::Fixture camera;
    pleora::Features view(*camera.map._Ptr);
    CHECK(!take(view.implemented("MissingNode")));
    CHECK(!take(view.implemented("NeverImplemented")));
    camera.port.fail_read = fixture::broken_implemented;
    rejected(view.implemented("BrokenImplementation"), backend::ErrorCode::io_error, "injected test port read failure");
    camera.port.fail_read.reset();
    auto snapshot = take(profile::read_snapshot(view));
    CHECK(snapshot.exposure_feature == "ExposureTime" && snapshot.frame_rate_feature == "AcquisitionFrameRate");
    camera.port.integer(fixture::exposure_implemented, 0);
    camera.port.integer(fixture::rate_implemented, 0);
    snapshot = take(profile::read_snapshot(view));
    CHECK(snapshot.exposure_feature == "ExposureTimeAbs" && snapshot.frame_rate_feature == "AcquisitionFrameRateAbs");
    camera.port.integer(fixture::exposure_implemented, 1);
    camera.port.integer(fixture::exposure_available, 0);
    CHECK(take(view.implemented("ExposureTime")));
    rejected(profile::read_snapshot(view), backend::ErrorCode::unsupported, "ExposureTime");
    // An implemented-but-unavailable preferred node cannot fall back to Abs.
    camera.port.integer(fixture::exposure_available, 1);
    camera.port.fail_read = fixture::exposure_implemented;
    rejected(profile::read_snapshot(view), backend::ErrorCode::io_error, "ExposureTime");
    CHECK(camera.port.write_attempts() == 0);
}

void string_enum_boolean_facts_and_entry_errors() {
    fixture::Fixture camera;
    pleora::Features view(*camera.map._Ptr);
    CHECK(take(view.read_string_or_enum("Description")) == "original local GenApi fixture");
    CHECK(take(view.read_string_or_enum("PixelFormat")) == "Mono8");
    CHECK(take(view.enum_entry_available("PixelFormat", "Mono8")));
    CHECK(take(view.enum_entry_available("PixelFormat", "Mono12")));
    CHECK(!take(view.enum_entry_available("PixelFormat", "MissingEntry")));
    CHECK(take(view.read_boolean("AcquisitionFrameRateEnable")));
    rejected(view.read_boolean("Width"), backend::ErrorCode::unsupported);
    rejected(view.read_string_or_enum("Width"), backend::ErrorCode::unsupported);
    rejected(view.enum_entry_available("Description", "Mono8"), backend::ErrorCode::unsupported);
    camera.port.integer(fixture::mono8_available, 0);
    CHECK(!take(view.enum_entry_available("PixelFormat", "Mono8")));
    camera.port.fail_read = fixture::mono8_available;
    rejected(view.enum_entry_available("PixelFormat", "Mono8"), backend::ErrorCode::io_error);
    camera.port.fail_read.reset();
    camera.port.integer(fixture::mono8_available, 1);
    camera.port.integer(fixture::frame_rate_enabled, 0);
    CHECK(!take(view.read_boolean("AcquisitionFrameRateEnable")));
    const auto snapshot = take(profile::read_snapshot(view));
    rejected(profile::check_preconditions(snapshot), backend::ErrorCode::unsupported, "AcquisitionFrameRateEnable");
    CHECK(camera.port.write_attempts() == 0);
    rejected(view.enum_entry_available("PixelFormat", ""), backend::ErrorCode::invalid_argument);
    rejected(view.enum_entry_available("PixelFormat", std::string_view{"Mono8\0bad", 9}), backend::ErrorCode::invalid_argument);
    const auto owned_string = take(view.read_string_or_enum("Description"));
    const auto owned_float = take(view.read_float_feature("ExposureTime", ReadAccess::readable));
    camera.port.real(fixture::exposure, 2000);
    CHECK(take(view.read_float_feature("ExposureTime", ReadAccess::readable)).value == 2000);
    CHECK(owned_float.value == 1000 && owned_float.unit == "us");
    CHECK(owned_string == "original local GenApi fixture");
}

void cache_freshness_and_dynamic_constraint_order() {
    fixture::Fixture camera;
    pleora::Features view(*camera.map._Ptr);
    native::CIntegerPtr raw_width(camera.map._GetNode("Width"));
    CHECK(raw_width->GetValue() == 320);
    camera.port.integer(fixture::width, 300);
    CHECK(raw_width->GetValue() == 320); // native cache really is populated
    CHECK(take(view.read_integer("Width")) == 300); // service invalidates it
    camera.port.integer(fixture::width, 320);
    camera.port.integer(fixture::offset_x, 320);
    CHECK(take(view.read_integer_feature("Width", ReadAccess::read_write)).maximum == 320);
    camera.port.calls.clear();
    rejected(view.write_integer("Width", 480), backend::ErrorCode::invalid_argument, "bounds");
    CHECK(camera.port.write_attempts() == 0);
    const auto envelope = take(profile::read_snapshot(view));
    auto request = take(profile::configuration_from(envelope));
    request.offset_x = 0;
    request.width = 480;
    request.frame_rate_hz = 25;
    request.exposure = 30ms; // old max20ms, new max40ms after rate write
    camera.port.calls.clear();
    CHECK(take(profile::apply_configuration(view, request, envelope, connected, ready)) == request);
    CHECK(writes(camera.port) == std::vector<std::int64_t>({fixture::offset_x, fixture::width,
                                                          fixture::rate, fixture::exposure}));
    CHECK(take(view.read_float_feature("ExposureTime", ReadAccess::read_write)).maximum == 40000);
}

void unchanged_profile_prevalidation_and_truthful_readback() {
    fixture::Fixture camera;
    pleora::Features view(*camera.map._Ptr);
    const auto envelope = take(profile::read_snapshot(view));
    CHECK(profile::profile_name == "aravis-mono8-free-running-v1");
    const auto capabilities = take(profile::capabilities_from(envelope));
    CHECK(capabilities.sensor_width == 640 && capabilities.sensor_height == 480);
    auto request = take(profile::read_configuration(view, envelope, connected));
    CHECK(request.width == 320 && request.height == 240 && request.frame_rate_hz == 50 && request.exposure == 1ms);
    request.width = 321;
    camera.port.calls.clear();
    rejected(profile::apply_configuration(view, request, envelope, connected, ready), backend::ErrorCode::invalid_argument);
    CHECK(camera.port.write_attempts() == 0);
    request.width = 300;
    request.exposure = 1200us;
    camera.port.after_write = [&](std::int64_t address) {
        if (address == fixture::exposure) camera.port.real(fixture::exposure, 1150.0);
    };
    const auto actual = take(profile::apply_configuration(view, request, envelope, connected, ready));
    CHECK(actual.width == 300 && actual.exposure == 1150us && actual != request);
    CHECK(camera.port.write_attempts(fixture::exposure) == 1);
    CHECK(camera.port.write_attempts(fixture::rate) == 1);
    camera.port.after_write = {};
    camera.port.integer(fixture::trigger_mode, 1);
    camera.port.calls.clear();
    rejected(profile::apply_configuration(view, request, envelope, connected, ready), backend::ErrorCode::unsupported, "TriggerMode");
    CHECK(camera.port.write_attempts() == 0); // no hidden trigger normalization
}

void partial_write_native_failure_and_terminal_guards() {
    fixture::Fixture camera;
    pleora::Features view(*camera.map._Ptr);
    const auto envelope = take(profile::read_snapshot(view));
    auto request = take(profile::configuration_from(envelope));
    request.width = 300;
    request.frame_rate_hz = 25;
    request.exposure = 30ms;
    camera.port.fail_write = fixture::exposure;
    const auto failed = profile::apply_configuration(view, request, envelope, connected, ready);
    rejected(failed, backend::ErrorCode::io_error, "injected test port write failure");
    CHECK(contains(failed.error().message, "earlier successful writes: Width=300, AcquisitionFrameRate=25"));
    CHECK(contains(failed.error().message, "no rollback was attempted"));
    CHECK(contains(failed.error().message, "actual native configuration:"));
    CHECK(camera.port.integer(fixture::width) == 300 && camera.port.real(fixture::rate) == 25);
    CHECK(camera.port.real(fixture::exposure) == 1000);
    CHECK(writes(camera.port) == std::vector<std::int64_t>({fixture::width, fixture::rate, fixture::exposure}));
    camera.port.fail_write.reset();
    camera.port.calls.clear();
    const auto guarded = profile::apply_configuration(view, request, envelope, connected,
        [](std::string_view) -> backend::Status {
            return backend::Error{backend::ErrorCode::disconnected, "exact terminal write guard"};
        });
    rejected(guarded, backend::ErrorCode::disconnected);
    CHECK(guarded.error().message == "exact terminal write guard");
    CHECK(camera.port.write_attempts() == 0);

    camera.port.fail_read = fixture::width;
    int checks = 0;
    const auto lost = profile::read_configuration(view, envelope, [&]() -> backend::Status {
        if (++checks == 2) return backend::Error{backend::ErrorCode::disconnected, "terminal loss wins"};
        return {};
    });
    rejected(lost, backend::ErrorCode::disconnected);
    CHECK(lost.error().message == "terminal loss wins" && checks == 2);
}

} // namespace

int main() {
    try {
        fixture::Fixture version;
        if (auto* node = version.map._GetNode("Sys::GenApiVersion"))
            std::cout << "Bundled GenApi runtime: " << native::CValuePtr(node)->ToString() << '\n';
        const std::pair<const char*, void (*)()> groups[] = {
            {"eBUS parameter array and native status mapping", actual_ebus_parameter_array_and_result_translation},
            {"integer type/access/current grid", integer_type_access_and_live_grid},
            {"native float increment modes", float_native_increment_modes_and_checked_writes},
            {"absence, native query errors and aliases", absence_native_query_errors_and_alias_selection},
            {"strings, enums, booleans and entry availability", string_enum_boolean_facts_and_entry_errors},
            {"cache freshness and dynamic write constraints", cache_freshness_and_dynamic_constraint_order},
            {"unchanged shared profile and actual readback", unchanged_profile_prevalidation_and_truthful_readback},
            {"partial writes and terminal guards", partial_write_native_failure_and_terminal_guards}};
        for (const auto& [name, test] : groups) {
            test();
            std::cout << "PASS " << name << '\n';
        }
        std::cout << "Passed 8 actual-runtime offline groups; no camera/stream/MEL hardware qualification.\n";
        return 0;
    } catch (const PV_GENICAM_NS::GenericException& error) {
        std::cerr << "GenApi fixture failed: " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Pleora SDK contract failed: " << error.what() << '\n';
    }
    return 1;
}
