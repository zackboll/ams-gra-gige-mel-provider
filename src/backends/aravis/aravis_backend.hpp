#pragma once

// Optional, internal Aravis camera-control foundation (GIGE-MEL-008). Built
// only with -DGIGE_MEL_BUILD_ARAVIS=ON; never installed or exported, and not
// selectable through getAPI_Manager(). Deliberately SDK-type free so callers
// need no Aravis/GLib include paths. Acquisition is NOT implemented:
// create_stream() always returns ErrorCode::unsupported.

#include <gige_mel/backend.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gige_mel::backend::aravis {

inline constexpr std::string_view backend_id = "aravis";

// Ordinary factory: Aravis's GigE Vision interface only. Construction performs
// no discovery, open or configuration I/O and changes no process-wide Aravis
// state (no interface enable flags, never arv_shutdown()).
std::shared_ptr<ICameraBackend> make_backend();

// Version of the Aravis library actually loaded at run time ("0.8.36"), and
// the version the adapter was compiled against.
std::string runtime_version();
std::string compiled_version();

// Read-only inspection used by the diagnostic tool. Opens exactly device_id
// under the same ownership policy as open(), reads native features, and never
// writes a camera feature or starts acquisition. The backend must come from
// this adapter (any interface source).
struct Inspection {
    std::string device_id;
    std::string interface_protocol;
    std::vector<std::pair<std::string, std::string>> native; // feature -> value/bounds text
    std::optional<CameraCapabilities> capabilities;            // set only for a supported profile
    std::optional<CameraConfiguration> configuration;          // set only when truthfully representable
    std::vector<std::string> problems;                         // precise unsupported/unrepresentable reasons
};
Result<Inspection> inspect(ICameraBackend& backend, std::string_view device_id);

} // namespace gige_mel::backend::aravis
