#pragma once

// Internal, uninstalled source interface. No SDK types or provider selection.
#include <gige_mel/backend.hpp>
#include <functional>

namespace gige_mel::backend::pleora {

// Construction performs no SDK I/O. enumerate() broadcasts discovery; open()
// accepts exactly a discovered MAC address, not an IP or display-name alias.
struct Options {
    // Receiver evaluation only; does not establish SDK development entitlement.
    bool allow_watermarked_evaluation{false};
    std::function<void(std::string_view)> diagnostic;
};
std::shared_ptr<ICameraBackend> make_backend(Options options = {});

} // namespace gige_mel::backend::pleora
