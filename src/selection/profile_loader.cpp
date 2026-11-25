#include "profile_loader.hpp"
#include "profiles/profile_module.hpp"
#include "profiles/navigation_handler.hpp"
#include "profiles/image_field_of_view.hpp"

#include <cstring>
#include <dlfcn.h>
#include <filesystem>

namespace gige_mel::selection {
namespace b = backend;
namespace p = b::profiles;
namespace m = p::module;
namespace {
b::Error invalid(std::string text) { return {b::ErrorCode::unsupported, "profile module: " + std::move(text)}; }
struct Module {
    void* handle;
    explicit Module(void* value) : handle(value) {}
    ~Module() { if (handle) dlclose(handle); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
};
template <std::size_t N> bool text(const char (&value)[N]) {
    return value[0] && std::memchr(value, 0, N);
}

class State : public p::ICameraProfileState {
public:
    State(std::shared_ptr<Module> module, std::shared_ptr<const p::ICameraProfileState> state)
        : module_(std::move(module)), state_(std::move(state)) {}
    b::CameraCapabilities capabilities() const override { return state_->capabilities(); }
    std::vector<std::pair<std::string, std::string>> native_diagnostics() const override { return state_->native_diagnostics(); }
    b::Result<b::CameraConfiguration> configuration(b::device_features::IDeviceFeatures& f, const p::ConnectionCheck& c) const override {
        return state_->configuration(f, c);
    }
    b::Result<b::CameraConfiguration> apply(b::device_features::IDeviceFeatures& f, const b::CameraConfiguration& r,
                                            const p::ConnectionCheck& c, const p::BeforeWrite& w) const override {
        return state_->apply(f, r, c, w);
    }
    b::Result<b::CameraConfiguration> preflight(b::device_features::IDeviceFeatures& f) const override { return state_->preflight(f); }
    p::FrameDecision validate_frame(const p::NativeFrameFacts& f, const b::CameraConfiguration& c, b::PixelFormat format) const override {
        return state_->validate_frame(f, c, format);
    }
private:
    // Declaration order is essential: plugin state AND its shared_ptr control
    // block/deleter must be retired before the final module reference.
    const std::shared_ptr<Module> module_;
    const std::shared_ptr<const p::ICameraProfileState> state_;
};

// Each forwarding base borrows a subobject of State::state_, whose lifetime
// ends before the module pin. Only compose bases for capabilities actually
// exposed by the module state.
class NavigationForwarder : public p::IProfileNavigationHandler {
public:
    explicit NavigationForwarder(const p::IProfileNavigationHandler& handler) : handler_(handler) {}
    b::Result<p::NavigationOperation> prepare_navigation(const b::NavigationReport& report) const override {
        return handler_.prepare_navigation(report);
    }
private:
    const p::IProfileNavigationHandler& handler_;
};

class FieldOfViewForwarder : public p::IProfileImageFieldOfView {
public:
    explicit FieldOfViewForwarder(const p::IProfileImageFieldOfView& handler) : handler_(handler) {}
    b::Result<b::ImageFieldOfView> image_field_of_view(const b::CameraConfiguration& configuration) const override {
        return handler_.image_field_of_view(configuration);
    }
private:
    const p::IProfileImageFieldOfView& handler_;
};

template<class... Forwarders>
class OptionalState final : public State, public Forwarders... {
public:
    template<class... Handlers>
    OptionalState(const std::shared_ptr<Module>& module, std::shared_ptr<const p::ICameraProfileState> state,
                  const Handlers&... handlers)
        : State(module, std::move(state)), Forwarders(handlers)... {}
};

std::shared_ptr<const p::ICameraProfileState> wrap_state(const std::shared_ptr<Module>& module,
                                                       std::shared_ptr<const p::ICameraProfileState> state) {
    const auto* navigation = p::navigation_handler(*state);
    const auto* fov = p::image_field_of_view_handler(*state);
    if (navigation && fov)
        return std::make_shared<OptionalState<NavigationForwarder, FieldOfViewForwarder>>(
            module, std::move(state), *navigation, *fov);
    if (navigation) return std::make_shared<OptionalState<NavigationForwarder>>(module, std::move(state), *navigation);
    if (fov) return std::make_shared<OptionalState<FieldOfViewForwarder>>(module, std::move(state), *fov);
    return std::make_shared<State>(module, std::move(state));
}

class Profile final : public p::ICameraProfile {
public:
    Profile(std::shared_ptr<Module> module, const p::ICameraProfile* profile, const m::Descriptor* descriptor)
        : module_(std::move(module)), profile_(profile), descriptor_(descriptor) {}
    ~Profile() override { descriptor_->destroy(profile_); }
    std::string_view id() const noexcept override { return descriptor_->profile_id; }
    b::Result<std::shared_ptr<const p::ICameraProfileState>> open(b::device_features::IDeviceFeatures& features) const override {
        try {
            auto state = profile_->open(features);
            if (!state) return state.error();
            if (!state.value()) return invalid("profile open returned null state");
            return wrap_state(module_, std::move(state).value());
        } catch (const std::exception& e) {
            return invalid(std::string("profile open exception: ") + e.what());
        } catch (...) {
            return invalid("profile open unexpected exception");
        }
    }
private:
    const std::shared_ptr<Module> module_;
    const p::ICameraProfile* profile_;
    const m::Descriptor* descriptor_;
};

b::Result<BackendCatalog::Profile> create_profile(const std::shared_ptr<Module>& module, const m::Descriptor* d) {
    const p::ICameraProfile* raw = nullptr;
    char diagnostic[512]{};
    const auto status = d->create(&raw, diagnostic, sizeof(diagnostic));
    auto destroy = [d](const p::ICameraProfile* value) { if (value) d->destroy(value); };
    std::unique_ptr<const p::ICameraProfile, decltype(destroy)> guard(raw, destroy);
    diagnostic[sizeof(diagnostic) - 1] = 0;
    if (status || !raw) return invalid(std::string("profile factory failed: ") + diagnostic);
    if (raw->id() != d->profile_id) return invalid("returned profile ID differs from descriptor");
    auto profile = std::make_shared<Profile>(module, raw, d);
    guard.release();
    return BackendCatalog::Profile(std::move(profile));
}
} // namespace

b::Result<LoadedProfile> load_profile_module(const std::string& path) try {
    if (path.empty() || path.find('\0') != std::string::npos || !std::filesystem::path(path).is_absolute())
        return b::Error{b::ErrorCode::invalid_argument, "profile module requires an explicit absolute path"};
    // Allocate owner before dlopen: allocation failure cannot leak an open handle.
    auto module = std::make_shared<Module>(nullptr);
    module->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module->handle) return b::Error{b::ErrorCode::not_found, "profile module dlopen " + path + ": " + dlerror()};
    dlerror();
    auto symbol = dlsym(module->handle, m::entry_point);
    const char* error = dlerror();
    if (error) return invalid(std::string("dlsym ") + m::entry_point + ": " + error);
    if (!symbol) return invalid("null entry point");
    const auto entry = reinterpret_cast<m::EntryPoint>(symbol);
    const auto* d = entry();
    if (!d) return invalid("null descriptor");
    // Read only the version/size prefix before interpreting the rest of v1.
    if (d->api != m::api_version) return invalid("unsupported API version");
    if (d->size != sizeof(m::Descriptor)) return invalid("incorrect descriptor size");
    if (d->interface != m::interface_version) return invalid("unsupported profile interface revision");
    if (!text(d->build_identity) || std::string_view(d->build_identity) != GIGE_MEL_PROFILE_BUILD_ID)
        return invalid("incompatible C++ build identity");
    if (!text(d->profile_id) || !text(d->vendor) || !text(d->module_version)) return invalid("malformed descriptor metadata");
    if (!d->compatible_backends || (d->compatible_backends & ~(m::aravis | m::pleora))) return invalid("missing or unknown backend compatibility declaration");
    if (!d->create || !d->destroy) return invalid("null profile factory/destructor");
    // Even an erroneous callback returning both an object and an error is cleaned
    // up while mapped. Descriptor callbacks themselves are contractually noexcept.
    auto profile = create_profile(module, d);
    if (!profile) return profile.error();
    return LoadedProfile{std::move(profile).value(), d->compatible_backends,
        [module, d] { return create_profile(module, d); }};
} catch (const std::bad_alloc&) {
    return b::Error{b::ErrorCode::resource_exhausted, "profile module allocation failed"};
} catch (const std::exception& e) {
    return b::Error{b::ErrorCode::io_error, std::string("profile module load failed: ") + e.what()};
} catch (...) {
    return b::Error{b::ErrorCode::io_error, "profile module load failed: unknown exception"};
}

b::Status register_loaded_profile(BackendCatalog& catalog, const LoadedProfile& loaded) {
    if (!loaded.profile || !loaded.create) return invalid("null loaded profile/factory");
    return catalog.register_profile(std::string(loaded.profile->id()), loaded.create);
}
} // namespace gige_mel::selection