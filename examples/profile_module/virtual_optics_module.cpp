#include "profiles/profile_module.hpp"
#include "profiles/image_field_of_view.hpp"
#include "profiles/mono_free_running_device.hpp"
#include "profiles/mono_free_running_profile.hpp"
#if VIRTUAL_OPTICS_WITH_NAVIGATION
#include "navigation_record_policy.hpp"
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace b = gige_mel::backend;
namespace p = b::profiles;
namespace m = p::module;
namespace mono = p::mono_free_running;
namespace {

/// @brief Explicit virtual deployment data, not inferred from native features.
/// Pixel edges span [0, sensor_width] and [0, sensor_height], increasing from
/// the top left to the right and down. Focal lengths and principal point are
/// in pixels in that same coordinate system; this models no physical lens.
struct VirtualOptics {
    std::uint32_t sensor_width, sensor_height;
    double fx, fy, cx, cy;
};
constexpr VirtualOptics fixture{384, 256, 140.0, 95.0, 192.0, 128.0};

std::atomic<unsigned> live_profiles{}, live_states{};
// Observes actual unload, not ownership. This environment variable is solely
// the existing test unload probe; it never supplies or changes optical data.
struct UnloadObserver {
    ~UnloadObserver() {
        if (live_profiles || live_states) std::abort();
        if (const auto* path = std::getenv("GIGE_MEL_TEST_UNLOAD_LOG")) {
            if (auto* file = std::fopen(path, "a")) { std::fputs("unloaded\n", file); std::fclose(file); }
        }
    }
} observer;

/// @brief Inherit configuration and frame policy; add a pure ROI interpretation.
class State final : public p::MonoFreeRunningProfileState, public p::IProfileImageFieldOfView
#if VIRTUAL_OPTICS_WITH_NAVIGATION
    , public p::IProfileNavigationHandler
#endif
{
public:
    /// Own the virtual deployment model alongside immutable native admission facts.
    State(mono::NativeSnapshot snapshot, b::CameraCapabilities caps, VirtualOptics optics)
        : MonoFreeRunningProfileState(std::move(snapshot), std::move(caps)), optics_(optics) { ++live_states; }
    ~State() override { --live_states; }

    /// Interpret a borrowed, complete actual configuration; retain no references.
    /// The closed-open pixel ROI must have positive extent and fit inside the
    /// fixture sensor. Subtract the angular directions at its two pixel edges.
    b::Result<b::ImageFieldOfView> image_field_of_view(const b::CameraConfiguration& c) const override {
        if (!c.width || !c.height || c.width > optics_.sensor_width || c.height > optics_.sensor_height ||
            c.offset_x > optics_.sensor_width - c.width || c.offset_y > optics_.sensor_height - c.height)
            return b::Error{b::ErrorCode::invalid_argument, "ROI is outside the virtual optics sensor"};
        const auto extent = [](double start, double size, double center, double focal) {
            return std::atan2(start + size - center, focal) - std::atan2(start - center, focal);
        };
        return b::ImageFieldOfView{extent(c.offset_x, c.width, optics_.cx, optics_.fx),
                                  extent(c.offset_y, c.height, optics_.cy, optics_.fy)};
    }
#if VIRTUAL_OPTICS_WITH_NAVIGATION
    /// Test-only composition with a fictional navigation record; no transport.
    b::Result<p::NavigationOperation> prepare_navigation(const b::NavigationReport& report) const override {
        if (report.state == b::NavigationSolutionState::aligning)
            return b::Error{b::ErrorCode::unsupported, "virtual fixture does not encode Aligning"};
        return p::NavigationOperation{std::string(navigation_record::format), report, navigation_record::encode(report)};
    }
#endif
private:
    const VirtualOptics optics_;
};

#if VIRTUAL_OPTICS_WITH_NAVIGATION
#define VIRTUAL_OPTICS_PROFILE_ID "test_virtual_optics_and_navigation_v1"
#else
#define VIRTUAL_OPTICS_PROFILE_ID "test_virtual_optics_v1"
#endif

/// @brief SDK-free profile explicitly creating its derived state on admission.
class Profile final : public p::MonoFreeRunningProfile {
public:
    /// Construct with explicit immutable fixture deployment data; no I/O.
    explicit Profile(VirtualOptics optics) : optics_(optics) { ++live_profiles; }
    ~Profile() override { --live_profiles; }
    std::string_view id() const noexcept override { return VIRTUAL_OPTICS_PROFILE_ID; }

    /// Read native facts, require the fixture sensor bounds, and copy in the model.
    b::Result<std::shared_ptr<const p::ICameraProfileState>> open(b::device_features::IDeviceFeatures& features) const override {
        auto snapshot = mono::read_snapshot(features);
        if (!snapshot) return snapshot.error();
        auto caps = mono::capabilities_from(snapshot.value());
        if (!caps) return caps.error();
        if (caps.value().sensor_width != optics_.sensor_width || caps.value().sensor_height != optics_.sensor_height)
            return b::Error{b::ErrorCode::unsupported, "virtual optics requires the 384x256 fixture sensor"};
        return std::shared_ptr<const p::ICameraProfileState>(
            std::make_shared<State>(std::move(snapshot).value(), std::move(caps).value(), optics_));
    }
private:
    const VirtualOptics optics_;
};

std::int32_t create(const p::ICameraProfile** out, char* error, std::uint32_t capacity) noexcept {
    return m::guarded_create(out, error, capacity, [] { return new Profile(fixture); });
}
void destroy(const p::ICameraProfile* profile) noexcept { delete profile; }

// v1 requires a recognized SDK backend bit. The fixture selects this profile
// explicitly; the descriptor does not qualify optics or forwarding on an SDK.
const m::Descriptor descriptor{m::api_version, sizeof(m::Descriptor), m::interface_version,
    m::aravis | m::pleora, GIGE_MEL_PROFILE_BUILD_ID, VIRTUAL_OPTICS_PROFILE_ID, "gige-mel virtual fixture", "1", create, destroy};
} // namespace

extern "C" __attribute__((visibility("default")))
const m::Descriptor* gige_mel_camera_profile_module_v1() noexcept { return &descriptor; }