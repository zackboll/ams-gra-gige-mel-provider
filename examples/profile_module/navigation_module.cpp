#include "profiles/profile_module.hpp"
#include "navigation_record_policy.hpp"
#include <cstdio>
#include <cstdlib>

// TEST ONLY module for the optional navigation handler (GIGE-MEL-047). The
// descriptor layout, discovery symbol and interface revision are the unchanged
// v1 contract; the capability is discovered on the returned state object.
namespace p = gige_mel::backend::profiles;
namespace m = p::module;
namespace {
// Test-only observation of actual ELF finalization. Never used to manage lifetime.
struct UnloadObserver {
    ~UnloadObserver() {
        if (navigation_record::live_profiles || navigation_record::live_states) std::abort();
        if (const auto* path = std::getenv("GIGE_MEL_TEST_UNLOAD_LOG")) {
            if (auto* file = std::fopen(path, "a")) { std::fputs("unloaded\n", file); std::fclose(file); }
        }
    }
} observer;

std::int32_t create(const p::ICameraProfile** out, char* error, std::uint32_t capacity) noexcept {
#if NAVIGATION_MODULE_VARIANT == 1
    (void)out; (void)error; (void)capacity;
    std::abort(); // the identity mismatch MUST be rejected before any factory call
#else
    return m::guarded_create(out, error, capacity, [] {
        return new navigation_record::Profile(std::make_shared<navigation_record::Controls>());
    });
#endif
}
void destroy(const p::ICameraProfile* profile) noexcept { delete profile; }
[[maybe_unused]] const m::Descriptor descriptor = [] {
    m::Descriptor d{m::api_version, sizeof(m::Descriptor), m::interface_version,
        m::aravis | m::pleora, GIGE_MEL_PROFILE_BUILD_ID, "test_navigation_record_v1",
        "gige-mel example", "1", create, destroy};
#if NAVIGATION_MODULE_VARIANT == 1
    d.build_identity[0] = '!'; // simulate a module built from a different interface closure
#endif
    return d;
}();
}
extern "C" __attribute__((visibility("default")))
const m::Descriptor* gige_mel_camera_profile_module_v1() noexcept { return &descriptor; }
