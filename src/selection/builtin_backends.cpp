#include "builtin_backends.hpp"
#include "profiles/profile_module.hpp"

#ifdef GIGE_MEL_SELECT_ARAVIS
#include "backends/aravis/aravis_backend.hpp"
#endif
#ifdef GIGE_MEL_SELECT_PLEORA
#include "backends/pleora/pleora_backend.hpp"
#endif

namespace gige_mel::selection {

backend::Result<BackendCatalog> make_builtin_catalog(const std::vector<LoadedProfile>& modules) {
    BackendCatalog catalog;
    [[maybe_unused]] auto compatible = [&](std::uint32_t bit) {
        std::vector<std::string> ids{std::string(mono_free_running_v1)};
        for (const auto& module : modules)
            if (module.compatible_backends & bit) ids.emplace_back(module.profile->id());
        return ids;
    };
    for (const auto& module : modules) {
        auto registered = register_loaded_profile(catalog, module);
        if (!registered) return registered.error();
    }
    auto status = catalog.register_backend("synthetic", std::string(mono_free_running_v1),
                                          [] { return backend::make_synthetic_backend(); });
    if (!status) return status.error();
#ifdef GIGE_MEL_SELECT_ARAVIS
    status = catalog.register_backend(std::string(backend::aravis::backend_id), compatible(backend::profiles::module::aravis),
                                      [](BackendCatalog::Profile p) { return backend::aravis::make_backend(std::move(p)); });
    if (!status) return status.error();
#endif
#ifdef GIGE_MEL_SELECT_PLEORA
    status = catalog.register_backend(std::string(backend::pleora::backend_id), compatible(backend::profiles::module::pleora),
                                      [](BackendCatalog::Profile p) { return backend::pleora::make_backend(std::move(p)); });
    if (!status) return status.error();
#endif
    return catalog;
}

} // namespace gige_mel::selection
