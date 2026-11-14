#include "backend_catalog.hpp"

#include <exception>
#include <new>
#include <algorithm>
#include <set>

namespace gige_mel::selection {
namespace b = backend;

BackendCatalog::BackendCatalog() {
    profiles_.emplace(std::string(mono_free_running_v1), [] { return b::profiles::make_mono_free_running_profile(); });
}

b::Status BackendCatalog::register_profile(std::string id, ProfileConstructor constructor) {
    if (id.empty() || id.find('\0') != std::string::npos || !constructor)
        return b::Error{b::ErrorCode::invalid_argument, "profile registration requires an ID and constructor"};
    if (profiles_.contains(id))
        return b::Error{b::ErrorCode::invalid_argument, "duplicate profile registration: " + id};
    profiles_.emplace(std::move(id), std::move(constructor));
    return {};
}

b::Status BackendCatalog::register_backend(std::string id, std::vector<std::string> compatible,
                                            ProfileBackendConstructor constructor) {
    if (id.empty() || id.find('\0') != std::string::npos || !constructor || compatible.empty())
        return b::Error{b::ErrorCode::invalid_argument, "backend registration requires ID, profiles and constructor"};
    if (entries_.contains(id))
        return b::Error{b::ErrorCode::invalid_argument, "duplicate backend registration: " + id};
    std::set<std::string> unique;
    for (const auto& profile : compatible) {
        if (!profiles_.contains(profile))
            return b::Error{b::ErrorCode::unsupported, "profile is not registered: " + profile};
        if (!unique.insert(profile).second)
            return b::Error{b::ErrorCode::invalid_argument, "duplicate compatible profile: " + profile};
    }
    entries_.emplace(std::move(id), Entry{std::move(compatible), std::move(constructor)});
    return {};
}

b::Status BackendCatalog::register_backend(std::string id, std::string profile_id, Constructor constructor) {
    if (id.empty() || id.find('\0') != std::string::npos || !constructor)
        return b::Error{b::ErrorCode::invalid_argument, "backend registration requires an ID and constructor"};
    if (profile_id != mono_free_running_v1)
        return b::Error{b::ErrorCode::unsupported, "unsupported profile: " + profile_id};
    // Legacy fixed-policy constructors can ONLY advertise the historical default.
    return register_backend(std::move(id), std::vector<std::string>{std::move(profile_id)},
        [constructor = std::move(constructor)](Profile) { return constructor(); });
}

std::vector<std::string> BackendCatalog::backend_ids() const {
    std::vector<std::string> ids;
    for (const auto& [id, entry] : entries_) {
        (void)entry;
        ids.push_back(id);
    }
    return ids;
}

b::Result<std::shared_ptr<b::ICameraBackend>> BackendCatalog::create(
    std::string_view id, std::string_view profile_id) const {
    const auto found = entries_.find(id);
    if (found == entries_.end())
        return b::Error{b::ErrorCode::not_found, "backend is not registered: " + std::string(id)};
    if (std::find(found->second.profile_ids.begin(), found->second.profile_ids.end(), profile_id) == found->second.profile_ids.end())
        return b::Error{b::ErrorCode::unsupported, "unsupported profile: " + std::string(profile_id)};
    try {
        const auto profile_factory = profiles_.find(profile_id);
        if (profile_factory == profiles_.end())
            return b::Error{b::ErrorCode::unsupported, "profile is not registered: " + std::string(profile_id)};
        auto profile = profile_factory->second();
        if (!profile) return profile.error();
        if (!profile.value() || profile.value()->id() != profile_id)
            return b::Error{b::ErrorCode::invalid_argument, "profile constructor returned null or mismatched ID"};
        auto result = found->second.constructor(std::move(profile).value());
        if (!result) return result.error();
        if (!result.value())
            return b::Error{b::ErrorCode::invalid_argument, "backend constructor returned null: " + std::string(id)};
        if (result.value()->id() != id)
            return b::Error{b::ErrorCode::invalid_argument, "backend constructor ID mismatch: " + std::string(id)};
        return result;
    } catch (const std::bad_alloc&) {
        return b::Error{b::ErrorCode::resource_exhausted, "backend constructor allocation failed: " + std::string(id)};
    } catch (const std::exception& error) {
        return b::Error{b::ErrorCode::io_error, "backend constructor failed: " + std::string(id) + ": " + error.what()};
    } catch (...) {
        return b::Error{b::ErrorCode::io_error, "backend constructor failed: " + std::string(id) + ": unknown exception"};
    }
}

} // namespace gige_mel::selection
