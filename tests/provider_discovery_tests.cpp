#include <gige_mel/provider.hpp>
#include "../src/provider_test_seams.hpp"
#include "support/recording_stack_backend.hpp"
#include "support/recording_task_backend.hpp"
#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/scheduling/SchedulingChannel.h>
#include <irmel/library/stacked-image/StackedImageChannel.h>
#include <irmel/library/stacked-image/StackedImageListener.h>
#include <iostream>
#include <new>

namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
namespace b = gige_mel::backend;
namespace seams = gige_mel::test_seams;
#define CHECK(e) do { if (!(e)) throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) + ": " #e); } while (false)

namespace {
class CountingBackend final : public b::ICameraBackend {
    std::shared_ptr<b::ICameraBackend> backend_;
public:
    explicit CountingBackend(std::shared_ptr<b::ICameraBackend> backend) : backend_(std::move(backend)) {}
    std::string_view id() const noexcept override { return backend_->id(); }
    b::Result<std::vector<b::DeviceInfo>> enumerate() override { return backend_->enumerate(); }
    b::Result<std::shared_ptr<b::ICameraDevice>> open(std::string_view id) override {
        ++opens;
        return backend_->open(id); // Do not wrap the device or hide its optional interfaces.
    }
    unsigned opens{0};
};

struct DiscoveryFailure {
    seams::DiscoveryPoint fail_at;
    unsigned hits{0};
};

class ScopedDiscoveryHook {
public:
    explicit ScopedDiscoveryHook(DiscoveryFailure& failure) {
        seams::set_discovery_hook([](seams::DiscoveryPoint point, void* context) {
            auto& state = *static_cast<DiscoveryFailure*>(context);
            if (point == state.fail_at) {
                ++state.hits;
                throw std::bad_alloc{};
            }
        }, &failure);
    }
    ~ScopedDiscoveryHook() { seams::set_discovery_hook(nullptr, nullptr); }
    ScopedDiscoveryHook(const ScopedDiscoveryHook&) = delete;
    ScopedDiscoveryHook& operator=(const ScopedDiscoveryHook&) = delete;
};

struct ImageListener final : ir::ImageListener {
    void onImage(const ir::Channel&, const ir::FrameHeader&, std::shared_ptr<ir::Buffer>) override {}
};

struct StackListener final : ir::StackedImageListener {
    void onImage(const ir::Channel&, const ir::FrameHeader&, std::shared_ptr<ir::Buffer>) override {}
    ir::Return onImageStack(ir::Channel&, const ir::StackFrameHeader&, std::shared_ptr<ir::Buffer>) override {
        return ir::Return::Success;
    }
};

struct ExpectedChannel {
    ir::ChannelType type;
    std::uint8_t uuid_suffix;
    std::string_view label;
};

void check_snapshot(const std::vector<ir::ChannelCapability>& snapshot, bool optional) {
    constexpr ExpectedChannel expected[] = {
        {ir::ChannelType::IRSTImage, 1, "synthetic/image"},
        {ir::ChannelType::CommandAndControl, 2, "synthetic/c2"},
        {ir::ChannelType::HealthAndStatus, 3, "synthetic/health"},
        {ir::ChannelType::Scheduling, 5, "synthetic/scheduling"},
        {ir::ChannelType::StackedImage, 4, "synthetic/stacked-image"},
    };
    CHECK(snapshot.size() == (optional ? 5U : 3U));
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        std::array<std::uint8_t, mel::UUID_SIZE> uuid{};
        uuid.back() = expected[i].uuid_suffix;
        CHECK(snapshot[i].getChannelTypes() == std::vector{expected[i].type});
        CHECK(snapshot[i].getChanID().getUUID() == uuid);
        CHECK(snapshot[i].getChanID().getDescriptiveLabel() == expected[i].label);
    }
}

void check_attachment(ir::Control& control, const ir::ChannelCapability& cap) {
    ir::Config config;
    const auto type = cap.getChannelTypes().front();
    config.setChannelType(type);
    config.setChanID(cap.getChanID());
    if (type == ir::ChannelType::IRSTImage) config.setImgLstnr(std::make_shared<ImageListener>());
    if (type == ir::ChannelType::StackedImage) config.setImgLstnr(std::make_shared<StackListener>());
    const auto attached = control.attachChannel(config);
    CHECK(attached);
    switch (type) {
        case ir::ChannelType::IRSTImage: CHECK(dynamic_cast<ir::ImageChannel*>(attached.get())); break;
        case ir::ChannelType::CommandAndControl: CHECK(dynamic_cast<ir::C2Channel*>(attached.get())); break;
        case ir::ChannelType::HealthAndStatus: CHECK(dynamic_cast<ir::HealthStatusChannel*>(attached.get())); break;
        case ir::ChannelType::Scheduling: CHECK(dynamic_cast<ir::SchedulingChannel*>(attached.get())); break;
        case ir::ChannelType::StackedImage: CHECK(dynamic_cast<ir::StackedImageChannel*>(attached.get())); break;
        default: throw std::runtime_error("unexpected discovery type");
    }
    CHECK(control.detachChannel(attached) == ir::Return::Success);
}

void discovery_publication(bool optional) {
    auto backend = b::make_synthetic_backend();
    if (optional) {
        // The recording Task device forwards the wrapped Stack interface.
        backend = std::make_shared<recording_task::Backend>(std::make_shared<recording_task::Recording>(),
            std::make_shared<recording_stack::Backend>(std::make_shared<recording_stack::Recording>()));
    }
    const auto counting = std::make_shared<CountingBackend>(std::move(backend));
    const auto manager = gige_mel::make_api_manager({}, counting);
    const auto control = ir::getControl("synthetic", manager);
    CHECK(control && control->getCapabilities().empty());
    CHECK(counting->opens == 0);

    DiscoveryFailure failure{optional ? seams::DiscoveryPoint::before_publication : seams::DiscoveryPoint::after_first_entry};
    {
        ScopedDiscoveryHook inject(failure);
        CHECK(control->init() == ir::Return::Fail);
    }
    CHECK(failure.hits == 1);
    CHECK(counting->opens == 1);
    if (!control->getCapabilities().empty()) {
        throw std::runtime_error("discovery visible after failed init: " +
            std::to_string(control->getCapabilities().size()) + " entries");
    }

    CHECK(control->init() == ir::Return::Success);
    CHECK(counting->opens == 1);
    const auto& snapshot = control->getCapabilities();
    check_snapshot(snapshot, optional);
    for (const auto& cap : snapshot) check_attachment(*control, cap);

    const auto* entries = snapshot.data();
    CHECK(control->init("mono8-bootstrap") == ir::Return::Success);
    CHECK(counting->opens == 1);
    CHECK(control->getCapabilities().data() == entries);
    check_snapshot(control->getCapabilities(), optional);
}
} // namespace

int main() {
    int result = 0;
    for (bool optional : {false, true}) {
        const auto* name = optional ? "late failure (five channels)" : "early failure (three channels)";
        try {
            discovery_publication(optional);
            std::cout << "PASS " << name << ": discovery recovered with one backend device open\n";
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            result = 1;
        }
    }
    return result;
}