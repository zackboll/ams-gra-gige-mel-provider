#pragma once

// Internal, uninstalled source interface. No SDK types or provider selection.
#include <gige_mel/backend.hpp>
#include "profiles/camera_profile.hpp"
#include <functional>

namespace gige_mel::backend::pleora {

inline constexpr std::string_view backend_id = "pleora";

struct Options {
    // Receiver evaluation only; does not establish SDK development entitlement.
    bool allow_watermarked_evaluation{false};
    std::function<void(std::string_view)> diagnostic;
};

// Construction performs no discovery, open or configuration I/O. The backend
// owns the selected profile; the options-only overload selects the built-in
// monochrome profile. enumerate() broadcasts discovery; open() accepts exactly
// a discovered MAC address, not an IP or display-name alias.
std::shared_ptr<ICameraBackend> make_backend(Options options = {});
std::shared_ptr<ICameraBackend> make_backend(std::shared_ptr<const profiles::ICameraProfile>, Options options = {});

} // namespace gige_mel::backend::pleora
