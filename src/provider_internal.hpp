#pragma once

#include <irmel/library/irmel-types/Control.h>

#include <memory>
#include <string_view>

namespace gige_mel {

/** @brief Reuse the live Control or create an uninitialized wrapper for this session.
 * @param instance Exact instance name (borrowed for the call).
 * @param manager Shared manager handle from make_api_manager.
 * @return Shared wrapper, or empty on foreign manager, instance mismatch or
 * callback/sink reentry. Under operations, publish a weak link on creation.
 * No discovery or device open; allocation may throw.
 */
std::shared_ptr<ams::iface::irmel::Control> control_for(
    std::string_view instance, const std::shared_ptr<API_Manager>& manager);
/// Create an uninitialized wrapper carrying only the manager affinity token.
/// No Control or open device is needed. A foreign manager or mismatched instance
/// returns empty; allocation can throw. Does not allocate consumer pixel storage.
std::shared_ptr<ams::iface::irmel::Buffer> buffer_for(
    std::string_view instance, const std::shared_ptr<API_Manager>& manager);

} // namespace gige_mel
