#include <gige_mel/provider.hpp>
#include "provider_internal.hpp"

// This file is the backend-selection seam for the exported MEL factory path.
// Future optional SDK factories belong here (or in an ordinary C++ registry),
// while the common MEL channel implementation remains unchanged.
//
// These are exactly the upstream loader symbols: C linkage for lookup, C++ ABI
// for strings/shared_ptr. They are not a C-callable provider interface.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif
extern "C" __attribute__((visibility("default"))) std::shared_ptr<API_Manager> getAPI_Manager(const std::string& instance) {
    try {
        if (instance != "synthetic") return {};
        return gige_mel::make_api_manager({}, gige_mel::backend::make_synthetic_backend());
    } catch (...) { return {}; }
}

namespace ams::iface::irmel {
extern "C" IR_MEL_EXPORT std::shared_ptr<Control> getControl(std::string_view instance, std::shared_ptr<::API_Manager> manager) {
    try { return gige_mel::control_for(instance, manager); } catch (...) { return {}; }
}
extern "C" IR_MEL_EXPORT std::shared_ptr<Buffer> getBuffer(std::string_view instance, std::shared_ptr<::API_Manager> manager) {
    try { return gige_mel::buffer_for(instance, manager); } catch (...) { return {}; }
}
} // namespace ams::iface::irmel
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
