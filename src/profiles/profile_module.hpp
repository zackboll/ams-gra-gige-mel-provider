#pragma once

#include "camera_profile.hpp"
#include "profile_build_identity.hpp"
#include <exception>

// Discovery only is C linkage. All callbacks MUST contain exceptions in-module.
// Objects/virtual calls/shared_ptr require the matched C++ build described in
// camera-profile-authoring.md. This is not a cross-toolchain stable C ABI.
namespace gige_mel::backend::profiles::module {
inline constexpr std::uint32_t api_version = 1;
inline constexpr std::uint32_t interface_version = 1;
inline constexpr std::uint32_t aravis = 1;
inline constexpr std::uint32_t pleora = 2;
inline constexpr char entry_point[] = "gige_mel_camera_profile_module_v1";
static_assert(sizeof(GIGE_MEL_PROFILE_BUILD_ID) <= 512, "profile build identity exceeds descriptor field");

struct Descriptor {
    std::uint32_t api;
    std::uint32_t size;
    std::uint32_t interface;
    std::uint32_t compatible_backends; // explicit promises, not camera qualification
    char build_identity[512];
    char profile_id[128]; // nonempty, NUL terminated; immutable module-owned descriptor
    char vendor[128];
    char module_version[64];
    // Host owns error buffer; callback writes a terminated diagnostic on error.
    // On success host owns *out until exactly one matching destroy call.
    std::int32_t (*create)(const ICameraProfile** out, char* error, std::uint32_t capacity) noexcept;
    void (*destroy)(const ICameraProfile*) noexcept;
};
using EntryPoint = const Descriptor* (*)() noexcept;

// Use inside the module: unexpected C++ factory exceptions become a useful
// project error without unwinding through a C callback. Destroy must be noexcept.
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

extern "C" __attribute__((visibility("default")))
const gige_mel::backend::profiles::module::Descriptor* gige_mel_camera_profile_module_v1() noexcept;