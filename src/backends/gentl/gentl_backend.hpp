#pragma once

// GIGE-MEL-GENTL-002 internal GenTL ICameraBackend. Uninstalled, not in the
// default backend catalog, and not reachable from the public MEL factories.
// No GenTL, Aravis or GLib type crosses this header.
//
// Construction performs no producer loading, discovery or device I/O. The
// producer is loaded from the exact absolute .cti path on the first explicit
// enumerate()/open() and retained (process singleton) by this backend.

#include "profiles/camera_profile.hpp"

#include <gige_mel/backend.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gige_mel::backend::gentl_backend {

inline constexpr std::string_view backend_id = "gentl";

enum class DeviceAccess { read_only, control };

struct Options {
    std::string producer_path;                            // exact absolute .cti path; never searched
    DeviceAccess access{DeviceAccess::read_only};         // explicit; never escalated
    std::chrono::milliseconds discovery_timeout{1000};    // TLUpdateInterfaceList / IFUpdateDeviceList
    std::uint64_t max_xml_bytes{16u << 20};
};

// Reversible composite selection ID: "gentl1:<n>:<interface-id><device-id>"
// where <n> is the decimal byte length of the interface ID (no leading zeros).
// Any byte is permitted in either ID; nothing is escaped, guessed or prefix-matched.
std::string compose_device_id(std::string_view interface_id, std::string_view device_id);
struct DeviceSelection {
    std::string interface_id;
    std::string device_id;
};
Result<DeviceSelection> parse_device_id(std::string_view composite);

// The profile is authoritative for compatibility; no default fallback.
std::shared_ptr<ICameraBackend> make_backend(Options options, std::shared_ptr<const profiles::ICameraProfile> profile);

// Explicit read-only inspection of an opened GenTL device (diagnostics only).
struct Inspection {
    std::string xml_url;
    std::uint64_t xml_length{0};
    bool xml_sha1_verified{false};
    std::string schema;
    std::string skipped_locations;
    std::vector<std::pair<std::string, std::string>> profile_diagnostics;
};
// Returns facts for a device opened by THIS module's backend, else null.
std::optional<Inspection> inspect(const ICameraDevice&);

// Native sessions whose close could not be resolved; they (and their
// producer) are retained, never re-closed or unmapped, for the process lifetime.
// A retired session's device identity stays blocked (busy, no native call)
// even after its public device object and the backend are destroyed, and the
// producer then refuses every further operation (GIGE-MEL-GENTL-002-R1).
std::size_t quarantined_native_sessions() noexcept;

// Test-only (offline fixtures): runs `callback` against the device's live
// GenApi feature service inside one serialized transaction. It is the ONLY
// way the offline IDeviceFeatures write tests reach writes; ICameraDevice::
// apply() stays unsupported. Not compiled into non-testing builds.
#ifdef GIGE_MEL_GENTL_TESTING
Status with_features_for_testing(ICameraDevice&, const std::function<Status(device_features::IDeviceFeatures&)>&);

// Test-only deterministic allocation-failure injection. The next allocation
// at `point` throws std::bad_alloc once, then the seam disarms itself.
// Returns the previously armed point. None of these points is reached after
// native ownership has transferred into the public device object.
enum class AllocationPoint {
    none,
    native_chain,    // the chain owner itself (before any native call)
    retirement_node, // the preallocated quarantine node (before any native call)
    registry_entry,  // the open-device reservation (before any native call)
    inspection,      // device diagnostics, after profile admission (before transfer)
    device_object,   // the GenTLDevice/shared control block (before transfer)
};
AllocationPoint fail_allocation_for_testing(AllocationPoint point) noexcept;
#endif

} // namespace gige_mel::backend::gentl_backend
