#pragma once

#include "backend_catalog.hpp"
#include <gige_mel/provider.hpp>

namespace gige_mel::selection {

/// Exact internal composition request; no implicit selection defaults.
struct SessionSelection {
    std::string backend_id;    ///< Catalog backend ID, not a MEL instance label.
    std::string device_id;     ///< Opaque, exact backend-specific identity.
    std::string profile_id;    ///< Registered profile binding for the backend.
    ProviderSettings settings; ///< Provider options, with device_id overridden.
};

/// Offline binding; owns the constructed backend, not an opened device.
struct SelectedSession {
    std::string backend_id;
    std::string device_id;
    std::string profile_id;
    ProviderSettings settings;
    std::shared_ptr<backend::ICameraBackend> backend;
};

/// @brief Bind an explicit backend, profile and device without device I/O.
/// The catalog must already contain the compatible factories. Supply exact IDs
/// (device ID must be nonempty and NUL-free) and provider settings. Device ID
/// overrides settings.device_id; all other settings, including the MEL instance
/// label, pass through.
/// @return Bound backend/settings or identity/factory error, with no fallback.
/// Its later open rejects a different device ID and delegates the exact match.
/// @note No enumeration, device open or camera inspection here or at
/// make_api_manager construction. The adapter decides access, existence and
/// profile admission when explicitly opened; IDs do not establish physical
/// identity equivalence across adapters.
backend::Result<SelectedSession> select_session(const BackendCatalog&, SessionSelection);

} // namespace gige_mel::selection
