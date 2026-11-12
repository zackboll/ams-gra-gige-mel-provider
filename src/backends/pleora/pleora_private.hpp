#pragma once

// Private native policy seam used by production open and offline SDK tests.
// This configures the SDK object only; it performs no camera/network I/O.

#include <gige_mel/backend.hpp>

class PvDeviceGEV;

namespace gige_mel::backend::pleora {

Status configure_device_policy(PvDeviceGEV& device);

} // namespace gige_mel::backend::pleora
