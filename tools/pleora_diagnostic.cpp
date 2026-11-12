#include "backends/pleora/pleora_backend.hpp"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <iostream>
#include <string>

namespace backend = gige_mel::backend;
int main(int argc, char** argv) {
    backend::pleora::Options options;
    options.diagnostic = [](std::string_view message) { std::cerr << message << '\n'; };
    if (argc > 1 && std::string(argv[1]) == "--allow-watermarked-evaluation") {
        options.allow_watermarked_evaluation = true;
        --argc;
        ++argv;
    }
    auto backend = backend::pleora::make_backend(std::move(options));
    if (argc == 2 && std::string(argv[1]) == "list") {
        auto devices = backend->enumerate();
        if (!devices) { std::cerr << devices.error().message << '\n'; return 1; }
        for (const auto& device : devices.value())
            std::cout << device.device_id << " " << device.display_name << '\n';
        std::cout << devices.value().size() << " GigE Vision devices\n";
        return 0;
    }
    const bool capture = argc >= 3 && std::string(argv[1]) == "capture";
    if ((!capture && (argc != 3 || std::string(argv[1]) != "inspect")) || argc > 5) {
        std::cerr << "usage: pleora_diagnostic [--allow-watermarked-evaluation] list | inspect MAC | capture MAC [FRAMES [OUTPUT.pgm]]\n"
                     "Evaluation is default-off, unqualified, and never overrides SDK licensing.\n"
                     "capture writes volatile ROI/timing (full frame, 10 Hz, <=35 ms), then restores readback.\n";
        return 2;
    }
    unsigned frames = 20;
    if (argc >= 4) {
        const std::string count = argv[3];
        const auto parsed = std::from_chars(count.data(), count.data() + count.size(), frames);
        if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() || frames == 0 || frames > 100000) {
            std::cerr << "FRAMES must be 1..100000\n";
            return 2;
        }
    }
    auto opened = backend->open(argv[2]);
    if (!opened) { std::cerr << opened.error().message << '\n'; return 1; }
    auto device = opened.value();
    auto original = device->configuration();
    if (!original) { std::cerr << original.error().message << '\n'; return 1; }
    const auto c = original.value();
    std::cout << c.width << 'x' << c.height << " offset " << c.offset_x << ',' << c.offset_y
              << " rate " << c.frame_rate_hz << " Hz exposure " << c.exposure.count() << " ns\n";
    int result = 0;
    if (capture) {
        const auto caps = device->capabilities();
        auto requested = c;
        requested.width = caps.sensor_width; requested.height = caps.sensor_height;
        requested.offset_x = 0; requested.offset_y = 0;
        requested.frame_rate_hz = 10;
        requested.exposure = std::min(c.exposure, std::chrono::nanoseconds(std::chrono::milliseconds(35)));
        auto applied = device->apply(requested);
        if (!applied || applied.value() != requested) {
            std::cerr << (applied ? "configuration did not read back exactly" : applied.error().message) << '\n';
            result = 1;
        } else {
            auto created = device->create_stream();
            if (!created) { std::cerr << created.error().message << '\n'; result = 1; }
            else {
                auto stream = std::move(created).value();
                auto started = stream->start(1);
                if (!started) { std::cerr << started.error().message << '\n'; result = 1; }
                else for (unsigned i = 0; i < frames; ++i) {
                    auto frame = stream->receive(std::chrono::milliseconds(1000));
                    if (!frame) { std::cerr << frame.error().message << '\n'; result = 1; break; }
                    const auto& d = frame.value().descriptor();
                    const auto bytes = frame.value().bytes();
                    std::cout << "frame " << d.native_frame_id << ' ' << d.width << 'x' << d.height
                              << " stride " << d.row_stride_bytes
                              << " format " << (d.pixel_format == backend::PixelFormat::mono8 ? "Mono8" : "Mono12")
                              << " bytes " << bytes.size() << " complete=" << (d.status == backend::FrameStatus::complete)
                              << " timestamp invalid\n";
                    if (i == 0 && argc == 5) {
                        // Save every row, unmodified, including any SDK watermark.
                        FILE* output = d.pixel_format == backend::PixelFormat::mono8 ? std::fopen(argv[4], "wbx") : nullptr;
                        bool saved = output != nullptr;
                        if (output) {
                            saved = std::fprintf(output, "P5\n%u %u\n255\n", d.width, d.height) > 0;
                            for (std::uint32_t y = 0; y < d.height; ++y)
                                saved = (std::fwrite(bytes.data() + y * d.row_stride_bytes, 1, d.width, output) == d.width) && saved;
                            saved = (std::fclose(output) == 0) && saved;
                        }
                        if (!saved) { std::cerr << "sample save failed (requires Mono8 and a new output path)\n"; result = 1; break; }
                    }
                }
                stream->stop();
                const auto stats = stream->statistics();
                std::cout << "delivered " << stats.delivered_frames << " incomplete " << stats.incomplete_frames
                          << " timeouts " << stats.timeout_count << " pool_starvation " << stats.pool_starvation_events << '\n';
            }
        }
        auto restored = device->apply(c);
        if (!restored || restored.value() != c) {
            std::cerr << "RESTORATION FAILED: " << (restored ? "readback mismatch" : restored.error().message) << '\n';
            result = 1;
        }
    }
    auto closed = device->close();
    if (!closed) { std::cerr << closed.error().message << '\n'; result = 1; }
    return result;
}
