#pragma once

// Internal, uninstalled source interface. No SDK types or provider selection.
#include <gige_mel/backend.hpp>

namespace gige_mel::backend::pleora {

// Construction performs no SDK I/O. enumerate() broadcasts discovery; open()
// accepts exactly a discovered MAC address, not an IP or display-name alias.
std::shared_ptr<ICameraBackend> make_backend();

} // namespace gige_mel::backend::pleora
