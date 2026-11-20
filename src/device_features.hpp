#pragma once

// Internal, SDK-free device feature access for camera profiles. This is a
// C++ seam, not an installed API or a stable cross-toolchain plugin ABI.

#include <gige_mel/backend.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gige_mel::backend::device_features {

/// Native integer value and current inclusive bounds and step. These are
/// observations, not profile capabilities. For an ordinary fixed grid, valid
/// writes are spaced by the increment from the minimum; the profile decides
/// which constraints it can support (including rejecting unsuitable steps).
struct IntegerFeature {
    std::int64_t value{0};
    std::int64_t minimum{0};
    std::int64_t maximum{0};
    std::int64_t increment{0};
};

/// Native floating-point value and current inclusive bounds, in native units.
/// An absent increment means no step is advertised, not a zero step or proof
/// that hardware will store the exact value. Present invalid facts stay visible
/// to policy. Unit text is copied when declared; absence implies no conversion.
struct FloatFeature {
    double value{0};
    double minimum{0};
    double maximum{0};
    std::optional<double> increment; // only when the camera advertises one
    std::optional<std::string> unit; // declared native unit, when present
};

/// Access required at the time of a numeric read. Neither option writes.
enum class ReadAccess {
    readable,   ///< Require read access only.
    read_write ///< Also require current write access, e.g. for a write plan.
};

/// @brief Borrowed typed access to the adapter's device, not camera-model policy.
/// The adapter keeps the device alive and serializes the whole invocation
/// (snapshot, plan, ordered writes and readback), or owns exclusive access
/// during open. A profile must not retain the service past that invocation.
/// No SDK nodes/handles cross the seam; names are borrowed only for the call
/// and returned data is owned. No feature alias selection or unit conversion
/// belongs here. A successful read can still be unrepresentable or unsupported
/// by profile policy. Current unavailability, access denial and native query
/// errors must not be treated as confirmation that an optional feature is absent.
class IDeviceFeatures {
public:
    virtual ~IDeviceFeatures() = default;

    /// Query native implementation, not availability. False means confirmed
    /// absent/unimplemented; a failed query returns an error. True does not
    /// promise current availability, access or the expected typed interface.
    virtual Result<bool> implemented(std::string_view name) = 0;

    /// Read the current integer value only; require an available, readable integer.
    virtual Result<std::int64_t> read_integer(std::string_view name) = 0;

    /// @brief Read a native integer and its current minimum, maximum and increment.
    /// @param[in] name Exact native feature name.
    /// @param[in] access Require read access, optionally also write access.
    /// @return Owned facts without clamping or policy validation, or a diagnostic
    /// for missing/unavailable/wrong-type/inaccessible features or native failure.
    /// Do not assume those failures have distinct backend error codes.
    virtual Result<IntegerFeature> read_integer_feature(std::string_view name, ReadAccess access) = 0;

    /// Read an available float with the requested access, including its current
    /// bounds, optional step and native unit. Normalize SDK-specific no-increment
    /// representations, not invalid advertised numeric constraints. Propagate failures.
    virtual Result<FloatFeature> read_float_feature(std::string_view name, ReadAccess access) = 0;

    /// Copy the current value of a readable string or symbolic enumeration;
    /// fail on unavailable/inaccessible features, wrong type or native error.
    virtual Result<std::string> read_string_or_enum(std::string_view name) = 0;

    /// Read a currently available and readable Boolean; do not coerce other types.
    virtual Result<bool> read_boolean(std::string_view name) = 0;

    /// Query an exact entry of an available, readable enumeration. False means
    /// the entry is absent or unavailable, not that the enumeration query failed.
    /// This does not select the entry or establish that the enumeration is writable.
    virtual Result<bool> enum_entry_available(std::string_view name, std::string_view entry) = 0;

    /// @brief Set only the named integer to the exact requested native value.
    /// @param[in] name Exact native feature name.
    /// @param[in] value Requested native integer, not a value to round or clamp.
    /// @return Native setter success or an admission/access/native diagnostic
    /// retaining backend provenance. Recheck availability, read/write access,
    /// type and native constraints at the write, not against stale planning facts.
    /// @note No implicit feature changes, service retries or automatic rollback.
    /// Success is not readback and does not certify profile-level admission.
    virtual Status write_integer(std::string_view name, std::int64_t value) = 0;

    /// Write only the named float in native units with fresh availability,
    /// read/write access, type and native constraint checks. Do not clamp or
    /// adjust another feature. Report native failure; native quantization can
    /// still affect the actual readback. No service retries or rollback.
    virtual Status write_float(std::string_view name, double value) = 0;
};

} // namespace gige_mel::backend::device_features
