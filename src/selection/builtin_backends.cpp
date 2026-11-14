#include "builtin_backends.hpp"

#ifdef GIGE_MEL_SELECT_ARAVIS
#include "backends/aravis/aravis_backend.hpp"
#endif
#ifdef GIGE_MEL_SELECT_PLEORA
#include "backends/pleora/pleora_backend.hpp"
#endif

namespace gige_mel::selection {

backend::Result<BackendCatalog> make_builtin_catalog() {
    BackendCatalog catalog;
    auto status = catalog.register_backend("synthetic", std::string(mono_free_running_v1),
                                          [] { return backend::make_synthetic_backend(); });
    if (!status) return status.error();
#ifdef GIGE_MEL_SELECT_ARAVIS
    status = catalog.register_backend("aravis", std::vector<std::string>{std::string(mono_free_running_v1)},
                                      [](BackendCatalog::Profile p) { return backend::aravis::make_backend(std::move(p)); });
    if (!status) return status.error();
#endif
#ifdef GIGE_MEL_SELECT_PLEORA
    status = catalog.register_backend("pleora", std::vector<std::string>{std::string(mono_free_running_v1)},
                                      [](BackendCatalog::Profile p) { return backend::pleora::make_backend(std::move(p)); });
    if (!status) return status.error();
#endif
    return catalog;
}

} // namespace gige_mel::selection
