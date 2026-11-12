#pragma once

// Internal, SDK-free device feature access for camera profiles. This is a
// source-level seam, not an installed API or a runtime plugin ABI.

#include <gige_mel/backend.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gige_mel::backend::device_features {

struct IntegerFeature {
    std::int64_t value{0};
    std::int64_t minimum{0};
    std::int64_t maximum{0};
    std::int64_t increment{0};
};

struct FloatFeature {
    double value{0};
    double minimum{0};
    double maximum{0};
    std::optional<double> increment; // only when the camera advertises one
    std::optional<std::string> unit; // declared native unit, when present
};

// Configuration snapshots may require writable features even though reading
// them has no side effect. Availability and access are checked on each call.
enum class ReadAccess { readable, read_write };

// Callers retain the native device and serialize the ENTIRE profile operation
// (snapshot, plan, ordered writes and readback). Implementations do not retain
// names or return borrowed SDK data. No native node or handle crosses this seam.
class IDeviceFeatures {
public:
    virtual ~IDeviceFeatures() = default;

    // False means confirmed absent or not implemented, not unavailable or a
    // failed query. True does not promise availability, access or node type.
    virtual Result<bool> implemented(std::string_view name) = 0;

    // Scalar reads require current read access. Numeric feature reads also
    // return fresh bounds/increments without clamping or validating them as
    // profile policy. SDK-specific absent-increment sentinels are normalized
    // here; invalid advertised numeric facts remain visible to the profile.
    virtual Result<std::int64_t> read_integer(std::string_view name) = 0;
    virtual Result<IntegerFeature> read_integer_feature(std::string_view name, ReadAccess access) = 0;
    virtual Result<FloatFeature> read_float_feature(std::string_view name, ReadAccess access) = 0;
    virtual Result<std::string> read_string_or_enum(std::string_view name) = 0;
    virtual Result<bool> read_boolean(std::string_view name) = 0;
    virtual Result<bool> enum_entry_available(std::string_view name, std::string_view entry) = 0;

    // Each setter checks current availability, access, type and native bounds
    // at the write, including constraints changed by earlier writes. It writes
    // only the named feature; no implicit trigger/auto/format changes, retries
    // or rollback. Failures retain backend diagnostic provenance.
    virtual Status write_integer(std::string_view name, std::int64_t value) = 0;
    virtual Status write_float(std::string_view name, double value) = 0;
};

} // namespace gige_mel::backend::device_features
