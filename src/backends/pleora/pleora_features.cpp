#include "pleora_features.hpp"

#include <PvGenBoolean.h>
#include <PvGenCommand.h>
#include <PvGenEnum.h>
#include <PvGenEnumEntry.h>
#include <PvGenFloat.h>
#include <PvGenInteger.h>
#include <PvGenString.h>

#include <cmath>

namespace gige_mel::backend::pleora {
namespace {
PvString name_of(std::string_view name) { return PvString(std::string(name).c_str()); }
Error unavailable(std::string_view name, std::string_view why) {
    return {ErrorCode::unsupported, "eBUS " + std::string(name) + ": " + std::string(why)};
}
}

Error native_error(const PvResult& r, std::string_view operation) {
    auto code = ErrorCode::io_error;
    switch (r.GetCode()) {
    case PvResult::Code::TIMEOUT: code = ErrorCode::timeout; break;
    case PvResult::Code::NOT_CONNECTED: code = ErrorCode::disconnected; break;
    case PvResult::Code::NOT_FOUND: code = ErrorCode::not_found; break;
    case PvResult::Code::BUSY: code = ErrorCode::busy; break;
    case PvResult::Code::NOT_ENOUGH_MEMORY: code = ErrorCode::resource_exhausted; break;
    case PvResult::Code::INVALID_PARAMETER: code = ErrorCode::invalid_argument; break;
    case PvResult::Code::STATE_ERROR: code = ErrorCode::invalid_state; break;
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

Result<bool> Features::implemented(std::string_view name) {
    auto* p = parameters_.Get(name_of(name));
    if (!p) return false;
    bool value = false;
    if (auto s = checked(p->IsImplemented(value), name); !s) return s.error();
    return value;
}
Result<PvGenParameter*> Features::node(std::string_view name, PvGenType type, bool read, bool write) {
    auto* p = parameters_.Get(name_of(name));
    if (!p) return unavailable(name, "absent");
    bool value = false;
    if (auto s = checked(p->IsImplemented(value), name); !s) return s.error();
    if (!value) return unavailable(name, "not implemented");
    if (auto s = checked(p->IsAvailable(value), name); !s) return s.error();
    if (!value) return unavailable(name, "unavailable");
    PvGenType actual = PvGenTypeUndefined;
    if (auto s = checked(p->GetType(actual), name); !s) return s.error();
    if (actual != type) return unavailable(name, "wrong native type");
    if (read) {
        if (auto s = checked(p->IsReadable(value), name); !s) return s.error();
        if (!value) return unavailable(name, "not readable");
    }
    if (write) {
        if (auto s = checked(p->IsWritable(value), name); !s) return s.error();
        if (!value) return unavailable(name, "not writable");
    }
    return p;
}
Result<std::int64_t> Features::read_integer(std::string_view name) {
    auto p = node(name, PvGenTypeInteger, true, false);
    if (!p) return p.error();
    std::int64_t value = 0;
    if (auto s = checked(static_cast<PvGenInteger*>(p.value())->GetValue(value), name); !s) return s.error();
    return value;
}
Result<device_features::IntegerFeature> Features::read_integer_feature(
    std::string_view name, device_features::ReadAccess access) {
    auto p = node(name, PvGenTypeInteger, true, access == device_features::ReadAccess::read_write);
    if (!p) return p.error();
    auto* i = static_cast<PvGenInteger*>(p.value());
    device_features::IntegerFeature f;
    if (auto s = checked(i->GetValue(f.value), name); !s) return s.error();
    if (auto s = checked(i->GetMin(f.minimum), name); !s) return s.error();
    if (auto s = checked(i->GetMax(f.maximum), name); !s) return s.error();
    if (auto s = checked(i->GetIncrement(f.increment), name); !s) return s.error();
    return f;
}
Result<device_features::FloatFeature> Features::read_float_feature(
    std::string_view name, device_features::ReadAccess access) {
    auto p = node(name, PvGenTypeFloat, true, access == device_features::ReadAccess::read_write);
    if (!p) return p.error();
    auto* i = static_cast<PvGenFloat*>(p.value());
    device_features::FloatFeature f;
    if (auto s = checked(i->GetValue(f.value), name); !s) return s.error();
    if (auto s = checked(i->GetMin(f.minimum), name); !s) return s.error();
    if (auto s = checked(i->GetMax(f.maximum), name); !s) return s.error();
    PvString unit;
    if (auto s = checked(i->GetUnit(unit), name); !s) return s.error();
    if (unit.GetLength() != 0) f.unit = unit.GetAscii();
    // PvGenFloat exposes no increment query. Do not invent a grid or unit.
    return f;
}
Result<std::string> Features::read_string_or_enum(std::string_view name) {
    auto exists = implemented(name);
    if (!exists) return exists.error();
    if (!exists.value()) return unavailable(name, "absent");
    PvGenType type = PvGenTypeUndefined;
    if (auto s = checked(parameters_.Get(name_of(name))->GetType(type), name); !s) return s.error();
    if (type != PvGenTypeString && type != PvGenTypeEnum) return unavailable(name, "not string or enum");
    auto p = node(name, type, true, false);
    if (!p) return p.error();
    PvString text;
    auto result = type == PvGenTypeEnum ? static_cast<PvGenEnum*>(p.value())->GetValue(text)
                                       : static_cast<PvGenString*>(p.value())->GetValue(text);
    if (auto s = checked(result, name); !s) return s.error();
    return std::string(text.GetAscii());
}
Result<bool> Features::read_boolean(std::string_view name) {
    auto p = node(name, PvGenTypeBoolean, true, false);
    if (!p) return p.error();
    bool value = false;
    if (auto s = checked(static_cast<PvGenBoolean*>(p.value())->GetValue(value), name); !s) return s.error();
    return value;
}
Result<bool> Features::enum_entry_available(std::string_view name, std::string_view entry) {
    auto p = node(name, PvGenTypeEnum, true, false);
    if (!p) return p.error();
    const PvGenEnumEntry* e = nullptr;
    const auto r = static_cast<PvGenEnum*>(p.value())->GetEntryByName(name_of(entry), &e);
    if (r.GetCode() == PvResult::Code::NOT_FOUND) return false;
    if (auto s = checked(r, name); !s) return s.error();
    if (!e) return false;
    bool available = false;
    if (auto s = checked(e->IsAvailable(available), name); !s) return s.error();
    return available;
}
Status Features::write_integer(std::string_view name, std::int64_t value) {
    auto f = read_integer_feature(name, device_features::ReadAccess::read_write);
    if (!f) return f.error();
    if (value < f.value().minimum || value > f.value().maximum || f.value().increment <= 0 ||
        (static_cast<std::uint64_t>(value) - static_cast<std::uint64_t>(f.value().minimum)) %
            static_cast<std::uint64_t>(f.value().increment) != 0)
        return Error{ErrorCode::invalid_argument, "eBUS " + std::string(name) + ": outside current native grid"};
    return checked(parameters_.GetInteger(name_of(name))->SetValue(value), name);
}
Status Features::write_float(std::string_view name, double value) {
    auto f = read_float_feature(name, device_features::ReadAccess::read_write);
    if (!f) return f.error();
    if (!std::isfinite(value) || value < f.value().minimum || value > f.value().maximum)
        return Error{ErrorCode::invalid_argument, "eBUS " + std::string(name) + ": outside current native range"};
    return checked(parameters_.GetFloat(name_of(name))->SetValue(value), name);
}
Status Features::command(std::string_view name) {
    auto p = node(name, PvGenTypeCommand, false, true);
    if (!p) return p.error();
    return checked(static_cast<PvGenCommand*>(p.value())->Execute(), name);
}

} // namespace gige_mel::backend::pleora
