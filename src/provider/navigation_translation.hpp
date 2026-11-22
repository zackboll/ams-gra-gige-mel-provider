#pragma once

#include "mel_types.hpp"
#include <mel/library/NavigationReport.h>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/** @brief Return the first structural error, or no error for an admissible shape.
 * Positive system time and nonnegative attitude-rate time (nanoseconds), a
 * supported solution state, finite geodetic coordinates in radian bounds,
 * finite remaining fields and nonnegative diagonal variances are required.
 * Does not enforce attachment ordering, validate physical data or establish an epoch.
 */
std::optional<std::string> navigation_error(const mel::NavigationReport& report);
/// Copy a structurally validated report to backend values without MEL ownership,
/// units/epoch conversion, normalization or physical alignment claims.
/// @pre navigation_error returned no error for this report.
backend::NavigationReport backend_navigation(const mel::NavigationReport& report) noexcept;

} // namespace provider_detail
} // namespace gige_mel
