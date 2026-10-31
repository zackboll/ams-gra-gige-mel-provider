// Optional, uninstalled Aravis diagnostic (GIGE-MEL-008/009). Uses Aravis's
// GigE Vision interface only (no Fake fallback). `inspect` never writes a
// camera feature or starts acquisition; `apply` and `capture` are separate
// explicit commands.
#include "aravis_backend.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace backend = gige_mel::backend;
namespace aravis = gige_mel::backend::aravis;

namespace {

constexpr const char* usage =
    "Usage:\n"
    "  gige_mel_aravis_diagnostic --help\n"
    "  gige_mel_aravis_diagnostic list\n"
    "      Discover GigE Vision devices (network broadcast) and print stable device IDs.\n"
    "  gige_mel_aravis_diagnostic inspect DEVICE_ID\n"
    "      Open exactly DEVICE_ID read-only: SDK version, identity, native settings, and\n"
    "      the profile decision. Writes no camera feature; starts no acquisition.\n"
    "  gige_mel_aravis_diagnostic apply DEVICE_ID WIDTH HEIGHT OFFSET_X OFFSET_Y RATE_HZ EXPOSURE_NS\n"
    "      Explicitly apply one complete configuration and print the actual readback.\n"
    "      Requires the documented manual/free-running Mono8 profile; changes nothing else.\n"
    "  gige_mel_aravis_diagnostic capture DEVICE_ID OUTPUT.pgm\n"
    "      Open exactly DEVICE_ID, use its current supported configuration (no feature write),\n"
    "      acquire ONE validated Mono8 frame within 5 s, write a tightly packed binary PGM,\n"
    "      then stop and close. The frame timestamp is unqualified (not a camera or MEL time).\n"
    "\n"
    "Internal tool for the experimental Aravis adapter; not a supported MEL image profile.\n"
    "Exit status: 0 success, 1 failure/unsupported/readback differs/cleanup failure, 2 usage.\n";

const char* code_name(backend::ErrorCode code) {
    using C = backend::ErrorCode;
    switch (code) {
    case C::invalid_argument: return "invalid_argument";
    case C::invalid_state: return "invalid_state";
    case C::not_found: return "not_found";
    case C::busy: return "busy";
    case C::unsupported: return "unsupported";
    case C::resource_exhausted: return "resource_exhausted";
    case C::timeout: return "timeout";
    case C::stopped: return "stopped";
    case C::disconnected: return "disconnected";
    case C::io_error: return "io_error";
    }
    return "unknown";
}

int report(const backend::Error& error) {
    std::cerr << "error (" << code_name(error.code) << "): " << error.message << '\n';
    return 1;
}

void print(const backend::CameraConfiguration& c) {
    std::cout << "  width=" << c.width << " height=" << c.height << " offset_x=" << c.offset_x
              << " offset_y=" << c.offset_y << " frame_rate_hz=" << c.frame_rate_hz
              << " exposure_ns=" << c.exposure.count() << '\n';
}

void versions() {
    std::cout << "Aravis runtime " << aravis::runtime_version() << ", compiled against "
              << aravis::compiled_version() << '\n';
}

template <class T> bool parse(std::string_view text, T& value) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size();
}

int inspect(backend::ICameraBackend& camera, std::string_view id) {
    versions();
    auto inspection = aravis::inspect(camera, id);
    if (!inspection) return report(inspection.error());
    const auto& i = inspection.value();
    std::cout << "Device " << i.device_id << " via " << i.interface_protocol << " (read-only inspection)\n";
    for (const auto& [name, value] : i.native) std::cout << "  " << name << ": " << value << '\n';
    if (i.capabilities) {
        const auto& c = *i.capabilities;
        std::cout << "Profile supported. Envelope: sensor " << c.sensor_width << 'x' << c.sensor_height
                  << ", increments W" << c.width_increment << " H" << c.height_increment << " X"
                  << c.offset_x_increment << " Y" << c.offset_y_increment << ", rate " << c.minimum_frame_rate_hz
                  << '-' << c.maximum_frame_rate_hz << " Hz, minimum exposure " << c.minimum_exposure.count() << " ns\n";
    }
    if (i.configuration) {
        std::cout << "Current configuration (truthful readback):\n";
        print(*i.configuration);
    }
    for (const auto& problem : i.problems) std::cout << "PROBLEM: " << problem << '\n';
    return i.problems.empty() ? 0 : 1;
}

int apply(backend::ICameraBackend& camera, const std::vector<std::string_view>& args) {
    backend::CameraConfiguration requested;
    std::int64_t exposure_ns = 0;
    if (!parse(args[2], requested.width) || !parse(args[3], requested.height) || !parse(args[4], requested.offset_x) ||
        !parse(args[5], requested.offset_y) || !parse(args[6], requested.frame_rate_hz) || !parse(args[7], exposure_ns)) {
        std::cerr << "apply: every value must be a decimal integer in range\n" << usage;
        return 2;
    }
    requested.exposure = std::chrono::nanoseconds{exposure_ns};
    versions();
    auto opened = camera.open(args[1]);
    if (!opened) return report(opened.error());
    auto device = std::move(opened).value();
    auto actual = device->apply(requested);
    int status = 0;
    if (!actual) status = report(actual.error());
    else {
        const bool same = actual.value() == requested;
        std::cout << "Applied; actual readback" << (same ? "" : " DIFFERS from the request") << ":\n";
        print(actual.value());
        status = same ? 0 : 1;
    }
    (void)device->close();
    return status;
}

int capture(backend::ICameraBackend& camera, std::string_view id, const std::string& output) {
    versions();
    const auto outcome = aravis::capture_one(camera, id, output, aravis::default_capture_deadline);
    int status = 0;
    if (outcome.report) {
        const auto& r = *outcome.report;
        std::cout << "Captured device " << r.device_id << ": " << r.width << 'x' << r.height << " Mono8, source row stride "
                  << r.source_row_stride << " bytes, native frame ID " << r.native_frame_id << ", stream generation "
                  << r.stream_generation << "\n  timestamp: unqualified (FrameTimestamp invalid; no camera, UTC/PTP or "
                  << "integration-start claim)\n  configuration:\n";
        print(r.configuration);
        std::cout << "  statistics: delivered " << r.statistics.delivered_frames << ", incomplete "
                  << r.statistics.incomplete_frames << ", timeouts " << r.statistics.timeout_count << ", pool starvation "
                  << r.statistics.pool_starvation_events << "\n  wrote " << output << " (" << r.pgm_bytes
                  << " bytes, tightly packed P5)\n";
    }
    if (outcome.error) status = report(*outcome.error);
    if (outcome.cleanup_error) {
        std::cerr << "cleanup ";
        status = report(*outcome.cleanup_error);
    }
    return outcome.ok() ? 0 : (status ? status : 1);
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        std::cout << usage; // no SDK call, no device I/O
        return args.empty() ? 2 : 0;
    }
    auto camera = aravis::make_backend(); // GigE Vision interface only; no I/O yet
    if (args[0] == "list" && args.size() == 1) {
        versions();
        auto devices = camera->enumerate();
        if (!devices) return report(devices.error());
        std::cout << devices.value().size() << " GigE Vision device(s)\n";
        for (const auto& device : devices.value()) std::cout << "  " << device.device_id << "  " << device.display_name << '\n';
        return 0;
    }
    if (args[0] == "inspect" && args.size() == 2) return inspect(*camera, args[1]);
    if (args[0] == "apply" && args.size() == 8) return apply(*camera, args);
    if (args[0] == "capture" && args.size() == 3) {
        if (args[1].empty() || args[2].empty()) {
            std::cerr << "capture: DEVICE_ID and OUTPUT.pgm must be non-empty\n" << usage;
            return 2;
        }
        return capture(*camera, args[1], std::string(args[2]));
    }
    std::cerr << usage;
    return 2;
}
