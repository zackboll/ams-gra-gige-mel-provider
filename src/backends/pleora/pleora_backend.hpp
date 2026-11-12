#pragma once

// Optional, internal Pleora eBUS GigE Vision adapter. Never installed or
// selected by the standard MEL factories. The unchanged shared monochrome
// profile supplies camera policy. Production timestamps remain unqualified.

#include <gige_mel/backend.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gige_mel::backend::pleora {

inline constexpr std::string_view backend_id = "pleora";

// Construction performs no discovery or device I/O. enumerate() returns exact
// canonical MAC identifiers, "mac:aa:bb:cc:dd:ee:ff". open() refreshes discovery
// and requires one exact match; it never interprets IPs, aliases or user IDs.
std::shared_ptr<ICameraBackend> make_backend();
std::string compiled_version();
std::string runtime_version();

struct Inspection {
    std::string device_id;
    std::string interface_protocol;
    std::vector<std::pair<std::string, std::string>> native;
    std::optional<CameraCapabilities> capabilities;
    std::optional<CameraConfiguration> configuration;
    std::vector<std::string> problems;
};

// Exact-ID, read-only inspection under the same ownership policy as open().
// No camera feature is changed and acquisition is never started.
Result<Inspection> inspect(ICameraBackend& backend, std::string_view device_id);

} // namespace gige_mel::backend::pleora
