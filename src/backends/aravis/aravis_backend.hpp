#pragma once

// Optional, internal Aravis adapter: camera control (GIGE-MEL-008) and bounded
// Mono8 source acquisition (GIGE-MEL-009). Built only with
// -DGIGE_MEL_BUILD_ARAVIS=ON; never installed or exported, and not selectable
// through getAPI_Manager(). Deliberately SDK-type free so callers need no
// Aravis/GLib include paths. Frames carry FrameTimestamp{} (unqualified), so
// the common MEL provider rejects them; see docs/aravis-integration.md.

#include <gige_mel/backend.hpp>

#include <chrono>
#include <cstdint>
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

// Bounded direct capture used by the diagnostic `capture` command and, with
// the private Fake backend, by tests (the SAME routine). Opens exactly
// device_id on `backend`, uses the current supported and representable
// configuration (no feature write), starts one acquisition, waits for one
// validated Mono8 frame until `deadline`, writes a tightly packed binary PGM
// (P5, maxval 255; rows copied without source stride padding) to
// output_path, then stops and closes. Cleanup runs on every path. Device
// I/O starts only after the arguments are validated.
struct CaptureReport {
    std::string device_id;
    CameraConfiguration configuration;  // frozen readback for the frame
    std::uint32_t width{0}, height{0};
    std::size_t source_row_stride{0};
    std::uint64_t native_frame_id{0};
    std::uint64_t stream_generation{0};
    bool timestamp_valid{false};        // always false in GIGE-MEL-009
    std::size_t pgm_bytes{0};           // total file size written
    StreamStatistics statistics;
};
struct CaptureOutcome {
    std::optional<CaptureReport> report; // set when a frame was captured and written
    std::optional<Error> error;          // first failure (start/receive/output)
    std::optional<Error> cleanup_error;  // stop/close failure, reported separately
    [[nodiscard]] bool ok() const noexcept { return report && !error && !cleanup_error; }
};
inline constexpr std::chrono::milliseconds default_capture_deadline{5000};
CaptureOutcome capture_one(ICameraBackend& backend, std::string_view device_id, const std::string& output_path,
                           std::chrono::milliseconds deadline = default_capture_deadline);

} // namespace gige_mel::backend::aravis
