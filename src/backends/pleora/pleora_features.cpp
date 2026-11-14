#include "pleora_features.hpp"

#include <PvGenCommand.h>
#include <PvGenParameterArray.h>
#include <PvResult.h>

#include <GenApi/IBoolean.h>
#include <GenApi/IEnumEntry.h>
#include <GenApi/IEnumeration.h>
#include <GenApi/IFloat.h>
#include <GenApi/IInteger.h>
#include <GenApi/INode.h>
#include <GenApi/INodeMap.h>
#include <GenApi/IString.h>
#include <Base/GCException.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace gige_mel::backend::pleora {
namespace {

namespace native = PV_GENAPI_NS;
namespace exceptions = PV_GENICAM_NS;

std::string context(std::string_view operation, std::string_view feature) {
    std::string message = "Pleora " + std::string(operation);
    if (!feature.empty()) message += " '" + std::string(feature) + "'";
    return message;
}

Error unsupported(std::string_view feature, std::string reason) {
    return {ErrorCode::unsupported, context("feature", feature) + " " + std::move(reason)};
}

Status check_name(std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos)
        return Error{ErrorCode::invalid_argument, "Pleora feature names must be nonempty and contain no NUL byte"};
    return {};
}

Error translate_exception(const exceptions::GenericException& error,
                          std::string_view operation, std::string_view feature) {
    ErrorCode code = ErrorCode::io_error;
    if (dynamic_cast<const exceptions::TimeoutException*>(&error)) code = ErrorCode::timeout;
    else if (dynamic_cast<const exceptions::OutOfRangeException*>(&error) ||
             dynamic_cast<const exceptions::InvalidArgumentException*>(&error)) code = ErrorCode::invalid_argument;
    else if (dynamic_cast<const exceptions::AccessException*>(&error) ||
             dynamic_cast<const exceptions::DynamicCastException*>(&error)) code = ErrorCode::unsupported;
    else if (dynamic_cast<const exceptions::BadAllocException*>(&error)) code = ErrorCode::resource_exhausted;
    const char* detail = error.what();
    return {code, context(operation, feature) + ": " + (detail ? detail : "unknown GenApi error") + " [GenApi 3.4]"};
}

// eBUS normally returns PvResult. Its documented advanced GetNode() interface
// uses GenApi exceptions instead; none may cross the project-owned seam.
// Ordinary C++ allocation failures retain the backend contract's usual policy.
template <class Operation>
auto guarded(std::string_view operation, std::string_view name, Operation&& invoke) -> decltype(invoke()) {
    try {
        return invoke();
    } catch (const exceptions::GenericException& error) {
        return translate_exception(error, operation, name);
    }
}

template <class T>
Result<T*> typed(native::INode* parameter, native::EInterfaceType expected, std::string_view name,
                 std::string_view description) {
    const auto actual = parameter->GetPrincipalInterfaceType();
    if (actual != expected)
        return unsupported(name, "is not " + std::string(description) + " (native type " +
                                     std::to_string(static_cast<int>(actual)) + ")");
    auto* result = dynamic_cast<T*>(parameter);
    if (!result) return unsupported(name, "does not expose its reported native GenApi interface");
    return result;
}

Result<std::int64_t> integer_increment(native::IInteger& value, std::string_view name) {
    const auto mode = value.GetIncMode();
    if (mode == native::listIncrement)
        return unsupported(name, "uses a list of integer values; the current profile requires a scalar increment");
    if (mode != native::noIncrement && mode != native::fixedIncrement)
        return unsupported(name, "has an unknown native integer increment mode " +
                                     std::to_string(static_cast<int>(mode)));
    // Read the actual GenApi fact, including its defined default for integer
    // nodes. Do not synthesize a grid from a UI presentation suggestion.
    return value.GetInc();
}

Result<std::optional<double>> float_increment(native::IFloat& value, std::string_view name) {
    const auto mode = value.GetIncMode();
    if (mode == native::listIncrement)
        return unsupported(name, "uses a list of floating-point values; the current profile requires a scalar increment");
    if (mode != native::noIncrement && mode != native::fixedIncrement)
        return unsupported(name, "has an unknown native float increment mode " +
                                     std::to_string(static_cast<int>(mode)));
    const bool present = value.HasInc();
    if (present != (mode == native::fixedIncrement))
        return Error{ErrorCode::io_error, context("feature", name) + " returned inconsistent increment mode and HasInc facts"};
    if (!present) return std::optional<double>{};
    // Keep even an invalid advertised value for shared profile validation.
    return std::optional<double>{value.GetInc()};
}

Status integer_request(native::IInteger& value, std::string_view name, std::int64_t requested) {
    const auto minimum = value.GetMin();
    const auto maximum = value.GetMax();
    const auto increment = integer_increment(value, name);
    if (!increment) return increment.error();
    if (minimum > maximum || increment.value() <= 0)
        return unsupported(name, "advertises invalid current integer bounds/increment");
    if (requested < minimum || requested > maximum)
        return Error{ErrorCode::invalid_argument, context("write", name) + " value " + std::to_string(requested) +
                     " is outside current native bounds [" + std::to_string(minimum) + ", " +
                     std::to_string(maximum) + "]; nothing was written"};
    // Unsigned subtraction represents the nonnegative mathematical distance
    // even for a native range spanning INT64_MIN through INT64_MAX.
    const auto distance = static_cast<std::uint64_t>(requested) - static_cast<std::uint64_t>(minimum);
    if (distance % static_cast<std::uint64_t>(increment.value()) != 0)
        return Error{ErrorCode::invalid_argument, context("write", name) + " value " + std::to_string(requested) +
                     " is not on the current native integer grid; nothing was written"};
    return {};
}

Status float_request(native::IFloat& value, std::string_view name, double requested) {
    if (!std::isfinite(requested))
        return Error{ErrorCode::invalid_argument, context("write", name) + " requires a finite value; nothing was written"};
    const double minimum = value.GetMin();
    const double maximum = value.GetMax();
    const auto increment = float_increment(value, name);
    if (!increment) return increment.error();
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum ||
        (increment.value() && (!std::isfinite(*increment.value()) || *increment.value() <= 0)))
        return unsupported(name, "advertises invalid current float bounds/increment");
    if (requested < minimum || requested > maximum)
        return Error{ErrorCode::invalid_argument, context("write", name) +
                     " value is outside the current native float bounds; nothing was written"};
    if (increment.value()) {
        const double steps = (requested - minimum) / *increment.value();
        const double rounded = std::nearbyint(steps);
        // The same representational tolerance used by the shared profile;
        // this is validation only and never changes the requested value.
        if (!std::isfinite(steps) || std::fabs(steps - rounded) >
                4.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::fabs(rounded)))
            return Error{ErrorCode::invalid_argument, context("write", name) +
                         " value is not on the current native float grid; nothing was written"};
    }
    return {};
}

} // namespace

Error native_error(const PvResult& r, std::string_view operation) {
    auto code = ErrorCode::io_error;
    switch (r.GetCode()) {
    case PvResult::Code::TIMEOUT:
    case PvResult::Code::PENDING: code = ErrorCode::timeout; break;
    case PvResult::Code::NOT_CONNECTED: code = ErrorCode::disconnected; break;
    case PvResult::Code::NOT_FOUND: code = ErrorCode::not_found; break;
    case PvResult::Code::BUSY: code = ErrorCode::busy; break;
    case PvResult::Code::NOT_ENOUGH_MEMORY: code = ErrorCode::resource_exhausted; break;
    case PvResult::Code::INVALID_PARAMETER: code = ErrorCode::invalid_argument; break;
    case PvResult::Code::NOT_INITIALIZED:
    case PvResult::Code::STATE_ERROR: code = ErrorCode::invalid_state; break;
    case PvResult::Code::ABORTED:
    case PvResult::Code::CANCEL: code = ErrorCode::stopped; break;
    case PvResult::Code::NOT_SUPPORTED:
    case PvResult::Code::NOT_IMPLEMENTED:
    case PvResult::Code::NO_LICENSE: code = ErrorCode::unsupported; break;
    default: break;
    }
    return {code, "eBUS " + std::string(operation) + ": " + r.GetCodeString().GetAscii() +
                  " (" + std::to_string(r.GetCode()) + ") " + r.GetDescription().GetAscii()};
}
Status checked(const PvResult& r, std::string_view operation) {
    if (!r.IsOK()) return native_error(r, operation);
    return {};
}

Result<native::INode*> Features::parameter(std::string_view name, bool readable, bool writable) {
    if (const auto valid = check_name(name); !valid) return valid.error();
    if (!nodes_) return unsupported(name, "is not present in the empty native parameter array");
    nodes_->InvalidateNodes();
    auto* parameter = nodes_->GetNode(std::string(name).c_str());
    if (!parameter) return unsupported(name, "is not present");
    const auto mode = parameter->GetAccessMode();
    if (mode == native::NI) return unsupported(name, "is not implemented");
    if (mode == native::NA) return unsupported(name, "is not available");
    if (mode != native::RO && mode != native::WO && mode != native::RW)
        return Error{ErrorCode::io_error, context("feature", name) + " returned an undefined native access mode"};
    if (readable && mode != native::RO && mode != native::RW)
        return unsupported(name, "requires current read access");
    if (writable && mode != native::WO && mode != native::RW)
        return unsupported(name, "requires current write access");
    return parameter;
}

Result<bool> Features::implemented(std::string_view name) {
    return guarded("query implementation of", name, [&]() -> Result<bool> {
        if (const auto valid = check_name(name); !valid) return valid.error();
        if (!nodes_) return false; // default-constructed eBUS array has no nodes
        nodes_->InvalidateNodes();
        auto* feature = nodes_->GetNode(std::string(name).c_str());
        if (!feature) return false; // documented confirmed name absence
        const auto mode = feature->GetAccessMode();
        if (mode == native::NI) return false;
        if (mode == native::NA || mode == native::RO || mode == native::WO || mode == native::RW) return true;
        return Error{ErrorCode::io_error, context("feature", name) + " returned an undefined native access mode"};
    });
}

Result<std::int64_t> Features::read_integer(std::string_view name) {
    return guarded("read", name, [&]() -> Result<std::int64_t> {
        const auto feature = parameter(name, true, false);
        if (!feature) return feature.error();
        const auto integer = typed<native::IInteger>(feature.value(), native::intfIInteger, name, "an integer feature");
        if (!integer) return integer.error();
        return integer.value()->GetValue(false, true);
    });
}

Result<device_features::IntegerFeature> Features::read_integer_feature(
    std::string_view name, device_features::ReadAccess access) {
    return guarded("read integer value/bounds/increment of", name, [&]() -> Result<device_features::IntegerFeature> {
        const auto feature = parameter(name, true, access == device_features::ReadAccess::read_write);
        if (!feature) return feature.error();
        const auto integer = typed<native::IInteger>(feature.value(), native::intfIInteger, name, "an integer feature");
        if (!integer) return integer.error();
        device_features::IntegerFeature value;
        value.value = integer.value()->GetValue(false, true);
        value.minimum = integer.value()->GetMin();
        value.maximum = integer.value()->GetMax();
        const auto increment = integer_increment(*integer.value(), name);
        if (!increment) return increment.error();
        value.increment = increment.value();
        return value;
    });
}

Result<device_features::FloatFeature> Features::read_float_feature(
    std::string_view name, device_features::ReadAccess access) {
    return guarded("read float value/bounds/increment of", name, [&]() -> Result<device_features::FloatFeature> {
        const auto feature = parameter(name, true, access == device_features::ReadAccess::read_write);
        if (!feature) return feature.error();
        const auto real = typed<native::IFloat>(feature.value(), native::intfIFloat, name, "a float feature");
        if (!real) return real.error();
        device_features::FloatFeature value;
        value.value = real.value()->GetValue(false, true);
        value.minimum = real.value()->GetMin();
        value.maximum = real.value()->GetMax();
        const auto increment = float_increment(*real.value(), name);
        if (!increment) return increment.error();
        value.increment = increment.value();
        const auto unit = real.value()->GetUnit();
        std::string owned_unit(unit.c_str());
        if (!owned_unit.empty()) value.unit = std::move(owned_unit);
        return value;
    });
}

Result<std::string> Features::read_string_or_enum(std::string_view name) {
    return guarded("read", name, [&]() -> Result<std::string> {
        const auto feature = parameter(name, true, false);
        if (!feature) return feature.error();
        const auto type = feature.value()->GetPrincipalInterfaceType();
        if (type == native::intfIString) {
            const auto value = typed<native::IString>(feature.value(), type, name, "a string feature");
            if (!value) return value.error();
            return std::string(value.value()->GetValue(false, true).c_str());
        }
        if (type == native::intfIEnumeration) {
            const auto value = typed<native::IEnumeration>(feature.value(), type, name, "an enumeration");
            if (!value) return value.error();
            return std::string(value.value()->ToString(false, true).c_str());
        }
        return unsupported(name, "is not an enumeration or string");
    });
}

Result<bool> Features::read_boolean(std::string_view name) {
    return guarded("read", name, [&]() -> Result<bool> {
        const auto feature = parameter(name, true, false);
        if (!feature) return feature.error();
        const auto boolean = typed<native::IBoolean>(feature.value(), native::intfIBoolean, name, "a Boolean feature");
        if (!boolean) return boolean.error();
        return boolean.value()->GetValue(false, true);
    });
}

Result<bool> Features::enum_entry_available(std::string_view name, std::string_view entry) {
    return guarded("query selected enumeration entry of", name, [&]() -> Result<bool> {
        if (const auto valid = check_name(entry); !valid) return valid.error();
        const auto feature = parameter(name, true, false);
        if (!feature) return feature.error();
        const auto enumeration = typed<native::IEnumeration>(feature.value(), native::intfIEnumeration, name, "an enumeration");
        if (!enumeration) return enumeration.error();
        auto* selected = enumeration.value()->GetEntryByName(std::string(entry).c_str());
        if (!selected) return false;
        const auto mode = selected->GetAccessMode();
        if (mode == native::NI || mode == native::NA) return false;
        if (mode == native::RO || mode == native::WO || mode == native::RW) return true;
        return Error{ErrorCode::io_error, context("enumeration", name) + " returned an undefined entry access mode"};
    });
}

Status Features::write_integer(std::string_view name, std::int64_t value) {
    return guarded("write", name, [&]() -> Status {
        const auto feature = parameter(name, true, true);
        if (!feature) return feature.error();
        const auto integer = typed<native::IInteger>(feature.value(), native::intfIInteger, name, "an integer feature");
        if (!integer) return integer.error();
        if (const auto valid = integer_request(*integer.value(), name, value); !valid) return valid.error();
        // The actual native setter verifies access/range again. Only this one
        // named feature is written; no convenience setter or retry is used.
        integer.value()->SetValue(value, true);
        return {};
    });
}

Status Features::write_float(std::string_view name, double value) {
    return guarded("write", name, [&]() -> Status {
        const auto feature = parameter(name, true, true);
        if (!feature) return feature.error();
        const auto real = typed<native::IFloat>(feature.value(), native::intfIFloat, name, "a float feature");
        if (!real) return real.error();
        if (const auto valid = float_request(*real.value(), name, value); !valid) return valid.error();
        real.value()->SetValue(value, true);
        return {};
    });
}

Status Features::command(std::string_view name) {
    return guarded("execute command", name, [&]() -> Status {
        const auto feature = parameter(name, false, true);
        if (!feature) return feature.error();
        if (feature.value()->GetPrincipalInterfaceType() != native::intfICommand)
            return unsupported(name, "is not a command");
        if (!parameters_)
            return Error{ErrorCode::invalid_state, "eBUS acquisition commands require the native parameter array"};
        auto* command = parameters_->GetCommand(PvString(std::string(name).c_str()));
        if (!command) return unsupported(name, "has no eBUS command wrapper");
        return checked(command->Execute(), name);
    });
}

} // namespace gige_mel::backend::pleora
