// Internal diagnostic for the optional Pleora backend. No SDK types escape
// the adapter. Capture exercises source acquisition; it does not qualify time.
#include "pleora_backend.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {
namespace backend = gige_mel::backend;
namespace pleora = backend::pleora;

void usage() {
    std::cout <<
        "Pleora eBUS source diagnostic (internal, not installed)\n"
        "Launch with: python3 scripts/run_pleora.py SDK_ROOT PROGRAM [ARGUMENTS]\n"
        "  gige_mel_pleora_diagnostic --help | --version\n"
        "  gige_mel_pleora_diagnostic list\n"
        "  gige_mel_pleora_diagnostic inspect DEVICE_ID\n"
        "  gige_mel_pleora_diagnostic capture DEVICE_ID [FRAMES [TIMEOUT_MS]]\n"
        "  gige_mel_pleora_diagnostic apply DEVICE_ID WIDTH HEIGHT OFFSET_X OFFSET_Y RATE_HZ EXPOSURE_NS\n"
        "Use an exact ID returned by list. No implicit first-camera selection.\n"
        "inspect reads features. capture starts/stops acquisition and configures\n"
        "packet size and the transport destination; it changes no ROI/exposure/rate/pixel format.\n"
        "apply explicitly writes the complete requested configuration while idle.\n"
        "Capture defaults: 4 frames, one overall 5000 ms receive deadline.\n"
        "Production timestamps are unqualified; this is not MEL Image delivery.\n";
}

template <class T> bool number(const char* argument, T& value) {
    const std::string_view text(argument);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

void configuration(const backend::CameraConfiguration& c) {
    std::cout << "configuration=" << c.width << 'x' << c.height
              << " offset=" << c.offset_x << ',' << c.offset_y
              << " rate_hz=" << c.frame_rate_hz << " exposure_ns=" << c.exposure.count() << '\n';
}

int error(const backend::Error& value) {
    std::cerr << "error code=" << static_cast<int>(value.code) << ": " << value.message << '\n';
    return 1;
}

class Session {
public:
    std::shared_ptr<backend::ICameraDevice> device;
    std::unique_ptr<backend::IFrameStream> stream;
    ~Session() { cleanup(); }
    int finish() {
        if (stream) stream->request_stop();
        // Device close owns native stop/drain/disconnect. Destroying the
        // stream afterwards observes a closed device, rather than retrying an
        // uncertain command once from stop(), again from its destructor and
        // a third time from close(). Preserve the single cleanup result.
        const auto closed = device ? device->close() : backend::Status{};
        stream.reset();
        device.reset();
        return closed ? 0 : error(closed.error());
    }
private:
    void cleanup() noexcept {
        try { (void)finish(); }
        catch (...) { std::cerr << "error: exception during Pleora session cleanup\n"; }
    }
};

int run(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") { usage(); return 0; }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "compiled_ebus=" << pleora::compiled_version()
                  << " runtime_ebus=" << pleora::runtime_version() << '\n';
        return pleora::compiled_version() == pleora::runtime_version() ? 0 : 1;
    }
    if (argc < 2) { usage(); return 2; }
    const std::string_view command(argv[1]);
    std::uint32_t frames = 4;
    std::uint32_t timeout_ms = 5000;
    backend::CameraConfiguration requested;
    std::int64_t exposure_ns{};
    // All command-line validation precedes construction/discovery/device I/O.
    if (command == "list") {
        if (argc != 2) { usage(); return 2; }
    } else if (command == "inspect") {
        if (argc != 3 || !*argv[2]) { usage(); return 2; }
    } else if (command == "capture") {
        if (argc < 3 || argc > 5 || !*argv[2] ||
            (argc > 3 && !number(argv[3], frames)) ||
            (argc > 4 && !number(argv[4], timeout_ms)) ||
            frames == 0 || frames > 1024 || timeout_ms == 0 || timeout_ms > 60000) {
            std::cerr << "capture requires 1..1024 frames and 1..60000 ms\n";
            return 2;
        }
    } else if (command == "apply") {
        if (argc != 9 || !*argv[2] || !number(argv[3], requested.width) ||
            !number(argv[4], requested.height) || !number(argv[5], requested.offset_x) ||
            !number(argv[6], requested.offset_y) || !number(argv[7], requested.frame_rate_hz) ||
            !number(argv[8], exposure_ns) || requested.width == 0 || requested.height == 0 ||
            requested.frame_rate_hz == 0 || exposure_ns <= 0) {
            std::cerr << "apply requires complete positive dimensions/rate/exposure and nonnegative offsets\n";
            return 2;
        }
        requested.exposure = std::chrono::nanoseconds(exposure_ns);
    } else { usage(); return 2; }

    const auto camera_backend = pleora::make_backend();
    if (command == "list") {
        const auto devices = camera_backend->enumerate();
        if (!devices) return error(devices.error());
        for (const auto& device : devices.value())
            std::cout << device.device_id << '\t' << device.display_name << '\n';
        std::cout << "devices=" << devices.value().size() << '\n';
        return 0;
    }
    if (command == "inspect") {
        const auto result = pleora::inspect(*camera_backend, argv[2]);
        if (!result) return error(result.error());
        std::cout << "device_id=" << result.value().device_id << '\n';
        for (const auto& [name, value] : result.value().native)
            std::cout << name << '=' << value << '\n';
        if (result.value().configuration) configuration(*result.value().configuration);
        for (const auto& problem : result.value().problems) std::cout << "profile_problem=" << problem << '\n';
        return result.value().capabilities && result.value().configuration && result.value().problems.empty() ? 0 : 1;
    }

    Session session;
    auto opened = camera_backend->open(argv[2]);
    if (!opened) return error(opened.error());
    session.device = std::move(opened).value();
    if (command == "apply") {
        const auto actual = session.device->apply(requested);
        if (!actual) return error(actual.error());
        configuration(actual.value());
        return session.finish();
    }

    auto created = session.device->create_stream();
    if (!created) return error(created.error());
    session.stream = std::move(created).value();
    const auto started = session.stream->start(1);
    if (!started) return error(started.error());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::uint32_t captured = 0;
    while (captured < frames) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) { std::cerr << "error: capture receive deadline expired\n"; return 1; }
        auto received = session.stream->receive(remaining);
        if (!received) {
            if (received.error().code == backend::ErrorCode::timeout) continue;
            return error(received.error());
        }
        const auto& frame = received.value();
        const auto& descriptor = frame.descriptor();
        // An inexpensive reproducible byte checksum for a host qualification log.
        std::uint64_t checksum = 14695981039346656037ULL;
        for (const auto value : frame.bytes()) { checksum ^= std::to_integer<std::uint8_t>(value); checksum *= 1099511628211ULL; }
        std::cout << "frame=" << ++captured << " native_id=" << descriptor.native_frame_id
                  << " size=" << descriptor.width << 'x' << descriptor.height
                  << " stride=" << descriptor.row_stride_bytes << " bytes=" << frame.bytes().size()
                  << " checksum_fnv1a64=" << checksum << " timestamp_valid=" << descriptor.timestamp.valid << '\n';
        if (captured == 1) configuration(descriptor.configuration);
    }
    const auto statistics = session.stream->statistics();
    std::cout << "delivered=" << statistics.delivered_frames << " incomplete=" << statistics.incomplete_frames
              << " timeouts=" << statistics.timeout_count << " starvation=" << statistics.pool_starvation_events << '\n';
    return session.finish();
}
} // namespace

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& exception) { std::cerr << "error: " << exception.what() << '\n'; return 1; }
    catch (...) { std::cerr << "error: unexpected diagnostic failure\n"; return 1; }
}
