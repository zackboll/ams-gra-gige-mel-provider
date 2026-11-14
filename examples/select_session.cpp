#include "selection/builtin_backends.hpp"
#include "selection/session_selection.hpp"

#include <iostream>
#include <stdexcept>

namespace {
template <class T> T take(gige_mel::backend::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: gige_mel_select --list | BACKEND EXACT_DEVICE_ID PROFILE [--inspect]\n"
                         "Selection performs no device I/O. --inspect explicitly opens and reads,\n"
                         "then closes; it never applies configuration or starts acquisition.\n";
            return 0;
        }
        auto catalog = take(gige_mel::selection::make_builtin_catalog());
        std::cout << "Registered backends (no discovery):";
        for (const auto& id : catalog.backend_ids()) std::cout << ' ' << id;
        std::cout << '\n';
        if (argc == 2 && std::string_view(argv[1]) == "--list") return 0;
        if ((argc != 4 && argc != 5) || (argc == 5 && std::string_view(argv[4]) != "--inspect"))
            throw std::runtime_error("expected BACKEND EXACT_DEVICE_ID PROFILE [--inspect]; see --help");
        auto selected = take(gige_mel::selection::select_session(catalog, {argv[1], argv[2], argv[3], {}}));
        // The same composition is used by a MEL application. Control::init is
        // still the authoritative MEL open, not selection or manager creation.
        auto manager = gige_mel::make_api_manager(selected.settings, selected.backend);
        if (!manager) throw std::runtime_error("manager construction failed");
        std::cout << "Selected backend=" << selected.backend_id << " device=" << selected.device_id
                  << " profile=" << selected.profile_id << '\n';
        if (argc == 5) {
            auto device = take(selected.backend->open(selected.device_id));
            // Close even on failed readback, preserving both errors.
            auto configuration = device->configuration();
            auto closed = device->close();
            if (configuration) {
                const auto& value = configuration.value();
                std::cout << "Readback: " << value.width << 'x' << value.height << " offsets="
                          << value.offset_x << ',' << value.offset_y << " rate=" << value.frame_rate_hz
                          << " Hz exposure=" << value.exposure.count() << " ns\n";
            } else {
                std::cerr << "Readback failed: " << configuration.error().message << '\n';
            }
            if (!closed) std::cerr << "Close failed: " << closed.error().message << '\n';
            if (!configuration || !closed) return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
