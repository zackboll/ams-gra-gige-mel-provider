#pragma once

#include <irmel/library/irmel-types/Control.h>

#include <memory>
#include <string_view>

namespace gige_mel {

// Internal object lookup, shared by factory selection and provider runtime.
std::shared_ptr<ams::iface::irmel::Control> control_for(
    std::string_view instance, const std::shared_ptr<API_Manager>& manager);
std::shared_ptr<ams::iface::irmel::Buffer> buffer_for(
    std::string_view instance, const std::shared_ptr<API_Manager>& manager);

} // namespace gige_mel
