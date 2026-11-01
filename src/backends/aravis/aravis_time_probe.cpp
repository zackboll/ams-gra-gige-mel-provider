// Bounded, read-only native timestamp investigation (GIGE-MEL-010). Shared by
// the diagnostic `time-probe` command (production GigE backend) and the tests
// (private Fake backend): the SAME routine. It OBSERVES SDK timestamp facts
// and never qualifies them: every record states timestamp_qualified=false,
// and the production FrameDescriptor timestamp stays FrameTimestamp{}.
// No SDK include is needed here.
//
// GIGE-MEL-010-R1 (schema version 2):
//  - the diagnostic's own tick-frequency read is reported as exactly that: a
//    separate read before stream creation, never proof of the value the SDK
//    stream sampled during its own construction, so the stream's actual
//    GigE timestamp path is reported as "unverified";
//  - strings are exact well-formed UTF-8; malformed input is an error;
//  - the exclusive output and the native device/stream are RAII-owned, so
//    exceptions also clean up.
#include "aravis_backend.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gige_mel::backend::aravis {
namespace {

Error fail(ErrorCode code, std::string message) { return Error{code, std::move(message)}; }

std::string hex_byte(unsigned char byte) {
    char text[8];
    std::snprintf(text, sizeof text, "0x%02x", static_cast<unsigned>(byte));
    return text;
}

Error malformed(std::size_t offset, std::string_view bytes, const char* reason) {
    std::string seen;
    for (const char c : bytes) seen += (seen.empty() ? "" : " ") + hex_byte(static_cast<unsigned char>(c));
    return fail(ErrorCode::io_error, "not well-formed UTF-8 at byte " + std::to_string(offset) + " (" + seen + "): " + reason);
}

std::string_view interface_name(NativeInterface native_interface) {
    switch (native_interface) {
    case NativeInterface::gige_vision: return "gige_vision";
    case NativeInterface::fake: return "fake";
    case NativeInterface::other: break;
    }
    return "other";
}

using Members = std::vector<std::pair<std::string_view, std::string_view>>;

// One JSON object built from already-encoded members. The first string that
// cannot be encoded exactly is remembered with its field name; done() then
// returns that error instead of a line.
class Line {
public:
    explicit Line(std::string_view record) : record_(record) { str("record", record); }
    Line& raw(std::string_view key, std::string_view encoded) {
        if (error_) return *this;
        auto name = time_probe_json_string(key);
        if (!name) return note(key, name.error());
        text_ += (text_.empty() ? "{" : ",") + name.value() + ":" + std::string(encoded);
        return *this;
    }
    Line& str(std::string_view key, std::string_view value) {
        if (error_) return *this;
        auto encoded = time_probe_json_string(value);
        if (!encoded) return note(key, encoded.error());
        return raw(key, encoded.value());
    }
    Line& u64(std::string_view key, std::uint64_t value) { return raw(key, std::to_string(value)); }
    Line& boolean(std::string_view key, bool value) { return raw(key, value ? "true" : "false"); }
    Line& null(std::string_view key) { return raw(key, "null"); }
    // Object of string members; member values are named "key.member".
    template <class Pairs> Line& strings(std::string_view key, const Pairs& members) {
        if (error_) return *this;
        std::string out = "{";
        for (const auto& [name, value] : members) {
            auto encoded_name = time_probe_json_string(name);
            if (!encoded_name) return note(std::string(key) + "." + std::string(name), encoded_name.error());
            auto encoded_value = time_probe_json_string(value);
            if (!encoded_value) return note(std::string(key) + "." + std::string(name), encoded_value.error());
            out += (out.size() > 1 ? "," : "") + encoded_name.value() + ":" + encoded_value.value();
        }
        return raw(key, out + "}");
    }
    Result<std::string> done() const {
        if (error_) return *error_;
        return text_ + "}\n";
    }

private:
    Line& note(std::string_view key, const Error& error) {
        error_ = fail(error.code, "time-probe report field '" + std::string(record_) + "." + std::string(key) + "' is " +
                                      error.message + "; the value is never substituted and no report is written");
        return *this;
    }
    std::string_view record_;
    std::string text_;
    std::optional<Error> error_;
};

std::string configuration_object(const CameraConfiguration& c) {
    return "{\"width\":" + std::to_string(c.width) + ",\"height\":" + std::to_string(c.height) +
           ",\"offset_x\":" + std::to_string(c.offset_x) + ",\"offset_y\":" + std::to_string(c.offset_y) +
           ",\"frame_rate_hz\":" + std::to_string(c.frame_rate_hz) +
           ",\"exposure_ns\":" + std::to_string(c.exposure.count()) + "}";
}

// ---- Timestamp provenance, from the pinned SDK source only (never inferred
// from the observed values, their intervals or magnitudes, and never from the
// diagnostic's separate tick-frequency read).

constexpr std::string_view unverified_path = "unverified";

std::string_view actual_stream_timestamp_path(NativeInterface native_interface) {
    return native_interface == NativeInterface::fake ? "pinned_fake_fill_host_wall_clock" : unverified_path;
}

std::string_view actual_stream_timestamp_path_basis(NativeInterface native_interface) {
    switch (native_interface) {
    case NativeInterface::fake:
        return "pinned SDK source: the in-process Fake stream fills every buffer with arv_fake_camera_fill_buffer() "
               "(src/arvfakestream.c:93; src/arvfakecamera.c:786-787); no GigE stream is constructed";
    case NativeInterface::gige_vision:
        return "not established by this diagnostic: the SDK stream chose its branch from its own tick-frequency "
               "read during stream construction, which this diagnostic cannot observe; neither "
               "diagnostic_tick_frequency_hz nor observed timestamp values, intervals or magnitudes identify it";
    case NativeInterface::other: break;
    }
    return "unknown native device type";
}

std::string_view buffer_timestamp_source(NativeInterface native_interface) {
    switch (native_interface) {
    case NativeInterface::fake: return "pinned SDK Fake fill: g_get_real_time() * 1000 (src/arvfakecamera.c:786)";
    case NativeInterface::gige_vision:
        return "conditional on the tick frequency the SDK stream read during its own construction (see "
               "sdk_stream_timestamp_paths); which branch this stream took is unverified";
    case NativeInterface::other: break;
    }
    return "unknown native device type";
}

const Members& gige_stream_timestamp_paths() {
    static const Members paths{
        {"stream_construction_frequency_source",
         "read once by the SDK itself while constructing the stream: arv_gv_device_get_timestamp_tick_frequency("
         "gv_device, NULL) (src/arvgvstream.c:1613, stored at :1640); a read error is ignored and yields 0"},
        {"stream_construction_frequency_nonzero",
         "GVSP leader ticks converted to nanoseconds by arv_gvsp_timestamp_to_ns() (src/arvgvstream.c:480-482); "
         "unchecked arithmetic; a conversion proves no integration start, epoch, UTC, PTP, continuity or accuracy"},
        {"stream_construction_frequency_zero",
         "the SDK substitutes its host reception wall-clock time g_get_real_time() * 1000 (src/arvgvstream.c:484); "
         "not camera time"},
    };
    return paths;
}

std::string_view diagnostic_tick_frequency_read(NativeInterface native_interface) {
    switch (native_interface) {
    case NativeInterface::gige_vision:
        return "separate read by this diagnostic through arv_gv_device_get_timestamp_tick_frequency() (GVBS "
               "0x093C/0x0940) before stream creation; NOT the value the SDK stream sampled during its own "
               "construction, which may differ or fail; not proof of the stream's timestamp path";
    case NativeInterface::fake: return "not read: the in-process Fake device is not an ArvGvDevice";
    case NativeInterface::other: break;
    }
    return "not read: no supported accessor for this device type";
}

std::string_view system_timestamp_source(NativeInterface native_interface) {
    switch (native_interface) {
    case NativeInterface::gige_vision:
        return "host g_get_real_time() * 1000 when the SDK processes the GVSP leader (src/arvgvstream.c:458)";
    case NativeInterface::fake: return "same value as the buffer timestamp (src/arvfakecamera.c:787)";
    case NativeInterface::other: break;
    }
    return "unknown native device type";
}

// ---- Exception-safe ownership.

// Owns ONLY a pathname this call created with fopen("wx"). Until commit()
// has written, flushed and closed it successfully, destruction (ordinary
// return or exception unwinding) closes the descriptor once and removes the
// path. A pre-existing file is never owned, so never removed. Never throws.
class ExclusiveOutput {
public:
    explicit ExclusiveOutput(const std::string& path) noexcept
        : path_(path), file_(std::fopen(path.c_str(), "wx")), error_number_(file_ ? 0 : errno), owned_(file_ != nullptr) {}
    ~ExclusiveOutput() { discard(); }
    ExclusiveOutput(const ExclusiveOutput&) = delete;
    ExclusiveOutput& operator=(const ExclusiveOutput&) = delete;

    [[nodiscard]] bool owned() const noexcept { return owned_; }
    [[nodiscard]] int error_number() const noexcept { return error_number_; }
    void write(std::string_view bytes) noexcept {
        written_ = written_ && file_ && std::fwrite(bytes.data(), 1, bytes.size(), file_) == bytes.size();
    }
    // Flush and close exactly once; only a complete success retains the file.
    [[nodiscard]] bool commit() noexcept {
        if (!file_) return false;
        const bool flushed = std::fflush(file_) == 0;
        const bool closed = std::fclose(std::exchange(file_, nullptr)) == 0;
        committed_ = written_ && flushed && closed;
        return committed_;
    }

private:
    void discard() noexcept {
        if (file_) (void)std::fclose(std::exchange(file_, nullptr));
        if (owned_ && !committed_) (void)std::remove(path_.c_str());
        owned_ = false;
    }
    const std::string& path_; // the caller's argument; outlives this guard
    std::FILE* file_;
    int error_number_;
    bool owned_;
    bool written_{true};
    bool committed_{false};
};

// Owns an opened logical device and, once created, its stream. finish()
// runs the established cleanup exactly once: stop() (noexcept; retires the
// native stream and pool through the adapter's contract) and destruction of
// the stream wrapper, then close() of the device. A close failure (e.g. a
// failed AcquisitionStop) is reported in cleanup_error, never replacing an
// earlier one. Never throws; the destructor is the backstop.
class NativeSession {
public:
    explicit NativeSession(std::shared_ptr<ICameraDevice> device) noexcept : device_(std::move(device)) {}
    ~NativeSession() {
        std::optional<Error> unreported; // only reached if a handler itself failed
        finish(unreported);
    }
    NativeSession(const NativeSession&) = delete;
    NativeSession& operator=(const NativeSession&) = delete;

    [[nodiscard]] ICameraDevice& device() const noexcept { return *device_; }
    void adopt(std::unique_ptr<IFrameStream> stream) noexcept { stream_ = std::move(stream); }
    [[nodiscard]] IFrameStream& stream() const noexcept { return *stream_; }

    void finish(std::optional<Error>& cleanup_error) noexcept {
        if (stream_) {
            stream_->stop();
            stream_.reset();
        }
        if (!device_) return;
        try {
            const auto owned = std::move(device_); // never closed twice by this owner
            if (auto closed = owned->close(); !closed && !cleanup_error) cleanup_error = closed.error();
        } catch (const std::exception& error) {
            note(cleanup_error, error.what());
        } catch (...) {
            note(cleanup_error, "unknown exception");
        }
    }

private:
    static void note(std::optional<Error>& cleanup_error, const char* what) noexcept {
        try {
            if (!cleanup_error) cleanup_error = fail(ErrorCode::io_error, std::string("device close threw: ") + what);
        } catch (...) {
            // Out of memory while describing it; the device was still released.
        }
    }
    std::shared_ptr<ICameraDevice> device_;
    std::unique_ptr<IFrameStream> stream_;
};

// Exception path: native cleanup first (the primary error and any cleanup
// error are kept separately), then the output guard removes the file.
void abandon(TimeProbeOutcome& outcome, std::optional<NativeSession>& session, const char* what) noexcept {
    const bool opened = session.has_value();
    if (session) session->finish(outcome.cleanup_error);
    try {
        if (!outcome.error)
            outcome.error = fail(ErrorCode::io_error, std::string("time probe failed: ") + what +
                                                          (opened ? "; the device was stopped and closed" : "") +
                                                          "; the output was removed");
    } catch (...) {
        // Out of memory while describing it; resources and output still released.
    }
}

TimeProbeOutcome probe(ICameraBackend& backend, std::string_view device_id, std::uint32_t frames,
                       const std::string& output_path, std::chrono::milliseconds deadline,
                       const TimeProbeTestFaults* faults) {
    TimeProbeOutcome outcome;
    // Strict argument validation, then exclusive output creation, all before
    // any device I/O.
    if (device_id.empty()) { outcome.error = fail(ErrorCode::invalid_argument, "DEVICE_ID must not be empty"); return outcome; }
    if (auto id = time_probe_json_string(device_id); !id) {
        outcome.error = fail(ErrorCode::invalid_argument,
                             "DEVICE_ID is recorded exactly, so it must be well-formed UTF-8; it is " + id.error().message);
        return outcome;
    }
    if (frames < 1 || frames > time_probe_max_frames) {
        outcome.error = fail(ErrorCode::invalid_argument, "COUNT must be in [1, " + std::to_string(time_probe_max_frames) + "]");
        return outcome;
    }
    if (deadline <= std::chrono::milliseconds::zero() || deadline > time_probe_max_deadline) {
        outcome.error = fail(ErrorCode::invalid_argument, "the receive deadline must be in (0, " +
                                                              std::to_string(time_probe_max_deadline.count()) + "] ms");
        return outcome;
    }
    if (output_path.empty()) { outcome.error = fail(ErrorCode::invalid_argument, "OUTPUT path must not be empty"); return outcome; }
    ExclusiveOutput output(output_path); // exclusive: never overwrite
    if (!output.owned()) {
        outcome.error = fail(ErrorCode::io_error, "cannot create output '" + output_path + "' exclusively: " +
                                                      std::strerror(output.error_number()) + "; no device I/O was performed");
        return outcome;
    }
    // From here the guard removes this call's output on every path that does
    // not commit it - ordinary returns and exceptions alike.
    const auto checkpoint = [faults](std::string_view stage) {
        if (faults && faults->checkpoint) faults->checkpoint(stage);
    };
    std::optional<NativeSession> session; // declared outside the try: the handlers finish it
    try {
        auto opened = backend.open(device_id);
        if (!opened) { outcome.error = opened.error(); return outcome; }
        session.emplace(std::move(opened).value());
        checkpoint("opened");

        TimeProbeReport report;
        report.device_id = std::string(device_id);
        report.compiled_sdk_version = compiled_version();
        report.runtime_sdk_version = runtime_version();
        const auto acquire = [&]() -> Status {
            auto& device = session->device();
            auto configuration = device.configuration();
            if (!configuration) return configuration.error();
            report.configuration = configuration.value();
            auto context = read_timing_context(device);
            if (!context) return context.error();
            report.context = std::move(context).value();
            auto created = device.create_stream();
            if (!created) return created.error();
            session->adopt(std::move(created).value()); // owned before start
            auto& stream = session->stream();
            if (auto started = stream.start(1); !started) return started.error();
            checkpoint("started");
            const auto end = std::chrono::steady_clock::now() + deadline; // ONE overall deadline
            while (report.frames.size() < frames) {
                const auto now = std::chrono::steady_clock::now();
                const auto remaining = now >= end ? std::chrono::milliseconds::zero()
                                                  : std::chrono::ceil<std::chrono::milliseconds>(end - now);
                auto received = receive_with_native_timestamps(stream, remaining);
                if (!received)
                    return fail(received.error().code, received.error().message + " (after " +
                                                           std::to_string(report.frames.size()) + " of " +
                                                           std::to_string(frames) + " frames)");
                const auto& observed = received.value();
                report.frames.push_back({observed.native, observed.frame.descriptor().timestamp.valid});
                // `received` (the lease and its SDK buffer) is released here.
            }
            report.statistics = stream.statistics();
            return {};
        };
        if (auto acquired = acquire(); !acquired) outcome.error = acquired.error();
        session->finish(outcome.cleanup_error); // stop, retire, close - before any output
        if (outcome.error || outcome.cleanup_error) return outcome;

        checkpoint("render");
        auto lines = render_time_probe_report(report, deadline, frames);
        if (!lines) {
            outcome.error = fail(lines.error().code, lines.error().message + "; the output was removed");
            return outcome;
        }
        for (const auto& line : lines.value()) {
            if (line.size() > time_probe_max_line_bytes) {
                outcome.error = fail(ErrorCode::io_error, "a report line exceeds " + std::to_string(time_probe_max_line_bytes) +
                                                              " bytes; the output was removed");
                return outcome;
            }
        }
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < lines.value().size(); ++i) {
            output.write(lines.value()[i]);
            bytes += lines.value()[i].size();
            if (i == 0) checkpoint("written"); // the header line is in the file; the rest is not
        }
        if (!output.commit()) {
            outcome.error = fail(ErrorCode::io_error, "writing output '" + output_path + "' failed; the partial file was removed");
            return outcome;
        }
        report.output_bytes = bytes;
        report.output_lines = lines.value().size();
        outcome.report = std::move(report);
    } catch (const std::exception& error) {
        abandon(outcome, session, error.what());
    } catch (...) {
        abandon(outcome, session, "unknown exception");
    }
    return outcome;
}

} // namespace

Result<std::string> time_probe_json_string(std::string_view text) {
    std::string out = "\"";
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            switch (lead) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (lead < 0x20 || lead == 0x7f) { // U+0000-U+001F (incl. NUL) and DEL: exact \u escapes
                    char escaped[8];
                    std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(lead));
                    out += escaped;
                } else {
                    out += static_cast<char>(lead);
                }
            }
            ++i;
            continue;
        }
        // Well-formed sequences, Unicode Table 3-7: the second byte's range
        // depends on the lead byte, which excludes overlong forms (E0, F0),
        // UTF-16 surrogates (ED) and code points above U+10FFFF (F4).
        std::size_t length = 0;
        unsigned char low = 0x80, high = 0xbf;
        const char* second_reason = "invalid continuation byte";
        if (lead >= 0xc2 && lead <= 0xdf) length = 2;
        else if (lead == 0xe0) { length = 3; low = 0xa0; second_reason = "overlong encoding"; }
        else if ((lead >= 0xe1 && lead <= 0xec) || lead == 0xee || lead == 0xef) length = 3;
        else if (lead == 0xed) { length = 3; high = 0x9f; second_reason = "UTF-16 surrogate code point"; }
        else if (lead == 0xf0) { length = 4; low = 0x90; second_reason = "overlong encoding"; }
        else if (lead >= 0xf1 && lead <= 0xf3) length = 4;
        else if (lead == 0xf4) { length = 4; high = 0x8f; second_reason = "code point above U+10FFFF"; }
        else if (lead <= 0xbf) return malformed(i, text.substr(i, 1), "continuation byte without a lead byte");
        else if (lead <= 0xc1) return malformed(i, text.substr(i, 1), "overlong encoding");
        else return malformed(i, text.substr(i, 1), "byte that never occurs in UTF-8 (0xF5-0xFF)");
        for (std::size_t k = 1; k < length; ++k) {
            if (i + k >= text.size()) return malformed(i, text.substr(i), "truncated sequence");
            const auto byte = static_cast<unsigned char>(text[i + k]);
            const bool first = k == 1;
            const bool in_range = first ? (byte >= low && byte <= high) : (byte >= 0x80 && byte <= 0xbf);
            if (!in_range) {
                const bool continuation = byte >= 0x80 && byte <= 0xbf;
                return malformed(i, text.substr(i, k + 1),
                                 first && continuation ? second_reason : "invalid continuation byte");
            }
        }
        out.append(text.substr(i, length)); // valid text is copied unchanged
        i += length;
    }
    return out + "\"";
}

Result<std::vector<std::string>> render_time_probe_report(const TimeProbeReport& r, std::chrono::milliseconds deadline,
                                                          std::uint32_t requested) {
    const auto native = r.context.native_interface;
    // The diagnostic frequency read exists exactly for an ArvGvDevice: a failed
    // read is an error before this point (read_timing_context), never an
    // absent value; any other device type has no supported accessor.
    if (native == NativeInterface::gige_vision && !r.context.tick_frequency_hz)
        return fail(ErrorCode::invalid_state, "time-probe report: a GigE Vision timing context lacks the diagnostic "
                                              "tick-frequency read result; a failed read is an error, never reported "
                                              "as an absent frequency");
    if (native != NativeInterface::gige_vision && r.context.tick_frequency_hz)
        return fail(ErrorCode::invalid_state, "time-probe report: a tick frequency is reported only for an ArvGvDevice, "
                                              "not for " + std::string(interface_name(native)));
    const auto iface = interface_name(native);
    const auto path = actual_stream_timestamp_path(native);
    std::vector<std::string> lines;
    const auto add = [&lines](const Line& line) -> Status {
        auto text = line.done();
        if (!text) return text.error();
        lines.push_back(std::move(text).value());
        return {};
    };
    Line header("header");
    header.str("schema", "gige-mel.aravis-time-probe").u64("schema_version", time_probe_schema_version);
    header.str("device_id", r.device_id).str("interface", iface).str("native_device_type", r.context.native_device_type);
    header.str("compiled_sdk_version", r.compiled_sdk_version).str("runtime_sdk_version", r.runtime_sdk_version);
    header.strings("identity", r.context.identity);
    if (r.context.tick_frequency_hz) header.u64("diagnostic_tick_frequency_hz", *r.context.tick_frequency_hz);
    else header.null("diagnostic_tick_frequency_hz");
    header.str("diagnostic_tick_frequency_read", diagnostic_tick_frequency_read(native));
    header.str("actual_stream_timestamp_path", path);
    header.str("actual_stream_timestamp_path_basis", actual_stream_timestamp_path_basis(native));
    if (native == NativeInterface::gige_vision) header.strings("sdk_stream_timestamp_paths", gige_stream_timestamp_paths());
    else header.null("sdk_stream_timestamp_paths");
    header.str("sdk_buffer_timestamp_source", buffer_timestamp_source(native));
    header.str("sdk_system_timestamp_source", system_timestamp_source(native));
    header.str("timing_origin", timing_origin(native));
    header.raw("configuration", configuration_object(r.configuration));
    header.u64("requested_frames", requested).u64("receive_deadline_ms", static_cast<std::uint64_t>(deadline.count()));
    header.str("timestamp_units", "SDK-reported unsigned 64-bit nanoseconds, written unconverted");
    header.boolean("timestamp_qualified", false).str("qualification_reason", timestamp_qualification_reason());
    if (auto added = add(header); !added) return added.error();
    std::uint64_t index = 0;
    for (const auto& f : r.frames) {
        Line line("frame");
        line.u64("index", index++);
        line.str("device_id", r.device_id).str("interface", iface);
        line.str("compiled_sdk_version", r.compiled_sdk_version).str("runtime_sdk_version", r.runtime_sdk_version);
        line.u64("stream_generation", f.native.stream_generation).u64("native_frame_id", f.native.native_frame_id);
        line.u64("width", f.native.width).u64("height", f.native.height);
        line.u64("offset_x", f.native.offset_x).u64("offset_y", f.native.offset_y);
        line.u64("sdk_buffer_timestamp_ns", f.native.sdk_buffer_timestamp_ns);
        line.u64("sdk_system_timestamp_ns", f.native.sdk_system_timestamp_ns);
        line.str("actual_stream_timestamp_path", path);
        line.boolean("descriptor_timestamp_valid", f.descriptor_timestamp_valid);
        line.boolean("timestamp_qualified", false).str("qualification_reason", timestamp_qualification_reason());
        if (auto added = add(line); !added) return added.error();
    }
    Line summary("summary");
    summary.u64("frames", r.frames.size());
    summary.u64("delivered_frames", r.statistics.delivered_frames).u64("incomplete_frames", r.statistics.incomplete_frames);
    summary.u64("timeout_count", r.statistics.timeout_count).u64("pool_starvation_events", r.statistics.pool_starvation_events);
    summary.boolean("timestamp_qualified", false);
    if (auto added = add(summary); !added) return added.error();
    return lines;
}

std::string_view timing_origin(NativeInterface native_interface) {
    switch (native_interface) {
    case NativeInterface::fake:
        return "pinned Aravis 0.8.36 private Fake device: both SDK timestamps are the host wall clock "
               "(g_get_real_time) sampled when the SDK fills the buffer; not camera time, not exposure start, "
               "not host-monotonic";
    case NativeInterface::gige_vision:
        return "unverified: GigE Vision device timestamp origin, event meaning, epoch, synchronization and "
               "accuracy are not established without a camera-specific timing profile";
    case NativeInterface::other: break;
    }
    return "unverified: unknown native device type";
}

std::string_view timestamp_qualification_reason() {
    return "observation only: no camera timing profile establishes the timestamp event (integration start), "
           "epoch, clock domain or continuity; the adapter's FrameDescriptor timestamp remains invalid";
}

TimeProbeOutcome time_probe(ICameraBackend& backend, std::string_view device_id, std::uint32_t frames,
                            const std::string& output_path, std::chrono::milliseconds deadline) {
    return probe(backend, device_id, frames, output_path, deadline, nullptr);
}

TimeProbeOutcome time_probe_with_faults_for_testing(ICameraBackend& backend, std::string_view device_id,
                                                    std::uint32_t frames, const std::string& output_path,
                                                    std::chrono::milliseconds deadline,
                                                    const TimeProbeTestFaults& faults) {
    return probe(backend, device_id, frames, output_path, deadline, &faults);
}

} // namespace gige_mel::backend::aravis
