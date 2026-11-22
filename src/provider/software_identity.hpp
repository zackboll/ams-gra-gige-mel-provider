#pragma once

#include <cstdint>

// The provider software version is the CMake project version, supplied as
// private definitions. It is distinct from the MEL API version and from the
// pinned upstream dependency revisions.
#if !defined(GIGE_MEL_VERSION_MAJOR) || !defined(GIGE_MEL_VERSION_MINOR) || !defined(GIGE_MEL_VERSION_PATCH)
#error "GIGE_MEL_VERSION_MAJOR/MINOR/PATCH must be defined from the CMake project version"
#endif
#define GIGE_MEL_STRINGIFY_IMPL(value) #value
#define GIGE_MEL_STRINGIFY(value) GIGE_MEL_STRINGIFY_IMPL(value)
#define GIGE_MEL_VERSION_TEXT GIGE_MEL_STRINGIFY(GIGE_MEL_VERSION_MAJOR) "." \
    GIGE_MEL_STRINGIFY(GIGE_MEL_VERSION_MINOR) "." GIGE_MEL_STRINGIFY(GIGE_MEL_VERSION_PATCH)

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/// @brief One CMake-derived software identity for Health About, VersionInfo
/// text and the subsystem CSCI version, distinct from API/dependency versions.
struct ProviderSoftware {
    static constexpr const char* name = "ams-gra-gige-mel-provider";
    static constexpr const char* version_text = GIGE_MEL_VERSION_TEXT;
    static constexpr std::uint32_t major = GIGE_MEL_VERSION_MAJOR;
    static constexpr std::uint32_t minor = GIGE_MEL_VERSION_MINOR;
    static constexpr std::uint32_t patch = GIGE_MEL_VERSION_PATCH;
};

} // namespace provider_detail
} // namespace gige_mel
