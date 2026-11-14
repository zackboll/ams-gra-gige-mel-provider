#pragma once

#include "backend_catalog.hpp"
#include <gige_mel/provider.hpp>

namespace gige_mel::selection {

struct SessionSelection {
    std::string backend_id;
    std::string device_id;
    std::string profile_id;
    ProviderSettings settings;
};

struct SelectedSession {
    std::string backend_id;
    std::string device_id;
    std::string profile_id;
    ProviderSettings settings;
    std::shared_ptr<backend::ICameraBackend> backend;
};

// Validates/binds identities only. device_id overrides settings.device_id;
// settings.instance is passed through, never interpreted as a backend selector.
// Neither this function nor make_api_manager opens or inspects a device.
backend::Result<SelectedSession> select_session(const BackendCatalog&, SessionSelection);

} // namespace gige_mel::selection
