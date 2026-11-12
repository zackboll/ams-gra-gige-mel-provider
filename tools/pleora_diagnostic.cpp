#include "backends/pleora/pleora_backend.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

namespace backend = gige_mel::backend;
int main(int argc, char** argv) {
    auto backend = backend::pleora::make_backend();
    if (argc == 2 && std::string(argv[1]) == "list") {
        auto devices = backend->enumerate();
        if (!devices) { std::cerr << devices.error().message << '\n'; return 1; }
        for (const auto& device : devices.value())
            std::cout << device.device_id << " " << device.display_name << '\n';
        std::cout << devices.value().size() << " GigE Vision devices\n";
        return 0;
    }
    if (argc != 3 || (std::string(argv[1]) != "inspect" && std::string(argv[1]) != "capture")) {
        std::cerr << "usage: pleora_diagnostic list | inspect MAC | capture MAC\n"
                     "capture writes volatile ROI/timing (full frame, 10 Hz, <=35 ms), then restores readback.\n";
        return 2;
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
    if (std::string(argv[1]) == "capture") {
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
                else for (int i = 0; i < 20; ++i) {
                    auto frame = stream->receive(std::chrono::milliseconds(1000));
                    if (!frame) { std::cerr << frame.error().message << '\n'; result = 1; break; }
                    std::cout << "frame " << frame.value().descriptor().native_frame_id
                              << " bytes " << frame.value().bytes().size() << " timestamp invalid\n";
                }
                stream->stop();
                const auto stats = stream->statistics();
                std::cout << "delivered " << stats.delivered_frames << " incomplete " << stats.incomplete_frames
                          << " timeouts " << stats.timeout_count << '\n';
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
