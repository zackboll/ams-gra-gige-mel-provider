#include "backend_catalog.hpp"

#include <exception>
#include <new>

namespace gige_mel::selection {
namespace b = backend;

b::Status BackendCatalog::register_backend(std::string id, std::string profile_id, Constructor constructor) {
    if (id.empty() || id.find('\0') != std::string::npos || !constructor)
        return b::Error{b::ErrorCode::invalid_argument, "backend registration requires an ID and constructor"};
    if (profile_id != mono_free_running_v1)
        return b::Error{b::ErrorCode::unsupported, "unsupported profile: " + profile_id};
    if (entries_.contains(id))
        return b::Error{b::ErrorCode::invalid_argument, "duplicate backend registration: " + id};
    entries_.emplace(std::move(id), Entry{std::move(profile_id), std::move(constructor)});
    return {};
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
    if (profile_id != found->second.profile_id)
        return b::Error{b::ErrorCode::unsupported, "unsupported profile: " + std::string(profile_id)};
    try {
        auto result = found->second.constructor();
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
