#pragma once

// Private adapter implementation, never installed.
#include "device_features.hpp"
#include <PvGenParameterArray.h>
#include <PvResult.h>

namespace gige_mel::backend::pleora {

Error native_error(const PvResult& result, std::string_view operation);
Status checked(const PvResult& result, std::string_view operation);

class Features final : public device_features::IDeviceFeatures {
public:
    explicit Features(PvGenParameterArray& parameters) : parameters_(parameters) {}
    Result<bool> implemented(std::string_view name) override;
    Result<std::int64_t> read_integer(std::string_view name) override;
    Result<device_features::IntegerFeature> read_integer_feature(
        std::string_view name, device_features::ReadAccess access) override;
    Result<device_features::FloatFeature> read_float_feature(
        std::string_view name, device_features::ReadAccess access) override;
    Result<std::string> read_string_or_enum(std::string_view name) override;
    Result<bool> read_boolean(std::string_view name) override;
    Result<bool> enum_entry_available(std::string_view name, std::string_view entry) override;
    Status write_integer(std::string_view name, std::int64_t value) override;
    Status write_float(std::string_view name, double value) override;
    Status command(std::string_view name);
private:
    Result<PvGenParameter*> node(std::string_view name, PvGenType type, bool read, bool write);
    PvGenParameterArray& parameters_;
};

} // namespace gige_mel::backend::pleora
