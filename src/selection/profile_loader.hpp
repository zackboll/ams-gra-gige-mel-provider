#pragma once
#include "backend_catalog.hpp"

namespace gige_mel::selection {
// Validated metadata with a probe profile and an independent per-selection
// factory. Treat as immutable after load; compose only explicit compatibility.
// Profile and every state independently pin the module. Never a force-unload.
struct LoadedProfile {
    BackendCatalog::Profile profile;
    std::uint32_t compatible_backends;
    BackendCatalog::ProfileConstructor create;
};
backend::Result<LoadedProfile> load_profile_module(const std::string& absolute_path);
backend::Status register_loaded_profile(BackendCatalog&, const LoadedProfile&);
} // namespace gige_mel::selection