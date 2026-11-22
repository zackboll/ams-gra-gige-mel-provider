#pragma once

#include "mel_types.hpp"
#include <mel/library/NavigationReport.h>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

std::optional<std::string> navigation_error(const mel::NavigationReport& report);
backend::NavigationReport backend_navigation(const mel::NavigationReport& report) noexcept;

} // namespace provider_detail
} // namespace gige_mel
