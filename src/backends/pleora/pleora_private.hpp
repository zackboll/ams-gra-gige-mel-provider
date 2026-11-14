#pragma once

// Internal policy and diagnostic helpers; never installed.
#include <gige_mel/backend.hpp>
class PvDeviceGEV;
namespace gige_mel::backend::pleora {
std::string compiled_version();
std::string runtime_version();
Status configure_device_policy(PvDeviceGEV& device);
}