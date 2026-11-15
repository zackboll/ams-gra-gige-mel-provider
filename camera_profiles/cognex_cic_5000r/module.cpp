#include "profile.hpp"
#include "profiles/profile_module.hpp"

namespace p = gige_mel::backend::profiles;
namespace m = p::module;
namespace {
std::int32_t create(const p::ICameraProfile** out, char* error, std::uint32_t capacity) noexcept {
    return m::guarded_create(out, error, capacity, [] { return new p::cognex::CognexCIC5000RProfile; });
}
void destroy(const p::ICameraProfile* profile) noexcept { delete profile; }
const m::Descriptor descriptor{m::api_version, sizeof(m::Descriptor), m::interface_version,
    m::aravis | m::pleora, GIGE_MEL_PROFILE_BUILD_ID, "cognex_cic_5000r_v1",
    "gige-mel", "1", create, destroy};
}
extern "C" __attribute__((visibility("default")))
const m::Descriptor* gige_mel_camera_profile_module_v1() noexcept { return &descriptor; }