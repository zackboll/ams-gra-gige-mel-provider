#pragma once

#include <gige_mel/backend.hpp>
#include "profiles/camera_profile.hpp"

#include <functional>
#include <map>

namespace gige_mel::selection {

// Internal source-level composition only; neither installed nor a plugin ABI.
// This binding name does not replace the profile's historical diagnostic ID.
inline constexpr auto mono_free_running_v1 = backend::profiles::mono_free_running_v1;

class BackendCatalog {
public:
    using BackendResult = backend::Result<std::shared_ptr<backend::ICameraBackend>>;
    using Constructor = std::function<BackendResult()>;
    using Profile = std::shared_ptr<const backend::profiles::ICameraProfile>;
    using ProfileConstructor = std::function<backend::Result<Profile>()>;
    using ProfileBackendConstructor = std::function<BackendResult(Profile)>;

    // Each owned catalog includes the existing default profile factory, no I/O.
    BackendCatalog();
    backend::Status register_profile(std::string id, ProfileConstructor);
    backend::Status register_backend(std::string id, std::vector<std::string> compatible_profiles,
                                    ProfileBackendConstructor);

    // Compatibility entry for fixed-policy/default factories, not arbitrary
    // profile dispatch. Only mono_free_running_v1 is admitted by this overload.
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
        std::vector<std::string> profile_ids;
        ProfileBackendConstructor constructor;
    };
    std::map<std::string, Entry, std::less<>> entries_;
    std::map<std::string, ProfileConstructor, std::less<>> profiles_;
};

} // namespace gige_mel::selection
