#pragma once

#include "camera_profile.hpp"
#include "profile_build_identity.hpp"
#include <exception>

/// Matched-C++ profile modules; C linkage is used only for symbol discovery.
/// Virtual objects, STL types and shared ownership cross the boundary. This is
/// not a stable C or C++ ABI. Build with matching interface sources, compiler,
/// standard library/ABI, sanitizer and build settings.
namespace gige_mel::backend::profiles::module {
/// Version of the discovery descriptor contract.
inline constexpr std::uint32_t api_version = 1;
/// Revision of the C++ profile and feature-service interface.
inline constexpr std::uint32_t interface_version = 1;
inline constexpr std::uint32_t aravis = 1;
inline constexpr std::uint32_t pleora = 2;
inline constexpr char entry_point[] = "gige_mel_camera_profile_module_v1";
static_assert(sizeof(GIGE_MEL_PROFILE_BUILD_ID) <= 512, "profile build identity exceeds descriptor field");

/// @brief Immutable module-owned v1 descriptor, valid while the module is mapped.
/// The loader checks version and exact size before inspecting remaining fields.
/// Matching metadata does not prove compiler/build compatibility, backend
/// behavior, device qualification or that executable module code is safe.
struct Descriptor {
    std::uint32_t api;                  ///< Exact api_version.
    std::uint32_t size;                 ///< Exact sizeof(Descriptor), not a minimum.
    std::uint32_t interface;            ///< Exact interface_version.
    std::uint32_t compatible_backends;  ///< Nonzero subset of aravis and pleora bits; explicit promises only.
    char build_identity[512];           ///< Exact generated GIGE_MEL_PROFILE_BUILD_ID, NUL terminated.
    char profile_id[128];               ///< Nonempty, NUL-terminated ID matching every produced profile.
    char vendor[128];                   ///< Nonempty, NUL-terminated vendor metadata, not authentication.
    char module_version[64];           ///< Nonempty, NUL-terminated module release metadata.

    /// Construct an independent profile without device I/O. Return zero with a
    /// non-null object, or nonzero with null output and diagnostic text. The
    /// host owns the output slot and error buffer for this call; write at most
    /// capacity bytes, NUL terminating if capacity is nonzero. Never retain either
    /// or allow exceptions to escape. The host owns the returned profile and
    /// retires it through the matching destroy, not host-side delete.
    std::int32_t (*create)(const ICameraProfile** out, char* error, std::uint32_t capacity) noexcept;

    /// Retire one returned profile while the module is still mapped. No throw.
    /// States may survive it; host wrappers independently pin the module through
    /// state destruction and release of its module-side shared ownership.
    void (*destroy)(const ICameraProfile*) noexcept;
};

/// Type of the discovery entry; return a borrowed descriptor without throwing.
using EntryPoint = const Descriptor* (*)() noexcept;

/// Contain factory exceptions inside the module and translate null or throwing
/// factories into nonzero status and a bounded diagnostic. Requires a valid
/// output pointer and writable error storage when capacity is nonzero.
template <class Factory>
std::int32_t guarded_create(const ICameraProfile** out, char* error, std::uint32_t capacity,
                            Factory factory) noexcept {
    *out = nullptr;
    const auto report = [&](const char* text) noexcept {
        if (!capacity) return;
        std::uint32_t n = 0;
        while (n + 1 < capacity && text[n]) { error[n] = text[n]; ++n; }
        error[n] = '\0';
    };
    try {
        *out = factory();
        if (*out) return 0;
        report("profile factory returned null");
    } catch (const std::exception& e) {
        report(e.what());
    } catch (...) {
        report("unexpected profile factory exception");
    }
    return 1;
}
} // namespace gige_mel::backend::profiles::module

/// Exact exported discovery symbol. It does not make the returned C++ contract
/// ABI-stable. Ordinary profile/state calls should report expected failures via
/// Result/Status; the loader catches open exceptions, not all state-method calls.
extern "C" __attribute__((visibility("default")))
const gige_mel::backend::profiles::module::Descriptor* gige_mel_camera_profile_module_v1() noexcept;