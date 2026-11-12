#pragma once

// Private eBUS feature view. The caller owns the remote-device parameter
// array and holds its operation lock across the whole profile transaction.
// No SDK node or string is retained by the shared profile.

#include "device_features.hpp"

#include <PvGenApi.h>

class PvGenCommand;
class PvGenParameterArray;
class PvResult;

namespace gige_mel::backend::pleora {

// Preserve the SDK status and description in the diagnostic. This helper
// translates a FAILED native operation; the caller checks IsOK() first.
Error translate_result(const PvResult& result, std::string_view operation,
                       std::string_view feature = {});

class PleoraFeatures final : public device_features::IDeviceFeatures {
public:
    explicit PleoraFeatures(PvGenParameterArray& parameters);
    // The same native implementation can be qualified without a camera using
    // an original XML fixture and the SDK's actual GenApi runtime. This view
    // still borrows its node map; it cannot expose eBUS acquisition commands.
    explicit PleoraFeatures(PV_GENAPI_NS::INodeMap& nodes) noexcept : nodes_(&nodes) {}

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

    // Validate only. The lifecycle owner marks the command outcome uncertain
    // immediately before invoking Execute() on the returned borrowed command.
    // The caller retains the device and its operation lock through execution.
    Result<PvGenCommand*> command(std::string_view name);

private:
    // Invalidate native caches before each access, then query current native
    // implementation, availability and access. Query exceptions stay errors.
    Result<PV_GENAPI_NS::INode*> parameter(std::string_view name, bool readable, bool writable);
    PV_GENAPI_NS::INodeMap* nodes_;
    PvGenParameterArray* parameters_{nullptr}; // only for the eBUS command wrapper
};

} // namespace gige_mel::backend::pleora
