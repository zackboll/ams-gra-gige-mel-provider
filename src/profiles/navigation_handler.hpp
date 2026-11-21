#pragma once

// Optional, internal C++20 companion to ICameraProfileState (GIGE-MEL-047).
// Part of the matched-C++ profile-module identity closure; not a stable ABI,
// not installed and not a detector wire format. No MEL or SDK declarations.
#include "camera_profile.hpp"

#include <gige_mel/navigation.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace gige_mel::backend::profiles {

/// @brief Owned result of profile-specific navigation preparation.
/// A plain value: only standard-library members, no virtual functions, deleters,
/// borrowed references or handles. It may therefore outlive the profile state
/// and the dynamically loaded module that produced it, and is destroyed by
/// whoever owns it last. Preparing it establishes sensor-specific policy only;
/// it is not transmitted, acknowledged, accepted by a detector or associated
/// with imagery.
struct NavigationOperation {
    /// Nonempty, profile-defined label for the payload layout. Identifies the
    /// profile's own encoding; there is no universal detector format.
    std::string format;
    /// Complete copy of the submitted report, unchanged (no coordinate, time
    /// or unit conversion). The bridge refuses an operation whose copy differs.
    NavigationReport report;
    /// Profile-defined bytes for a backend-owned executor; may be empty.
    std::vector<std::byte> payload;
    bool operator==(const NavigationOperation&) const = default;
};

/// @brief Optional profile capability: interpret, validate and prepare navigation.
/// Implement it on the same concrete object as ICameraProfileState. Absence
/// means the profile provides no navigation processing; no default handler
/// exists. Discover it with navigation_handler(). Base profile and state
/// vtables are unchanged by this interface.
class IProfileNavigationHandler {
public:
    virtual ~IProfileNavigationHandler() = default;

    /// @brief Apply profile policy to one report and build an owned operation.
    /// @param report Borrowed for this call only; copy what the result needs.
    /// The common provider has already validated it structurally; the profile
    /// may apply stricter sensor-specific rules but must not rewrite values.
    /// @return An operation with a nonempty format and an unchanged report copy,
    /// or a diagnostic: `unsupported` for a profile-specific unsupported
    /// condition, `invalid_argument` for values outside profile policy. Errors
    /// are definite: nothing has been submitted.
    /// @note Bounded and synchronous: no native device I/O, waits, retained
    /// references, workers, queues, provider or consumer calls. May run on any
    /// submission thread; use immutable state or synchronized instrumentation.
    /// Unexpected exceptions are contained by the bridge as preparation failures.
    virtual Result<NavigationOperation> prepare_navigation(const NavigationReport& report) const = 0;
};

/// Return the optional handler implemented by this state object, or null when
/// the profile provides none. The result borrows from `state` and is valid only
/// while that exact object lives; host module wrappers preserve the capability.
inline const IProfileNavigationHandler* navigation_handler(const ICameraProfileState& state) noexcept {
    return dynamic_cast<const IProfileNavigationHandler*>(&state);
}

} // namespace gige_mel::backend::profiles
