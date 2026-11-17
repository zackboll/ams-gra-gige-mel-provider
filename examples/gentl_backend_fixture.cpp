// GIGE-MEL-GENTL-002 uninstalled example: explicit construction and read-only
// use of the internal GenTL backend against an EXPLICIT producer path. Built
// only with GIGE_MEL_BUILD_GENTL_BACKEND=ON and examples enabled. Its CTest
// entry runs it against the controlled fixture producer; with no camera
// description installed, the unchanged default profile rejects that fixture
// and the example reports the profile's own diagnostic (no fallback).
//
//   gentl_backend_example /absolute/path/producer.cti [composite-device-id]
//
// Never writes camera configuration and never acquires images.
#include "gentl_backend.hpp"

#include <iostream>

namespace g = gige_mel::backend::gentl_backend;

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: " << argv[0] << " /absolute/path/producer.cti [composite-device-id]\n";
        return 2;
    }
    // Explicit producer path, explicit read-only access, explicit profile.
    auto backend = g::make_backend(g::Options{argv[1], g::DeviceAccess::read_only},
                                   gige_mel::backend::profiles::make_mono_free_running_profile());
    std::cout << "backend " << backend->id() << " constructed (no producer loaded yet)\n";
    auto devices = backend->enumerate();
    if (!devices) {
        std::cerr << "enumerate: " << devices.error().message << "\n";
        return 1;
    }
    for (const auto& device : devices.value())
        std::cout << "device " << device.device_id << " : " << device.display_name << "\n";
    if (argc == 2) return 0;
    auto opened = backend->open(argv[2]);
    if (!opened) {
        std::cout << "open refused by profile/backend (no fallback): " << opened.error().message << "\n";
        return 3;
    }
    const auto caps = opened.value()->capabilities();
    std::cout << "sensor " << caps.sensor_width << "x" << caps.sensor_height << "\n";
    if (auto config = opened.value()->configuration())
        std::cout << "configuration " << config.value().width << "x" << config.value().height << " @ "
                  << config.value().frame_rate_hz << " Hz\n";
    if (auto info = g::inspect(*opened.value()))
        std::cout << "xml " << info->xml_url << " schema " << info->schema << "\n";
    if (auto closed = opened.value()->close(); !closed) {
        std::cerr << "close: " << closed.error().message << "\n";
        return 1;
    }
    return 0;
}
