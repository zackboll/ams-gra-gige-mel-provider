#pragma once

#include "backend_catalog.hpp"
#include "profile_loader.hpp"

namespace gige_mel::selection {

// Explicitly owned, build-local composition. Optional SDK factories are linked
// here, not into the catalog or installed common MEL provider. No device I/O.
backend::Result<BackendCatalog> make_builtin_catalog(const std::vector<LoadedProfile>& modules = {});

} // namespace gige_mel::selection
