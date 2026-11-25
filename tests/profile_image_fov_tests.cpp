#include <gige_mel/provider.hpp>
#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/ImageListener.h>
#include "profiles/navigation_handler.hpp"
#include "selection/profile_loader.hpp"
#include "support/module_unload_probe.hpp"
#include "support/profile_fov_backend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <limits>

namespace ir = ams::iface::irmel;
namespace b = gige_mel::backend;
namespace p = b::profiles;
namespace s = gige_mel::selection;
namespace fx = profile_fov_fixture;
using namespace std::chrono_literals;
#define CHECK(e) do { if (!(e)) throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) + ": " #e); } while(false)

namespace {
constexpr char optics_module[] = GIGE_MEL_TEST_PROFILE_DIR "/libvirtual_optics_profile.so";
constexpr b::CameraConfiguration centered{80, 60, 152, 98, 40, 1250us};
constexpr b::CameraConfiguration resized{100, 72, 110, 54, 35, 1500us};
// Reference values calculated separately from the module with the angle
// between the two edge vectors: atan2(|cross(a,b)|, dot(a,b)).
constexpr b::ImageFieldOfView edge_fov{0.2736332383214969, 0.3309629787920281};
constexpr b::ImageFieldOfView centered_fov{0.5565993180102227, 0.6117577428097043};
constexpr b::ImageFieldOfView resized_fov{0.6577189880160664, 0.6407219787043014};
constexpr b::ImageFieldOfView adjusted_fov{0.6768650307004183, 0.6407219787043014};

template<class T> T take(b::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

void check_angles(b::ImageFieldOfView actual, b::ImageFieldOfView expected) {
    if (!(std::abs(actual.horizontal_radians - expected.horizontal_radians) < 1e-12 &&
          std::abs(actual.vertical_radians - expected.vertical_radians) < 1e-12))
        throw std::runtime_error("Image FOV mismatch: got (" + std::to_string(actual.horizontal_radians) + ", " +
            std::to_string(actual.vertical_radians) + "), expected (" + std::to_string(expected.horizontal_radians) +
            ", " + std::to_string(expected.vertical_radians) + ") radians");
}

void module_and_bridge() {
    ModuleUnloadProbe probe(true);
    unsigned unloads = 0;
    CHECK(!p::retain_image_field_of_view({}));
    auto features = fx::features();
    CHECK(!p::retain_image_field_of_view(take(p::make_mono_free_running_profile()->open(features))));
    {
        auto cognex_features = fx::rf::cognex_features();
        auto loaded = take(s::load_profile_module(GIGE_MEL_COGNEX_MODULE));
        CHECK(!p::retain_image_field_of_view(take(loaded.profile->open(cognex_features))));
    }

    struct Case { const char* path; bool navigation, fov; };
    for (const auto& c : {Case{GIGE_MEL_TEST_PROFILE_DIR "/librate_limited_profile.so", false, false},
                          Case{GIGE_MEL_TEST_PROFILE_DIR "/libnavigation_record_profile.so", true, false},
                          Case{optics_module, false, true},
                          Case{GIGE_MEL_TEST_PROFILE_DIR "/libvirtual_optics_navigation_profile.so", true, true}}) {
        std::shared_ptr<const p::ICameraProfileState> state;
        {
            auto loaded = take(s::load_profile_module(c.path));
            auto selected = take(loaded.create());
            auto native = fx::features();
            state = take(selected->open(native));
            CHECK(native.writes().empty());
        } // Feature service, profiles and factory all retire independently.
        probe.expect(unloads);
        auto fov = p::retain_image_field_of_view(state);
        const auto* nav = p::navigation_handler(*state);
        CHECK((nav != nullptr) == c.navigation);
        CHECK(static_cast<bool>(fov) == c.fov);
        if (nav) {
            b::NavigationReport report{};
            report.state = b::NavigationSolutionState::gps;
            auto operation = take(nav->prepare_navigation(report));
            CHECK(operation.format == "test.fictional-navigation-record.v1" && !operation.payload.empty());
            CHECK(operation.report.state == report.state);
        }
        state.reset(); // The FOV adapter, if any, is now the only relevant owner.
        if (fov) {
            probe.expect(unloads);
            const auto saved = take(fov->image_field_of_view(fx::initial));
            check_angles(take(fov->image_field_of_view(centered)), centered_fov);
            auto bad = centered;
            bad.offset_x = std::numeric_limits<std::uint32_t>::max();
            CHECK(!fov->image_field_of_view(bad));
            bad = centered;
            bad.height = 0;
            CHECK(!fov->image_field_of_view(bad));
            fov.reset();
            probe.expect(unloads + 1);
            check_angles(saved, edge_fov); // Module has actually finalized.
        }
        probe.expect(++unloads);
    }
}

struct Delivered {
    ir::FrameHeader header;
    std::shared_ptr<ir::Buffer> held;
    std::vector<std::byte> bytes;
};

class Listener final : public ir::ImageListener {
public:
    // Arm while disabled only. Hold just the first frame of this run.
    bool hold_next{};
    void onImage(const ir::Channel&, const ir::FrameHeader& header, std::shared_ptr<ir::Buffer> buffer) override {
        const auto* pixels = static_cast<const std::byte*>(buffer->getImageAddress());
        Delivered frame{header, hold_next ? buffer : nullptr,
            {pixels, pixels + static_cast<std::size_t>(header.getWidth()) * header.getHeight()}};
        hold_next = false;
        if (!frame.held) CHECK(buffer->release() == ir::Return::Success);
        { std::lock_guard lock(mutex_); frames_.push_back(std::move(frame)); }
        changed_.notify_all();
    }
    void wait(std::size_t count) {
        std::unique_lock lock(mutex_);
        CHECK(changed_.wait_for(lock, 3s, [&] { return frames_.size() >= count; }));
    }
    std::vector<Delivered> frames() const { std::lock_guard lock(mutex_); return frames_; }
private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Delivered> frames_;
};

constexpr std::byte untouched{0xd1};
struct Session {
    std::array<std::vector<std::byte>, 2> storage; // outlives control/worker and buffers
    std::shared_ptr<fx::Observation> observed = std::make_shared<fx::Observation>();
    std::shared_ptr<Listener> listener = std::make_shared<Listener>();
    gige_mel::ProviderSettings settings;
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::shared_ptr<ir::ImageChannel> image;
    std::shared_ptr<ir::C2Channel> c2;
    std::vector<std::shared_ptr<ir::Buffer>> buffers;

    explicit Session(std::shared_ptr<const p::ICameraProfile> profile, double horizontal_pitch = 0.002) {
        settings.device_id = fx::device_id;
        settings.timestamp_domain = b::ClockDomain::device;
        settings.horizontal_radians_per_pixel = horizontal_pitch;
        settings.vertical_radians_per_pixel = 0.003;
        manager = gige_mel::make_api_manager(settings, std::make_shared<fx::Backend>(std::move(profile), observed));
        CHECK(manager);
        control = ir::getControl(settings.instance, manager);
        CHECK(control && control->init() == ir::Return::Success);
        image = std::dynamic_pointer_cast<ir::ImageChannel>(control->attachChannel(config(ir::ChannelType::IRSTImage)));
        c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(config(ir::ChannelType::CommandAndControl)));
        CHECK(image && c2);
    }
    ir::Config config(ir::ChannelType type) const {
        for (const auto& cap : control->getCapabilities()) if (cap.getChannelTypes() == std::vector{type}) {
            ir::Config config;
            config.setChanID(cap.getChanID());
            config.setChannelType(type);
            if (type == ir::ChannelType::IRSTImage) config.setImgLstnr(listener);
            return config;
        }
        throw std::runtime_error("missing channel discovery");
    }
    void register_buffers() {
        for (auto& bytes : storage) {
            bytes.assign(100 * 72, untouched);
            auto buffer = ir::getBuffer(settings.instance, manager);
            CHECK(buffer && buffer->init(bytes.data(), bytes.size(), 0) == ir::Return::Success);
            CHECK(image->registerBuffer(buffer) == ir::Return::Success);
            buffers.push_back(std::move(buffer));
        }
    }
    void configure(const b::CameraConfiguration& actual, bool accepted = true) {
        ir::CameraCommand cmd;
        cmd.setWidth(actual.width); cmd.setHeight(actual.height);
        cmd.setColumnOffset(actual.offset_x); cmd.setRowOffset(actual.offset_y);
        cmd.setFrameRate(actual.frame_rate_hz); cmd.setIntegrationTime(actual.exposure);
        cmd.setImageType(ir::ImageType::Staring); cmd.setNumberOfStages(0);
        auto response = c2->send(cmd).get();
        CHECK(response && response.get() && response.get()->getCommandSuccess() == accepted);
    }
    void count_native(unsigned acquisitions) const {
        CHECK(observed->stream_creation_attempts == acquisitions);
        CHECK(observed->native->stream_start_attempts == acquisitions);
        CHECK(observed->native->pools_created == acquisitions);
    }
};

void check_frame(const Delivered& frame, const b::CameraConfiguration& actual, b::ImageFieldOfView expected) {
    const auto& h = frame.header;
    check_angles({h.getHorizontalFieldOfView(), h.getVerticalFieldOfView()}, expected);
    CHECK(h.getWidth() == actual.width && h.getHeight() == actual.height);
    CHECK(h.getColumnOffset() == actual.offset_x && h.getRowOffset() == actual.offset_y);
    CHECK(h.getIntegrationTime() == actual.exposure);
    CHECK(frame.bytes.size() == actual.width * actual.height);
    CHECK(std::to_integer<unsigned>(frame.bytes.front()) == contract_fixture::pixel(actual.offset_x, actual.offset_y, h.getFrameID()));
}

void image_delivery() {
    {
        Session f(p::make_mono_free_running_profile());
        f.register_buffers();
        CHECK(f.image->enable() == ir::Return::Success);
        f.listener->wait(3);
        CHECK(f.image->disable() == ir::Return::Success);
        for (const auto& frame : f.listener->frames()) check_frame(frame, fx::initial, {0.16, 0.18});
        f.count_native(1);
        CHECK(f.observed->fov_requests().empty());
    }
    Session f(take(s::load_profile_module(optics_module)).profile); // same settings as default case
    f.register_buffers();
    f.listener->hold_next = true;
    CHECK(f.image->enable() == ir::Return::Success);
    f.listener->wait(2);
    CHECK(f.image->enable() == ir::Return::Success); // repeat while running
    f.listener->wait(4);
    CHECK(f.image->disable() == ir::Return::Success);
    const auto first_run = f.listener->frames();
    const auto saved = first_run.front();
    for (const auto& frame : first_run) check_frame(frame, fx::initial, edge_fov);
    f.count_native(1);
    CHECK(f.observed->fov_requests() == std::vector{fx::initial});
    CHECK(saved.held);
    f.configure(centered); // Equal size, different offsets on both axes.
    CHECK(f.image->enable() == ir::Return::Fail); // held registered buffer: no new resolution
    f.count_native(1);
    CHECK(f.observed->fov_requests() == std::vector{fx::initial});
    CHECK(std::equal(saved.bytes.begin(), saved.bytes.end(), static_cast<const std::byte*>(saved.held->getImageAddress())));
    CHECK(saved.held->release() == ir::Return::Success);

    const auto acquire = [&](const auto& configuration, const auto& expected, unsigned acquisitions) {
        const auto first = f.listener->frames().size();
        CHECK(f.image->enable() == ir::Return::Success);
        f.listener->wait(first + 3);
        CHECK(f.image->disable() == ir::Return::Success);
        const auto frames = f.listener->frames();
        for (auto i = first; i < frames.size(); ++i) check_frame(frames[i], configuration, expected);
        f.count_native(acquisitions);
    };
    acquire(centered, centered_fov, 2);
    f.configure(resized);
    acquire(resized, resized_fov, 3);

    // Force a valid but different native readback on an unsuccessful request.
    // Neither the original requested X nor the last successful X is correct.
    f.observed->override_written_x = 160;
    auto request = resized;
    request.offset_x = 140;
    f.configure(request, false);
    auto adjusted = resized;
    adjusted.offset_x = 160;
    acquire(adjusted, adjusted_fov, 4);
    CHECK((f.observed->fov_requests() == std::vector{fx::initial, centered, resized, adjusted}));
    check_frame(saved, fx::initial, edge_fov);
    check_frame(f.listener->frames().front(), fx::initial, edge_fov);
}

void require_clean_failure(Session& f, std::string_view diagnostic) {
    CHECK(f.image->enable() == ir::Return::Fail);
    f.count_native(0);
    const auto stats = gige_mel::provider_statistics(f.manager);
    CHECK(stats.last_error.find(diagnostic) != std::string::npos);
    CHECK(stats.delivered_frames == 0 && f.listener->frames().empty());
    for (const auto& buffer : f.buffers) CHECK(buffer->release() == ir::Return::Fail);
    for (const auto& bytes : f.storage) CHECK(std::ranges::all_of(bytes, [](auto v) { return v == untouched; }));
}

void enable_failures() {
    Session f(take(s::load_profile_module(optics_module)).profile);
    CHECK(f.image->enable() == ir::Return::Fail); // buffer admission precedes FOV
    CHECK(f.observed->fov_requests().empty());
    f.register_buffers();
    f.observed->failure = [] { return b::Error{b::ErrorCode::unsupported, "test model unavailable"}; };
    require_clean_failure(f, "Image FOV source: test model unavailable");
    f.observed->failure = []() -> b::Result<b::ImageFieldOfView> { throw std::runtime_error("test model exception"); };
    require_clean_failure(f, "Image FOV source exception: test model exception");
    f.observed->failure = []() -> b::Result<b::ImageFieldOfView> { throw 7; };
    require_clean_failure(f, "Image FOV source threw an unknown exception");
    for (double bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 0.0, -1.0}) {
        f.observed->failure = [bad] { return b::ImageFieldOfView{bad, 1.0}; };
        require_clean_failure(f, "Image FOV source: horizontal angle must be finite and positive");
        f.observed->failure = [bad] { return b::ImageFieldOfView{1.0, bad}; };
        require_clean_failure(f, "Image FOV source: vertical angle must be finite and positive");
    }
    CHECK(f.observed->fov_requests() == std::vector(11, fx::initial));
    f.observed->failure = {};
    f.configure(centered); // no partially published acquisition prevents this
    CHECK(f.image->enable() == ir::Return::Success);
    f.listener->wait(2);
    CHECK(f.image->disable() == ir::Return::Success);
    for (const auto& frame : f.listener->frames()) check_frame(frame, centered, centered_fov);
    f.count_native(1);

    Session overflow(p::make_mono_free_running_profile(), std::numeric_limits<double>::max());
    overflow.register_buffers();
    require_clean_failure(overflow, "Image FOV session pitch: horizontal angle must be finite and positive");
    CHECK(overflow.observed->fov_requests().empty());
}
} // namespace

int main(int argc, char** argv) {
    try {
        CHECK(argc == 2);
        const std::string_view scenario = argv[1];
        if (scenario == "module_and_bridge") module_and_bridge();
        else if (scenario == "image_delivery") image_delivery();
        else if (scenario == "enable_failures") enable_failures();
        else CHECK(false);
        std::cout << scenario << " passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}