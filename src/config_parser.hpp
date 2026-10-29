#pragma once

#include <gige_mel/backend.hpp>

#include <string_view>

namespace gige_mel {
// Private representation/schema validation, independent of camera limits.
backend::Result<backend::CameraConfiguration> parse_camera_config(std::string_view input);
}