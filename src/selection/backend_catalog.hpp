#pragma once

#include <gige_mel/backend.hpp>

#include <functional>
#include <map>

namespace gige_mel::selection {

// Internal source-level composition only; neither installed nor a plugin ABI.
// This binding name does not replace the profile's historical diagnostic ID.
inline constexpr std::string_view mono_free_running_v1 = "mono_free_running_v1";

class BackendCatalog {
public:
    using BackendResult = backend::Result<std::shared_ptr<backend::ICameraBackend>>;
    using Constructor = std::function<BackendResult()>;

    // Constructors must create independent backends without device I/O. Profile
    // support is an explicit promise by the composition, not inferred from IDs.
    // Register on the owning composition thread before sharing for selection;
    // concurrent mutation is not supported. Constructors own their thread safety.
    backend::Status register_backend(std::string id, std::string profile_id, Constructor constructor);
    [[nodiscard]] std::vector<std::string> backend_ids() const;
    backend::Result<std::shared_ptr<backend::ICameraBackend>> create(
        std::string_view id, std::string_view profile_id) const;

private:
    struct Entry {
        std::string profile_id;
        Constructor constructor;
    };
    std::map<std::string, Entry, std::less<>> entries_;
};

} // namespace gige_mel::selection
