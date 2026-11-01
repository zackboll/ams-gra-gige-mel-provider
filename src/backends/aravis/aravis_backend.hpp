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
#include <functional>
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

// ---------------------------------------------------------------------------
// Native timestamp OBSERVATION (GIGE-MEL-010). Observation is not
// qualification: nothing below feeds FrameDescriptor::timestamp, which stays
// FrameTimestamp{} (valid == false) for every Aravis frame. The values are the
// SDK's own unsigned 64-bit results, copied without any signed conversion,
// arithmetic, relabelling or mutation of the ArvBuffer.

// SDK facts for ONE successfully received and validated buffer. Read on the
// thread that called receive (never on the SDK receiver thread), after the
// production validator accepted the buffer and before its lease is returned.
struct NativeTimestampObservation {
    std::uint64_t stream_generation{0};
    std::uint64_t native_frame_id{0};          // arv_buffer_get_frame_id()
    std::uint32_t width{0}, height{0};         // arv_buffer_get_image_region() (validated == frozen ROI)
    std::uint32_t offset_x{0}, offset_y{0};
    std::uint64_t sdk_buffer_timestamp_ns{0};  // arv_buffer_get_timestamp()
    std::uint64_t sdk_system_timestamp_ns{0};  // arv_buffer_get_system_timestamp()
};
struct ObservedFrame {
    FrameLease frame;                    // the ordinary lease (same owner, same release/requeue)
    NativeTimestampObservation native;   // facts read from that lease's SDK buffer
};
// Exactly stream.receive(timeout) (same deadline, validation, pool accounting,
// lease ownership and queue/release order), additionally returning the
// observation for the buffer behind the returned lease. The stream must come
// from this adapter. No callback is installed and no state is shared: the
// observation travels only in this call's result.
Result<ObservedFrame> receive_with_native_timestamps(IFrameStream& stream, std::chrono::milliseconds timeout);

// Native device kind, from the GObject type of the open native device (not
// from a device ID or a timestamp value).
enum class NativeInterface { gige_vision, fake, other };
// Read-only timing context of an open adapter device: identity features the
// device implements, and - for an ArvGvDevice only - the timestamp tick
// frequency through the supported arv_gv_device_get_timestamp_tick_frequency()
// (GVBS bootstrap registers 0x093C/0x0940). Writes nothing; starts nothing.
// A failed identity or frequency query is an error, never treated as absence.
// GIGE-MEL-010-R1: tick_frequency_hz is THIS separate diagnostic read, made
// before any stream exists. The pinned SDK stream reads the frequency again,
// itself, during stream construction (src/arvgvstream.c:1613, error ignored),
// so this value does not establish which timestamp path the stream used.
struct TimingContext {
    NativeInterface native_interface{NativeInterface::other};
    std::string native_device_type;                            // e.g. "ArvGvDevice", "ArvFakeDevice"
    std::vector<std::pair<std::string, std::string>> identity; // implemented identity features only
    std::optional<std::uint64_t> tick_frequency_hz;            // GigE Vision devices only (diagnostic read)
};
Result<TimingContext> read_timing_context(ICameraDevice& device);

// Bounded, read-only timestamp investigation used by the diagnostic
// `time-probe` command and, with the private Fake backend, by tests (the SAME
// routine). Validates every argument and creates output_path EXCLUSIVELY (an
// existing file is never overwritten) before any device I/O. Then opens
// exactly device_id on `backend`, reads its current configuration and timing
// context (no feature write), starts one acquisition, receives `frames`
// validated Mono8 frames within ONE overall `deadline`, releasing each lease
// immediately, stops and closes. The JSONL report is written only after
// cleanup; on any failure (including cleanup) the output file is removed.
// GIGE-MEL-010-R1: ownership of the exclusively created output and of the
// opened device/stream is held by RAII guards from the moment each exists,
// so an exception from any later step (open, acquisition, rendering,
// writing) also retires the native resources and removes the output. An
// exception is reported through `error`; a cleanup failure stays separate in
// `cleanup_error` and never replaces the first failure.
struct TimeProbeFrame {
    NativeTimestampObservation native;
    bool descriptor_timestamp_valid{false}; // the production FrameDescriptor value (always false)
};
struct TimeProbeReport {
    std::string device_id;
    std::string compiled_sdk_version, runtime_sdk_version;
    TimingContext context;
    CameraConfiguration configuration; // current readback used for the run (nothing written)
    std::vector<TimeProbeFrame> frames;
    StreamStatistics statistics;
    std::size_t output_bytes{0};
    std::size_t output_lines{0};
};
struct TimeProbeOutcome {
    std::optional<TimeProbeReport> report; // set only when the report file was completely written
    std::optional<Error> error;            // first failure (arguments/output/open/start/receive)
    std::optional<Error> cleanup_error;    // stop/close failure, reported separately
    [[nodiscard]] bool ok() const noexcept { return report && !error && !cleanup_error; }
};
inline constexpr std::uint32_t time_probe_max_frames = 32;
// The one overall receive deadline for all frames; also its maximum.
inline constexpr std::chrono::milliseconds time_probe_max_deadline{10000};
// Every JSONL line is at most this long; a longer record is an error.
inline constexpr std::size_t time_probe_max_line_bytes = 4096;
TimeProbeOutcome time_probe(ICameraBackend& backend, std::string_view device_id, std::uint32_t frames,
                            const std::string& output_path,
                            std::chrono::milliseconds deadline = time_probe_max_deadline);

// Wording written by the probe (also asserted by tests).
std::string_view timing_origin(NativeInterface native_interface);
std::string_view timestamp_qualification_reason();

// GIGE-MEL-010-R1. The PRODUCTION JSONL serializer used by time_probe(),
// declared here only so the adapter's own tests exercise exactly that code
// with synthetic contexts (docs/aravis-integration.md, schema version 2).
inline constexpr std::uint64_t time_probe_schema_version = 2;
// Exact JSON string literal of well-formed UTF-8 (Unicode Table 3-7): valid
// text is copied unchanged except for the escapes JSON needs (quote,
// backslash, U+0000-U+001F) and DEL. Malformed input (stray or missing
// continuation byte, overlong form, surrogate, above U+10FFFF, truncated
// sequence) is an error; it is never replaced, re-encoded or guessed.
Result<std::string> time_probe_json_string(std::string_view text);
// Header, one line per frame, summary. Returns an error - never a substituted
// value - naming the field that is not well-formed UTF-8, or rejecting an
// inconsistent timing context (a GigE context without its diagnostic
// frequency read result; a frequency for a device that is not an ArvGvDevice).
Result<std::vector<std::string>> render_time_probe_report(const TimeProbeReport& report,
                                                          std::chrono::milliseconds deadline,
                                                          std::uint32_t requested_frames);

// TEST-ONLY fault seam (GIGE-MEL-010-R1). Not reachable from the diagnostic
// CLI, which calls only time_probe(). `checkpoint` runs at named production
// boundaries of the SAME routine: "opened" (device open, nothing read yet),
// "started" (acquisition running), "render" (native cleanup finished, before
// serialization), "written" (header line written, before the rest). It may
// throw to exercise exception cleanup; it must not touch the device or file.
struct TimeProbeTestFaults {
    std::function<void(std::string_view stage)> checkpoint;
};
TimeProbeOutcome time_probe_with_faults_for_testing(ICameraBackend& backend, std::string_view device_id,
                                                    std::uint32_t frames, const std::string& output_path,
                                                    std::chrono::milliseconds deadline,
                                                    const TimeProbeTestFaults& faults);

} // namespace gige_mel::backend::aravis
