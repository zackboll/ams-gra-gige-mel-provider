#include "profiles/profile_module.hpp"
#include "rate_limited_policy.hpp"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace p = gige_mel::backend::profiles;
namespace m = p::module;
namespace {
// Test-only observation of actual ELF finalization. Never used to manage lifetime.
struct UnloadObserver {
    ~UnloadObserver() {
        if (rate_limited::live_profiles || rate_limited::live_states) std::abort();
        if (const auto* path = std::getenv("GIGE_MEL_TEST_UNLOAD_LOG")) {
            if (auto* file = std::fopen(path, "a")) { std::fputs("unloaded\n", file); std::fclose(file); }
        }
    }
} observer;

std::int32_t create(const p::ICameraProfile** out, char* error, std::uint32_t capacity) noexcept {
#if MODULE_VARIANT == 16
    *out = nullptr;
    constexpr char reason[] = "expected factory error";
    if (capacity >= sizeof(reason)) std::copy(std::begin(reason), std::end(reason), error);
    return 1;
#endif
#if MODULE_VARIANT == 2 || MODULE_VARIANT == 3 || MODULE_VARIANT == 4 || MODULE_VARIANT == 8 || MODULE_VARIANT == 12 || MODULE_VARIANT == 14 || MODULE_VARIANT == 15
    std::abort(); // compatibility rejection MUST precede any factory call
#endif
#if MODULE_VARIANT == 13
    (void)m::guarded_create(out, error, capacity, [] {
        return new rate_limited::Profile(std::make_shared<rate_limited::Counts>());
    });
    return 7; // deliberately malformed error+object; host must destroy it
#else
    return m::guarded_create(out, error, capacity, []() -> const p::ICameraProfile* {
#if MODULE_VARIANT == 9
        return nullptr;
#elif MODULE_VARIANT == 10
        throw std::runtime_error("intentional unexpected factory exception");
#elif MODULE_VARIANT == 11
        throw 42;
#else
        return new rate_limited::Profile(std::make_shared<rate_limited::Counts>());
#endif
    });
#endif
}
void destroy(const p::ICameraProfile* profile) noexcept { delete profile; }
[[maybe_unused]] const m::Descriptor descriptor = [] {
    m::Descriptor d{m::api_version, sizeof(m::Descriptor), m::interface_version,
        m::aravis | m::pleora, GIGE_MEL_PROFILE_BUILD_ID, "test_rate_limited_v1",
        "gige-mel example", "1", create, destroy};
#if MODULE_VARIANT == 2
    d.api = 99;
#elif MODULE_VARIANT == 3
    d.size = 8;
#elif MODULE_VARIANT == 4
    d.build_identity[0] = '!';
#elif MODULE_VARIANT == 6
    d.create = nullptr;
#elif MODULE_VARIANT == 7
    d.profile_id[0] = '!';
#elif MODULE_VARIANT == 8
    d.compatible_backends = 0;
#elif MODULE_VARIANT == 12
    d.interface = 99;
#elif MODULE_VARIANT == 14
    for (auto& c : d.profile_id) c = 'x'; // no terminator
#elif MODULE_VARIANT == 15
    d.destroy = nullptr;
#elif MODULE_VARIANT == 17
    d.compatible_backends = m::aravis;
#endif
    return d;
}();
}
#if MODULE_VARIANT != 1
extern "C" __attribute__((visibility("default")))
const m::Descriptor* gige_mel_camera_profile_module_v1() noexcept {
#if MODULE_VARIANT == 5
    return nullptr;
#else
    return &descriptor;
#endif
}
#endif