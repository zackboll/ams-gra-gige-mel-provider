#pragma once

#include <gige_mel/backend.hpp>
#include "profiles/camera_profile.hpp"

#include <functional>
#include <map>

namespace gige_mel::selection {

// Internal C++ composition; neither installed nor a stable plugin ABI.
// This binding name does not replace the profile's historical diagnostic ID.
inline constexpr auto mono_free_running_v1 = backend::profiles::mono_free_running_v1;

/// @brief Internal, owned registry of backend and camera-profile factories.
/// Registration stores callable objects and identity/compatibility declarations;
/// it invokes no factory and performs no camera I/O. Finish registration before
/// sharing for selection. Mutation must not race with catalog use; factories
/// own their thread safety when selections run concurrently.
class BackendCatalog {
public:
    using BackendResult = backend::Result<std::shared_ptr<backend::ICameraBackend>>;
    using Constructor = std::function<BackendResult()>;
    using Profile = std::shared_ptr<const backend::profiles::ICameraProfile>;
    using ProfileConstructor = std::function<backend::Result<Profile>()>;
    using ProfileBackendConstructor = std::function<BackendResult(Profile)>;

    /// Install the built-in monochrome profile factory, but no backends.
    BackendCatalog();

    /// Store a profile factory under an exact ID. Empty/NUL-containing or
    /// duplicate IDs and empty callables are invalid_argument. Identity of the
    /// resulting object is checked at create, not registration.
    backend::Status register_profile(std::string id, ProfileConstructor);

    /// @brief Register a backend factory that receives the selected profile.
    /// @param[in] id Exact, nonempty, NUL-free backend ID.
    /// @param[in] compatible_profiles Nonempty list of distinct registered IDs.
    /// Compatibility is a composition promise, not device admission.
    /// @param[in] constructor Owned callable: construct an independent backend, inject the
    /// supplied profile and not enumerate, open or inspect devices.
    /// @return Success, invalid_argument for malformed/duplicate registration,
    /// or unsupported for an unregistered compatible profile. No factory runs.
    backend::Status register_backend(std::string id, std::vector<std::string> compatible_profiles,
                                    ProfileBackendConstructor);

    /// Fixed-policy overload. Only mono_free_running_v1 is admitted; other
    /// profile IDs are unsupported. The no-argument factory must construct an
    /// independent backend without device I/O; it does not receive the profile.
    backend::Status register_backend(std::string id, std::string profile_id, Constructor constructor);

    /// List registered backend IDs in catalog order (not discovered devices).
    [[nodiscard]] std::vector<std::string> backend_ids() const;

    /// @brief Construct the requested backend and profile without fallback.
    /// @param[in] id Exact registered backend ID.
    /// @param[in] profile_id Exact profile ID allowed by that registration.
    /// @return Owned backend. Unknown backend is not_found; incompatible profile
    /// is unsupported. Propagate factory failures, reject null or mismatched
    /// objects, and translate factory exceptions (allocation to resource_exhausted,
    /// others to io_error).
    /// @note Compatibility is checked before either factory runs. Independence,
    /// injection and absence of factory I/O are obligations of the composition,
    /// not properties the catalog can enforce on arbitrary callables.
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
