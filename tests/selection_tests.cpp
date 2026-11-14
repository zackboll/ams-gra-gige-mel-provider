#include "selection/session_selection.hpp"
#include "support/alternate_camera_profile.hpp"

#include <iostream>
#include <stdexcept>

namespace b = gige_mel::backend;
namespace s = gige_mel::selection;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

namespace {
struct Counts {
    unsigned created{}, destroyed{}, enumerated{}, opened{};
    std::string last_id;
};
class Probe final : public b::ICameraBackend {
public:
    explicit Probe(std::shared_ptr<Counts> counts) : counts_(std::move(counts)) { ++counts_->created; }
    ~Probe() override { ++counts_->destroyed; }
    std::string_view id() const noexcept override { return "probe"; }
    b::Result<std::vector<b::DeviceInfo>> enumerate() override {
        ++counts_->enumerated;
        return std::vector<b::DeviceInfo>{};
    }
    b::Result<std::shared_ptr<b::ICameraDevice>> open(std::string_view id) override {
        ++counts_->opened;
        counts_->last_id = id;
        return b::Error{b::ErrorCode::disconnected, "selected device is disconnected"};
    }
private:
    std::shared_ptr<Counts> counts_;
};
template <class T> T take(b::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
template <class T> void rejected(const b::Result<T>& result, b::ErrorCode code) {
    CHECK(!result);
    CHECK(result.error().code == code);
}
const std::string profile{s::mono_free_running_v1};

void validation() {
    s::BackendCatalog catalog;
    auto synthetic = [] { return b::make_synthetic_backend(); };
    rejected(catalog.register_backend("", profile, synthetic), b::ErrorCode::invalid_argument);
    rejected(catalog.register_backend("synthetic", profile, {}), b::ErrorCode::invalid_argument);
    rejected(catalog.register_backend("synthetic", "other", synthetic), b::ErrorCode::unsupported);
    CHECK(catalog.register_backend("synthetic", profile, synthetic));
    rejected(catalog.register_backend("synthetic", profile, synthetic), b::ErrorCode::invalid_argument);
    CHECK(catalog.backend_ids() == std::vector<std::string>{"synthetic"});
    CHECK(take(catalog.create("synthetic", profile))->id() == "synthetic");
    for (auto id : {"Synthetic", "synthetic ", "", "aravis", "pleora"})
        rejected(catalog.create(id, profile), b::ErrorCode::not_found);
    rejected(catalog.create("synthetic", "mono8-bootstrap"), b::ErrorCode::unsupported);
    rejected(s::select_session(catalog, {"synthetic", "", profile, {}}), b::ErrorCode::invalid_argument);
    rejected(s::select_session(catalog, {"synthetic", std::string{"id\0suffix", 9}, profile, {}}),
             b::ErrorCode::invalid_argument);
    CHECK(catalog.register_backend("null", profile, [] { return std::shared_ptr<b::ICameraBackend>{}; }));
    rejected(catalog.create("null", profile), b::ErrorCode::invalid_argument);
    CHECK(catalog.register_backend("throw", profile, []() -> b::Result<std::shared_ptr<b::ICameraBackend>> {
        throw std::runtime_error("constructor detail");
    }));
    auto thrown = catalog.create("throw", profile);
    rejected(thrown, b::ErrorCode::io_error);
    CHECK(thrown.error().message.find("constructor detail") != std::string::npos);
    CHECK(catalog.register_backend("allocation", profile, []() -> s::BackendCatalog::BackendResult {
        throw std::bad_alloc{};
    }));
    rejected(catalog.create("allocation", profile), b::ErrorCode::resource_exhausted);
    CHECK(catalog.register_backend("unknown-throw", profile, []() -> b::Result<std::shared_ptr<b::ICameraBackend>> {
        throw 42;
    }));
    rejected(catalog.create("unknown-throw", profile), b::ErrorCode::io_error);
    CHECK(catalog.register_backend("mismatch", profile, synthetic));
    rejected(catalog.create("mismatch", profile), b::ErrorCode::invalid_argument);
    CHECK(catalog.register_backend("failed", profile, []() -> b::Result<std::shared_ptr<b::ICameraBackend>> {
        return b::Error{b::ErrorCode::busy, "constructor error retained"};
    }));
    auto failed = catalog.create("failed", profile);
    rejected(failed, b::ErrorCode::busy);
    CHECK(failed.error().message == "constructor error retained");
}

void mel_lifecycle() {
    auto counts = std::make_shared<Counts>();
    s::BackendCatalog catalog;
    CHECK(catalog.register_backend("probe", profile, [counts]() -> std::shared_ptr<b::ICameraBackend> {
        return std::make_shared<Probe>(counts);
    }));
    auto selected = take(s::select_session(catalog, {"probe", "exact", profile, {}}));
    auto manager = gige_mel::make_api_manager(selected.settings, selected.backend);
    auto control = ams::iface::irmel::getControl(selected.settings.instance, manager);
    CHECK(control && counts->opened == 0 && counts->enumerated == 0);
    CHECK(control->init() != ams::iface::irmel::Return::Success);
    CHECK(counts->opened == 1 && counts->last_id == "exact" && counts->enumerated == 0);
}

void binding_and_no_fallback() {
    auto counts = std::make_shared<Counts>();
    s::BackendCatalog catalog;
    unsigned fallback = 0;
    CHECK(catalog.register_backend("probe", profile, [counts]() -> std::shared_ptr<b::ICameraBackend> {
        return std::make_shared<Probe>(counts);
    }));
    CHECK(catalog.register_backend("synthetic", profile, [&] { ++fallback; return b::make_synthetic_backend(); }));
    rejected(s::select_session(catalog, {"probe", "exact", "unknown", {}}), b::ErrorCode::unsupported);
    CHECK(counts->created == 0);
    for (unsigned i = 0; i < 100; ++i) {
        gige_mel::ProviderSettings settings;
        settings.instance = "not-a-backend";
        settings.device_id = "must-be-overridden";
        settings.maximum_registered_buffers = 7;
        auto session = take(s::select_session(catalog, {"probe", " Exact-ID ", profile, settings}));
        CHECK(session.backend_id == "probe" && session.device_id == " Exact-ID " && session.profile_id == profile);
        CHECK(session.settings.instance == "not-a-backend");
        CHECK(session.settings.device_id == session.device_id);
        CHECK(session.settings.maximum_registered_buffers == 7);
        auto manager = gige_mel::make_api_manager(session.settings, session.backend);
        CHECK(manager);
        CHECK(counts->opened == i && counts->enumerated == 0);
        rejected(session.backend->open("Exact-ID"), b::ErrorCode::invalid_argument);
        CHECK(counts->opened == i);
        auto opened = session.backend->open(session.device_id);
        rejected(opened, b::ErrorCode::disconnected);
        CHECK(opened.error().message == "selected device is disconnected");
        CHECK(counts->last_id == " Exact-ID ");
    }
    CHECK(counts->created == 100 && counts->destroyed == 100);
    CHECK(fallback == 0 && counts->enumerated == 0);
}

void independent_sessions() {
    s::BackendCatalog catalog;
    CHECK(catalog.register_backend("synthetic", profile, [] { return b::make_synthetic_backend(); }));
    auto a = take(s::select_session(catalog, {"synthetic", "synthetic-0", profile, {}}));
    auto b = take(s::select_session(catalog, {"synthetic", "synthetic-0", profile, {}}));
    CHECK(a.backend != b.backend);
    auto da = take(a.backend->open(a.device_id));
    auto db = take(b.backend->open(b.device_id));
    auto config = take(da->configuration());
    config.width = 32;
    CHECK(da->apply(config));
    CHECK(take(db->configuration()).width == 64);
    CHECK(da->close());
    CHECK(db->configuration());
    CHECK(db->close());
    auto bad = take(s::select_session(catalog, {"synthetic", "synthetic", profile, {}}));
    rejected(bad.backend->open(bad.device_id), b::ErrorCode::not_found);
}

void independent_profile_registration() {
    s::BackendCatalog catalog;
    using Profile = s::BackendCatalog::Profile;
    using Profiles = std::vector<std::string>;
    auto counts = std::make_shared<alternate_profile::Counts>();
    const std::string alternate{alternate_profile::id};
    auto factory = [counts] { return alternate_profile::make(counts); };
    rejected(catalog.register_profile("", factory), b::ErrorCode::invalid_argument);
    rejected(catalog.register_profile(std::string{"id\0suffix", 9}, factory), b::ErrorCode::invalid_argument);
    rejected(catalog.register_profile(alternate, {}), b::ErrorCode::invalid_argument);
    CHECK(catalog.register_profile(alternate, factory));
    rejected(catalog.register_profile(alternate, factory), b::ErrorCode::invalid_argument);
    rejected(catalog.register_profile(profile, factory), b::ErrorCode::invalid_argument);
    unsigned constructed = 0;
    std::vector<Profile> injected;
    auto constructor = [&](Profile p) -> s::BackendCatalog::BackendResult {
        ++constructed;
        injected.push_back(std::move(p));
        return b::make_synthetic_backend(); // probe of injection only, not alternate synthetic support
    };
    rejected(catalog.register_backend("synthetic", Profiles{}, constructor), b::ErrorCode::invalid_argument);
    rejected(catalog.register_backend("synthetic", Profiles{alternate, alternate}, constructor), b::ErrorCode::invalid_argument);
    rejected(catalog.register_backend("synthetic", Profiles{"missing"}, constructor), b::ErrorCode::unsupported);
    CHECK(catalog.register_backend("synthetic", Profiles{profile, alternate}, constructor));
    CHECK(counts->created == 0 && constructed == 0);
    for (const auto& wrong : {"test_rate_limited_v1 ", "Test_rate_limited_v1", "missing", ""})
        rejected(catalog.create("synthetic", wrong), b::ErrorCode::unsupported);
    CHECK(counts->created == 0 && constructed == 0);
    CHECK(catalog.create("synthetic", alternate));
    CHECK(catalog.create("synthetic", alternate));
    CHECK(injected[0] != injected[1] && counts->created == 2 && counts->opened == 0);
    CHECK(catalog.create("synthetic", profile));
    CHECK(injected.back()->id() == profile);
    injected.clear();
    CHECK(counts->destroyed == 2);

    CHECK(catalog.register_profile("null-profile", [] { return Profile{}; }));
    CHECK(catalog.register_profile("wrong-profile", [] { return b::profiles::make_mono_free_running_profile(); }));
    CHECK(catalog.register_profile("throw-profile", []() -> b::Result<Profile> { throw std::runtime_error("profile failure"); }));
    CHECK(catalog.register_profile("bad-alloc-profile", []() -> b::Result<Profile> { throw std::bad_alloc{}; }));
    CHECK(catalog.register_profile("unknown-profile", []() -> b::Result<Profile> { throw 42; }));
    CHECK(catalog.register_profile("error-profile", []() -> b::Result<Profile> {
        return b::Error{b::ErrorCode::unsupported, "profile creation rejected"};
    }));
    for (auto id : {"null-profile", "wrong-profile", "throw-profile", "bad-alloc-profile", "unknown-profile", "error-profile"}) {
        const std::string backend_id{id};
        CHECK(catalog.register_backend(backend_id, Profiles{id}, constructor));
        auto failed = catalog.create(id, id);
        CHECK(!failed);
        if (backend_id == "null-profile" || backend_id == "wrong-profile")
            CHECK(failed.error().code == b::ErrorCode::invalid_argument);
        else if (backend_id == "bad-alloc-profile") CHECK(failed.error().code == b::ErrorCode::resource_exhausted);
        else if (backend_id == "error-profile") CHECK(failed.error().message == "profile creation rejected");
        else CHECK(failed.error().code == b::ErrorCode::io_error);
    }
    CHECK(constructed == 3); // invalid profiles never reach a backend constructor
}
}

int main() {
    try {
        validation(); mel_lifecycle(); binding_and_no_fallback(); independent_sessions(); independent_profile_registration();
        std::cout << "SDK-free selection contracts passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
