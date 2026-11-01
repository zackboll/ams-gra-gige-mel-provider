// Bounded, read-only native timestamp investigation (GIGE-MEL-010). Shared by
// the diagnostic `time-probe` command (production GigE backend) and the tests
// (private Fake backend): the SAME routine. It OBSERVES SDK timestamp facts
// and never qualifies them: every record states timestamp_qualified=false,
// and the production FrameDescriptor timestamp stays FrameTimestamp{}.
// No SDK include is needed here.
#include "aravis_backend.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gige_mel::backend::aravis {
namespace {

Error fail(ErrorCode code, std::string message) { return Error{code, std::move(message)}; }

// JSON string literal. Control characters, quote, backslash and DEL are
// escaped; bytes >= 0x80 are written as \u00XX (the BYTE value, not a decoded
// code point), so arbitrary camera strings always yield valid JSON text.
std::string quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (byte < 0x20 || byte >= 0x7f) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(byte));
            out += escaped;
        } else out += c;
    }
    return out + "\"";
}

std::string_view interface_name(NativeInterface native_interface) {
    switch (native_interface) {
    case NativeInterface::gige_vision: return "gige_vision";
    case NativeInterface::fake: return "fake";
    case NativeInterface::other: break;
    }
    return "other";
}

// One JSON object built from already-encoded members.
class Line {
public:
    Line& raw(std::string_view key, std::string_view encoded) {
        text_ += (text_.empty() ? "{" : ",") + quote(key) + ":" + std::string(encoded);
        return *this;
    }
    Line& str(std::string_view key, std::string_view value) { return raw(key, quote(value)); }
    Line& u64(std::string_view key, std::uint64_t value) { return raw(key, std::to_string(value)); }
    Line& boolean(std::string_view key, bool value) { return raw(key, value ? "true" : "false"); }
    std::string done() const { return text_ + "}\n"; }

private:
    std::string text_;
};

std::string object(const std::vector<std::pair<std::string, std::string>>& members) {
    std::string out = "{";
    for (std::size_t i = 0; i < members.size(); ++i)
        out += (i ? "," : "") + quote(members[i].first) + ":" + quote(members[i].second);
    return out + "}";
}

std::string configuration_object(const CameraConfiguration& c) {
    return "{\"width\":" + std::to_string(c.width) + ",\"height\":" + std::to_string(c.height) +
           ",\"offset_x\":" + std::to_string(c.offset_x) + ",\"offset_y\":" + std::to_string(c.offset_y) +
           ",\"frame_rate_hz\":" + std::to_string(c.frame_rate_hz) +
           ",\"exposure_ns\":" + std::to_string(c.exposure.count()) + "}";
}

// What the SDK buffer timestamp is for THIS device type, from pinned source
// only (never inferred from the values).
std::string_view buffer_timestamp_source(const TimingContext& context) {
    if (context.native_interface == NativeInterface::fake)
        return "pinned SDK Fake fill: g_get_real_time() * 1000 (src/arvfakecamera.c:786)";
    if (context.native_interface != NativeInterface::gige_vision) return "unknown native device type";
    if (context.tick_frequency_hz && *context.tick_frequency_hz != 0)
        return "SDK conversion of the GVSP leader timestamp with the stream's tick frequency "
               "(src/arvgvstream.c:480-482); event meaning, epoch and accuracy unverified";
    return "probe-time tick frequency is 0: the SDK stream substitutes its host reception wall-clock time "
           "(src/arvgvstream.c:484); not camera time";
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

std::vector<std::string> render(const TimeProbeReport& r, std::chrono::milliseconds deadline, std::uint32_t requested) {
    const auto iface = interface_name(r.context.native_interface);
    std::vector<std::string> lines;
    Line header;
    header.str("record", "header").str("schema", "gige-mel.aravis-time-probe").u64("schema_version", 1);
    header.str("device_id", r.device_id).str("interface", iface).str("native_device_type", r.context.native_device_type);
    header.str("compiled_sdk_version", r.compiled_sdk_version).str("runtime_sdk_version", r.runtime_sdk_version);
    header.raw("identity", object(r.context.identity));
    if (r.context.tick_frequency_hz) {
        header.u64("tick_frequency_hz", *r.context.tick_frequency_hz);
        header.str("tick_frequency_source",
                   "arv_gv_device_get_timestamp_tick_frequency() read once by this probe before acquisition "
                   "(GVBS 0x093C/0x0940); the SDK stream reads it separately at stream construction");
    } else {
        header.raw("tick_frequency_hz", "null");
        header.str("tick_frequency_source", r.context.native_interface == NativeInterface::fake
                                                ? "not reported: the Fake device is not an ArvGvDevice"
                                                : "not reported: no supported accessor for this device type");
    }
    header.str("sdk_buffer_timestamp_source", buffer_timestamp_source(r.context));
    header.str("sdk_system_timestamp_source", system_timestamp_source(r.context.native_interface));
    header.str("timing_origin", timing_origin(r.context.native_interface));
    header.raw("configuration", configuration_object(r.configuration));
    header.u64("requested_frames", requested).u64("receive_deadline_ms", static_cast<std::uint64_t>(deadline.count()));
    header.str("timestamp_units", "SDK-reported unsigned 64-bit nanoseconds, written unconverted");
    header.boolean("timestamp_qualified", false).str("qualification_reason", timestamp_qualification_reason());
    lines.push_back(header.done());
    std::uint64_t index = 0;
    for (const auto& f : r.frames) {
        Line line;
        line.str("record", "frame").u64("index", index++);
        line.str("device_id", r.device_id).str("interface", iface);
        line.str("compiled_sdk_version", r.compiled_sdk_version).str("runtime_sdk_version", r.runtime_sdk_version);
        line.u64("stream_generation", f.native.stream_generation).u64("native_frame_id", f.native.native_frame_id);
        line.u64("width", f.native.width).u64("height", f.native.height);
        line.u64("offset_x", f.native.offset_x).u64("offset_y", f.native.offset_y);
        line.u64("sdk_buffer_timestamp_ns", f.native.sdk_buffer_timestamp_ns);
        line.u64("sdk_system_timestamp_ns", f.native.sdk_system_timestamp_ns);
        line.boolean("descriptor_timestamp_valid", f.descriptor_timestamp_valid);
        line.boolean("timestamp_qualified", false).str("qualification_reason", timestamp_qualification_reason());
        lines.push_back(line.done());
    }
    Line summary;
    summary.str("record", "summary").u64("frames", r.frames.size());
    summary.u64("delivered_frames", r.statistics.delivered_frames).u64("incomplete_frames", r.statistics.incomplete_frames);
    summary.u64("timeout_count", r.statistics.timeout_count).u64("pool_starvation_events", r.statistics.pool_starvation_events);
    summary.boolean("timestamp_qualified", false);
    lines.push_back(summary.done());
    return lines;
}

using File = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

} // namespace

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
    TimeProbeOutcome outcome;
    // Strict argument validation, then exclusive output creation, all before
    // any device I/O.
    if (device_id.empty()) { outcome.error = fail(ErrorCode::invalid_argument, "DEVICE_ID must not be empty"); return outcome; }
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
    File file(std::fopen(output_path.c_str(), "wx"), &std::fclose); // exclusive: never overwrite
    if (!file) {
        const int saved = errno;
        outcome.error = fail(ErrorCode::io_error, "cannot create output '" + output_path + "' exclusively: " +
                                                      std::strerror(saved) + "; no device I/O was performed");
        return outcome;
    }
    // Only the file this call created is ever removed.
    const auto discard = [&] {
        if (file) (void)std::fclose(file.release());
        std::remove(output_path.c_str());
    };

    auto opened = backend.open(device_id);
    if (!opened) { outcome.error = opened.error(); discard(); return outcome; }
    auto device = std::move(opened).value();
    std::unique_ptr<IFrameStream> stream;
    // Cleanup on every path: stop (noexcept) and destroy the stream wrapper,
    // then close. A close failure (e.g. a failed AcquisitionStop) is
    // reported separately and never hidden.
    const auto cleanup = [&] {
        if (stream) { stream->stop(); stream.reset(); }
        if (auto closed = device->close(); !closed) outcome.cleanup_error = closed.error();
    };
    TimeProbeReport report;
    report.device_id = std::string(device_id);
    report.compiled_sdk_version = compiled_version();
    report.runtime_sdk_version = runtime_version();
    const auto run = [&]() -> Status {
        auto configuration = device->configuration();
        if (!configuration) return configuration.error();
        report.configuration = configuration.value();
        auto context = read_timing_context(*device);
        if (!context) return context.error();
        report.context = std::move(context).value();
        auto created = device->create_stream();
        if (!created) return created.error();
        stream = std::move(created).value();
        if (auto started = stream->start(1); !started) return started.error();
        const auto end = std::chrono::steady_clock::now() + deadline; // ONE overall deadline
        while (report.frames.size() < frames) {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining = now >= end ? std::chrono::milliseconds::zero()
                                              : std::chrono::ceil<std::chrono::milliseconds>(end - now);
            auto received = receive_with_native_timestamps(*stream, remaining);
            if (!received)
                return fail(received.error().code, received.error().message + " (after " +
                                                       std::to_string(report.frames.size()) + " of " +
                                                       std::to_string(frames) + " frames)");
            const auto& observed = received.value();
            report.frames.push_back({observed.native, observed.frame.descriptor().timestamp.valid});
            // `received` (the lease and its SDK buffer) is released here.
        }
        report.statistics = stream->statistics();
        return {};
    };
    try {
        if (auto ok = run(); !ok) outcome.error = ok.error();
    } catch (const std::exception& error) {
        outcome.error = fail(ErrorCode::io_error, std::string("time probe failed: ") + error.what());
    }
    cleanup();
    if (outcome.error || outcome.cleanup_error) { discard(); return outcome; }

    const auto lines = render(report, deadline, frames);
    bool written = true;
    for (const auto& line : lines) {
        if (line.size() > time_probe_max_line_bytes) {
            outcome.error = fail(ErrorCode::io_error, "a report line exceeds " + std::to_string(time_probe_max_line_bytes) +
                                                          " bytes; the output was removed");
            discard();
            return outcome;
        }
    }
    for (const auto& line : lines) {
        written = written && std::fwrite(line.data(), 1, line.size(), file.get()) == line.size();
        report.output_bytes += line.size();
    }
    written = std::fflush(file.get()) == 0 && written;
    written = std::fclose(file.release()) == 0 && written;
    if (!written) {
        outcome.error = fail(ErrorCode::io_error, "writing output '" + output_path + "' failed; the partial file was removed");
        std::remove(output_path.c_str());
        return outcome;
    }
    report.output_lines = lines.size();
    outcome.report = std::move(report);
    return outcome;
}

} // namespace gige_mel::backend::aravis
