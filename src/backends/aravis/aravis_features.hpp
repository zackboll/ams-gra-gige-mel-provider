#pragma once

#include "device_features.hpp"

#include <arv.h>

namespace gige_mel::backend::aravis::detail {

// Borrowed native view used only while the caller owns the device lifetime
// and operation lock (or exclusive, unpublished open/inspection). This class
// supplies SDK mechanics; feature aliases and camera policy live in the profile.
class AravisDeviceFeatures final : public device_features::IDeviceFeatures {
public:
    explicit AravisDeviceFeatures(ArvDevice* device);
    Result<bool> implemented(std::string_view name) override;
    Result<std::int64_t> read_integer(std::string_view name) override;
    Result<device_features::IntegerFeature> read_integer_feature(std::string_view name,
        device_features::ReadAccess access) override;
    Result<device_features::FloatFeature> read_float_feature(std::string_view name,
        device_features::ReadAccess access) override;
    Result<std::string> read_string_or_enum(std::string_view name) override;
    Result<bool> read_boolean(std::string_view name) override;
    Result<bool> enum_entry_available(std::string_view name, std::string_view entry) override;
    Status write_integer(std::string_view name, std::int64_t value) override;
    Status write_float(std::string_view name, double value) override;

private:
    ArvDevice* const device_; // borrowed, never retained beyond the caller's scope
};

Result<std::optional<std::string>> read_optional_identity(ArvDevice* device, std::string_view name);

} // namespace gige_mel::backend::aravis::detail
