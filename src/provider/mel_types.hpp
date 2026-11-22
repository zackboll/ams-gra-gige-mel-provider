#pragma once

#include <gige_mel/provider.hpp>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/** @namespace gige_mel::provider_detail
 * @brief Private common provider implementation across translation units.
 * Hidden symbols and aliases do not define an installed or extension interface.
 */
/// Local shorthand for the pinned IR MEL declarations.
namespace ir = ams::iface::irmel;
/// Local shorthand for the pinned common MEL declarations.
namespace mel = ams::iface::mel;
using namespace std::chrono_literals;

} // namespace provider_detail
} // namespace gige_mel
