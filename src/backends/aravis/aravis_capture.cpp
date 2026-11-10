// Bounded direct capture (GIGE-MEL-009). Uses only the project's backend
// contract, so the diagnostic (GigE backend) and the tests (private Fake
// backend) exercise the same routine. No SDK include is needed here.
#include "aravis_backend.hpp"

#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gige_mel::backend::aravis {
namespace {

Error fail(ErrorCode code, std::string message) { return Error{code, std::move(message)}; }

// Writes "P5\n<w> <h>\n255\n" followed by exactly width*height bytes, copied
// row by row from the leased span (stride padding is not written). The file
// is created only after the frame was validated; a partial file is removed.
Result<std::size_t> write_pgm(const std::string& path, const FrameLease& frame) {
    const auto& d = frame.descriptor();
    // This diagnostic's P5/255 and evidence schema remain Mono8-only. Never
    // silently truncate Mono12 or emit little-endian data as a 16-bit PGM
    // (PGM requires big-endian words). Native Mono12 is available via leases.
    if (d.pixel_format != PixelFormat::mono8 || d.storage_bits_per_pixel != 8 ||
        d.significant_bits_per_pixel != 8 || d.byte_order != ByteOrder::not_applicable)
        return fail(ErrorCode::unsupported, "PGM capture supports Mono8 only; Mono12 requires native frame leases");
    const auto layout = image_layout(d.pixel_format, d.width, d.height);
    if (!layout || d.row_stride_bytes < layout->row_bytes ||
        (d.height > 1 && d.row_stride_bytes > (std::numeric_limits<std::size_t>::max() - layout->row_bytes) / (d.height - 1)) ||
        frame.bytes().size() < (d.height - 1) * d.row_stride_bytes + layout->row_bytes)
        return fail(ErrorCode::io_error, "leased frame span does not cover its layout");
    const std::string header = "P5\n" + std::to_string(d.width) + " " + std::to_string(d.height) + "\n255\n";
    if (layout->image_bytes > std::numeric_limits<std::size_t>::max() - header.size())
        return fail(ErrorCode::io_error, "PGM size overflows");
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path.c_str(), "wb"), &std::fclose);
    if (!file) return fail(ErrorCode::io_error, "cannot create output '" + path + "'");
    bool ok = std::fwrite(header.data(), 1, header.size(), file.get()) == header.size();
    for (std::uint32_t row = 0; ok && row < d.height; ++row)
        ok = std::fwrite(frame.bytes().data() + std::size_t{row} * d.row_stride_bytes, 1, d.width, file.get()) == d.width;
    ok = std::fflush(file.get()) == 0 && ok;
    ok = std::fclose(file.release()) == 0 && ok;
    if (!ok) {
        std::remove(path.c_str());
        return fail(ErrorCode::io_error, "writing output '" + path + "' failed; the partial file was removed");
    }
    return header.size() + layout->image_bytes;
}

} // namespace

CaptureOutcome capture_one(ICameraBackend& backend, std::string_view device_id, const std::string& output_path,
                           std::chrono::milliseconds deadline) {
    CaptureOutcome outcome;
    // Argument validation before any device I/O.
    if (device_id.empty()) { outcome.error = fail(ErrorCode::invalid_argument, "DEVICE_ID must not be empty"); return outcome; }
    if (output_path.empty()) { outcome.error = fail(ErrorCode::invalid_argument, "OUTPUT path must not be empty"); return outcome; }
    if (deadline <= std::chrono::milliseconds::zero()) {
        outcome.error = fail(ErrorCode::invalid_argument, "capture deadline must be positive");
        return outcome;
    }
    auto opened = backend.open(device_id);
    if (!opened) { outcome.error = opened.error(); return outcome; }
    auto device = std::move(opened).value();
    std::unique_ptr<IFrameStream> stream;
    // Cleanup on every path: stop (noexcept) and destroy the stream wrapper,
    // then close the device. A close failure (e.g. a failed AcquisitionStop)
    // is reported separately and never hidden.
    const auto cleanup = [&] {
        if (stream) { stream->stop(); stream.reset(); }
        if (auto closed = device->close(); !closed) outcome.cleanup_error = closed.error();
    };
    auto configuration = device->configuration();
    if (!configuration) { outcome.error = configuration.error(); cleanup(); return outcome; }
    auto created = device->create_stream();
    if (!created) { outcome.error = created.error(); cleanup(); return outcome; }
    stream = std::move(created).value();
    if (auto started = stream->start(1); !started) { outcome.error = started.error(); cleanup(); return outcome; }
    auto received = stream->receive(deadline); // one overall deadline
    if (!received) { outcome.error = received.error(); cleanup(); return outcome; }
    const FrameLease frame = std::move(received).value();
    const auto& d = frame.descriptor();
    auto written = write_pgm(output_path, frame);
    if (!written) { outcome.error = written.error(); cleanup(); return outcome; }
    CaptureReport report;
    report.device_id = std::string(device_id);
    report.configuration = d.configuration;
    report.width = d.width;
    report.height = d.height;
    report.source_row_stride = d.row_stride_bytes;
    report.native_frame_id = d.native_frame_id;
    report.stream_generation = d.stream_generation;
    report.timestamp_valid = d.timestamp.valid;
    report.pgm_bytes = written.value();
    report.statistics = stream->statistics();
    // GIGE-MEL-019: measured on the receiving thread after the first buffer.
    if (auto buffer = receive_buffer_observation(*stream)) report.receive_buffer = buffer.value();
    outcome.report = std::move(report);
    cleanup(); // the lease outlives stop/close by contract; released after
    return outcome;
}

} // namespace gige_mel::backend::aravis
