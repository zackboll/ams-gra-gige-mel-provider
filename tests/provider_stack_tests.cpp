#include <gige_mel/provider.hpp>
#include "support/recording_stack_backend.hpp"
#include "join_observer.hpp"
#if defined(GIGE_MEL_PROVIDER_TEST_SEAMS)
#include "../src/provider_test_seams.hpp"
#endif
#include <irmel/library/stacked-image/StackedImageChannel.h>
#include <irmel/library/stacked-image/StackedImageListener.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <future>
#include <iostream>

namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
namespace b = gige_mel::backend;
namespace r = recording_stack;
using namespace std::chrono_literals;
#define CHECK(e) do { if (!(e)) throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) + ": " #e); } while(false)
namespace {
struct Listener final : ir::StackedImageListener {
    std::mutex mutex;
    unsigned count{0};
    ir::StackFrameHeader header;
    ir::Return result{ir::Return::Success};
    bool throws{false}, release{false};
    std::function<void(ir::Channel&)> hook;
    ir::Return onImageStack(ir::Channel& channel, const ir::StackFrameHeader& h, std::shared_ptr<ir::Buffer> buffer) override {
        CHECK(dynamic_cast<ir::StackedImageChannel*>(&channel));
        { std::lock_guard lock(mutex); ++count; header = h; }
        if (hook) hook(channel);
        if (release) CHECK(buffer->release() == ir::Return::Success);
        if (throws) throw std::runtime_error("fixture callback");
        return result;
    }
    unsigned calls() { std::lock_guard lock(mutex); return count; }
};
struct Plain final : ir::ImageListener {
    void onImage(const ir::Channel&, const ir::FrameHeader&, std::shared_ptr<ir::Buffer>) override {}
};
struct Fixture {
    std::array<std::byte, 3072> bytes{};
    std::shared_ptr<r::Recording> recording;
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::shared_ptr<Listener> listener{std::make_shared<Listener>()};
    std::shared_ptr<ir::StackedImageChannel> stack;
    std::shared_ptr<ir::Buffer> buffer;
    explicit Fixture(std::shared_ptr<r::Recording> rec = std::make_shared<r::Recording>()) : recording(std::move(rec)) {
        manager = gige_mel::make_api_manager({}, recording ? std::shared_ptr<b::ICameraBackend>{std::make_shared<r::Backend>(recording)} : b::make_synthetic_backend());
        control = ir::getControl("synthetic", manager);
        CHECK(control && control->init() == ir::Return::Success);
    }
    ~Fixture() { if (control && stack) (void)control->detachChannel(stack); }
    ir::Config config(ir::ChannelType type) {
        for (const auto& c : control->getCapabilities()) if (c.getChannelTypes() == std::vector{type}) {
            ir::Config v; v.setChanID(c.getChanID()); v.setChannelType(type); v.setImgLstnr(listener); return v;
        }
        throw std::runtime_error("missing capability");
    }
    void attach() {
        stack = std::dynamic_pointer_cast<ir::StackedImageChannel>(control->attachChannel(config(ir::ChannelType::StackedImage)));
        CHECK(stack); buffer = ir::getBuffer("synthetic", manager);
        CHECK(buffer->init(bytes.data(), bytes.size(), 44) == ir::Return::Success);
        CHECK(stack->registerBuffer(buffer) == ir::Return::Success);
    }
    std::shared_ptr<r::Subscription> start() {
        CHECK(stack->enable() == ir::Return::Success); auto s = recording->latest; CHECK(s); s->wait(1); return s;
    }
    void exact(std::uint32_t id = 701) {
        // Independent oracle: no source conversion/helper is used here.
        const auto& h = listener->header;
        CHECK(h.getplatformId() == 1234 && h.getTxaFOM() == 3);
        CHECK(h.getStackDepth() == 2 && h.getWidth() == 4 && h.getHeight() == 3);
        CHECK(h.getBitDepth() == 8 && h.getPixelDepth() == 8 && h.getPackedPixelsSize() == 24);
        CHECK(h.getStackId() == id && h.getDeadPixelMask() == 0);
        std::array<std::uint8_t,16> cap{}, act{}; cap.back() = 41; act.back() = 42;
        CHECK(h.getCapabilityId() == mel::UCI_ID(cap, "test-volume"));
        CHECK(h.getActivityId() == mel::UCI_ID(act, "test-recording"));
        for (unsigned i = 0; i < 24; ++i) CHECK(bytes[i] == static_cast<std::byte>(i < 12 ? i + 1 : i + 89));
        for (unsigned i = 24; i < bytes.size(); ++i) CHECK(bytes[i] == std::byte{});
    }
};
void discovery() {
    Fixture absent(nullptr);
    CHECK(absent.control->getCapabilities().size() == 3);
    for (unsigned invalid = 0; invalid < 15; ++invalid) {
        auto rec = std::make_shared<r::Recording>(); auto& s = rec->declarations;
        switch (invalid) {
        case 0:s.width = 65536;break; case 1:s.height = 0;break; case 2:s.depth = 1;break;
        case 3:s.depth = 65536;break; case 4:s.width = 65535;s.height = 65535;s.depth = 65535;break;
        case 5:s.logical_bits = 12;break; case 6:s.storage_bits = 256;break;
        case 7:s.layout = b::StackLayout::unknown;break; case 8:s.packed_bytes = 23;break;
        case 9:s.source_generation = 0;break; case 10:s.formation_meaning.clear();break;
        case 11:s.identity_scheme.clear();break; case 12:s.txa_fom = 11;break;
        case 13:s.platform_id = 65536;break; case 14:s.mask_information_absent = false;break;
        }
        Fixture f(rec); CHECK(f.control->getCapabilities().size() == 3); CHECK(rec->opens == 0);
    }
    Fixture f;
    CHECK(f.control->getCapabilities().size() == 4);
    auto cfg = f.config(ir::ChannelType::StackedImage);
    cfg.setImgLstnr(std::make_shared<Plain>()); CHECK(!f.control->attachChannel(cfg));
    cfg.setImgLstnr(nullptr); CHECK(!f.control->attachChannel(cfg));
    cfg.setImgLstnr(f.listener); cfg.setChannelType(ir::ChannelType::IRSTImage); CHECK(!f.control->attachChannel(cfg));
    cfg = f.config(ir::ChannelType::StackedImage); auto id = cfg.getChanID(); id.setUUID({}); cfg.setChanID(id); CHECK(!f.control->attachChannel(cfg));
    cfg = f.config(ir::ChannelType::StackedImage); id = cfg.getChanID(); id.setDescriptiveLabel("foreign"); cfg.setChanID(id); CHECK(!f.control->attachChannel(cfg));
    f.attach(); CHECK(!f.control->attachChannel(f.config(ir::ChannelType::StackedImage)));
    CHECK(f.recording->opens == 0);
    const auto c = f.stack->getCapabilities();
    CHECK(c.getWidth() == 4 && c.getHeight() == 3 && c.getBitDepth() == 8);
    CHECK(c.getRowPitch() == 4 && c.getBufferSize() == 24 && c.getImageSize() == 24);
    CHECK(c.getNumberOfBands() == 1 && c.getFormat() == ir::PixelFormat::Mono);
    CHECK(c.getChannelTypes() == std::vector{ir::ChannelType::StackedImage});
    CHECK(c.getChannelMetadataCapabilities() == std::set{ir::ChannelMetadataCapabilityType::ChannelCommsTestRep});
    for (const auto& cap : f.control->getCapabilities()) if (cap.getChannelTypes() == c.getChannelTypes()) {
        CHECK(cap.getBufferSize() == c.getBufferSize()); CHECK(cap.getWidth() == c.getWidth());
    }
    CHECK(f.stack->registerMetadataCallback(std::function<void(ir::Channel&, const ir::LOS3D_KinematicsType*)>{}) == ir::Return::NotSupported);
    unsigned callbacks = 0;
    CHECK(f.stack->registerMetadataCallback(std::function<void(ir::Channel&, const ir::ChannelCommsTestRep*)>{[&](auto&, auto* rep) {
        CHECK(rep->getCommandID() == 12 && rep->getRequestID() == 34); ++callbacks;
    }}) == ir::Return::Success);
    CHECK(f.stack->sendKeepAliveRep().get());
    ir::ChannelCommsTestReq request; request.setCommandID(12); request.setRequestID(34);
    CHECK(f.stack->send(request).get()); CHECK(callbacks == 1);
    Fixture foreign; CHECK(foreign.control->detachChannel(f.stack) == ir::Return::BadPointer);
    CHECK(f.control->detachChannel(f.stack) == ir::Return::Success);
    CHECK(f.control->detachChannel(f.stack) == ir::Return::BadPointer);
    CHECK(f.stack->enable() == ir::Return::Fail);
    f.stack = std::dynamic_pointer_cast<ir::StackedImageChannel>(f.control->attachChannel(f.config(ir::ChannelType::StackedImage)));
    CHECK(f.stack && f.stack->enable() == ir::Return::Fail); // no registered buffers
}
void payload_ownership() {
    Fixture f; f.attach();
    auto tiny = ir::getBuffer("synthetic", f.manager); CHECK(tiny->init(f.bytes.data(), 23, 0) == ir::Return::Success);
    CHECK(f.stack->registerBuffer(tiny) != ir::Return::Success);
    auto image = std::dynamic_pointer_cast<ir::ImageChannel>(f.control->attachChannel(f.config(ir::ChannelType::IRSTImage)));
    CHECK(image); CHECK(image->registerBuffer(f.buffer) != ir::Return::Success);
    auto alias = ir::getBuffer("synthetic", f.manager); CHECK(alias->init(f.bytes.data(),f.bytes.size(),0) == ir::Return::Success);
    CHECK(image->registerBuffer(alias) != ir::Return::Success);
    auto s = f.start(); CHECK(image->enable() == ir::Return::Fail);
    s->wait(s->deliver()); CHECK(f.listener->calls() == 1); f.exact();
    s->wait(s->deliver(r::support(),702)); CHECK(f.listener->calls() == 1); f.exact();
    CHECK(gige_mel::provider_statistics(f.manager).dropped_no_buffer == 1);
    CHECK(f.buffer->release() == ir::Return::Success);
    s->wait(s->deliver(r::support(),703)); f.exact(703);
    CHECK(f.stack->disable() == ir::Return::Success); f.exact(703);
    CHECK(f.stack->unregisterBuffer(f.buffer) == ir::Return::Fail);
    CHECK(f.stack->enable() == ir::Return::Fail);
    CHECK(f.control->detachChannel(f.stack) == ir::Return::Success); f.exact(703);
    CHECK(image->registerBuffer(alias) != ir::Return::Success); // held detached memory still protected
    CHECK(f.buffer->release() == ir::Return::Success);
    CHECK(image->registerBuffer(alias) == ir::Return::Success);
    CHECK(image->unregisterBuffer(alias) == ir::Return::Success);
    CHECK(f.recording->image_streams == 0);
}
void independent_of_image_fov() {
    auto recording = std::make_shared<r::Recording>();
    recording->unusable_image_fov = true;
    Fixture f(recording);
    auto image = std::dynamic_pointer_cast<ir::ImageChannel>(f.control->attachChannel(f.config(ir::ChannelType::IRSTImage)));
    CHECK(image);
    auto buffer = ir::getBuffer("synthetic", f.manager);
    CHECK(buffer->init(f.bytes.data(), f.bytes.size(), 0) == ir::Return::Success);
    CHECK(image->registerBuffer(buffer) == ir::Return::Success);
    CHECK(image->enable() == ir::Return::Fail);
    CHECK(recording->image_fov_requests == 1 && recording->image_streams == 0);
    CHECK(image->unregisterBuffer(buffer) == ir::Return::Success);
    f.attach();
    auto stream = f.start();
    stream->wait(stream->deliver());
    CHECK(f.listener->calls() == 1);
    f.exact();
    CHECK(f.stack->disable() == ir::Return::Success);
    CHECK(f.buffer->release() == ir::Return::Success);
    CHECK(recording->image_fov_requests == 1 && recording->image_streams == 0);
}
void malformed() {
    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        Fixture f; f.attach(); auto stream = f.start(); auto s = r::support();
        switch(invalid) {
        case 0:s.width = 5;break;case 1:s.logical_bits = 7;break;case 2:++s.source_generation;break;
        case 3:s.depth = 3;break;case 4:s.layout = b::StackLayout::unknown;break;default:break;
        }
        stream->deliver(s,701,invalid == 5 ? std::optional<std::uint64_t>{999} : std::nullopt, invalid == 6 ? 23 : 24, invalid != 7);
        stream->wait_stop(); CHECK(f.listener->calls() == 0);
        CHECK(f.stack->enable() == ir::Return::Fail);
        const auto stats = gige_mel::provider_statistics(f.manager); CHECK(stats.rejected_frames == 1 && stats.stream_failures == 1);
        CHECK(f.stack->disable() == ir::Return::Success); CHECK(stream->destroyed == 1);
    }
}
void callback_results() {
    for (unsigned kind = 0; kind < 5; ++kind) {
        Fixture f; f.attach();
        f.listener->result = kind == 1 ? ir::Return::Fail : kind == 2 ? ir::Return::NotSupported : ir::Return::Success;
        f.listener->throws = kind == 3; f.listener->release = kind == 4;
        f.listener->hook = [&](ir::Channel& c) {
            CHECK(c.disable() == ir::Return::Fail); CHECK(c.enable() == ir::Return::Fail);
            CHECK(f.control->detachChannel(f.stack) == ir::Return::Fail);
        };
        auto s = f.start(); const auto next = s->deliver();
        if (kind >= 1 && kind <= 3) {
            s->wait_stop(); CHECK(f.stack->enable() == ir::Return::Fail);
            CHECK(gige_mel::provider_statistics(f.manager).callback_failures == 1);
        } else s->wait(next);
        CHECK(f.listener->calls() == 1); f.exact();
        CHECK(f.stack->disable() == ir::Return::Success);
        CHECK(f.buffer->release() == (kind == 4 ? ir::Return::Fail : ir::Return::Success));
        CHECK(f.stack->unregisterBuffer(f.buffer) == ir::Return::Success);
    }
}
mel::MFA_State health_state(Fixture& f) {
    auto health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(f.control->attachChannel(f.config(ir::ChannelType::HealthAndStatus)));
    CHECK(health);
    std::promise<mel::MFA_State> report;
    std::atomic<bool> first{true};
    CHECK(health->registerMetadataCallback(std::function<void(ir::Channel&, const mel::MFA_Status*)>{[&](auto&, auto* v) {
        if (first.exchange(false)) report.set_value(v->getMFAState());
    }}) == ir::Return::Success);
    CHECK(health->enable() == ir::Return::Success);
    auto reported = report.get_future(); CHECK(reported.wait_for(3s) == std::future_status::ready);
    const auto result = reported.get();
    CHECK(health->disable() == ir::Return::Success);
    CHECK(f.control->detachChannel(health) == ir::Return::Success);
    return result;
}
void lifecycle() {
    Fixture f; f.attach(); f.recording->fail_open = true;
    CHECK(f.stack->enable() == ir::Return::Fail); f.recording->fail_open = false;
    auto s = f.start(); s->wait(2); // bounded receive timeout, not an error
    CHECK(gige_mel::provider_statistics(f.manager).stream_failures == 0);
    { std::lock_guard lock(s->mutex); s->fail = true; s->changed.notify_all(); }
    s->wait_stop(); CHECK(f.stack->enable() == ir::Return::Fail);
    CHECK(health_state(f) == mel::MFA_State::Degraded);
    CHECK(f.stack->disable() == ir::Return::Success);
    CHECK(health_state(f) == mel::MFA_State::Standby);
    s = f.start(); s->wait(s->deliver());
    CHECK(health_state(f) == mel::MFA_State::Operate);
    auto c2 = std::dynamic_pointer_cast<ir::C2Channel>(f.control->attachChannel(f.config(ir::ChannelType::CommandAndControl)));
    CHECK(c2); ir::ModeCmd standby; standby.setState(mel::MFA_State::Standby); standby.setMode(ir::MFA_Mode::Unused);
    CHECK(c2->send(standby).get()); CHECK(s->destroyed == 1); f.exact();
    CHECK(health_state(f) == mel::MFA_State::Standby);
    CHECK(f.buffer->release() == ir::Return::Success);
    s = f.start(); CHECK(f.stack->disable() == ir::Return::Success);
    CHECK(s->destroyed == 1); CHECK(f.recording->image_streams == 0);
    auto image = std::dynamic_pointer_cast<ir::ImageChannel>(f.control->attachChannel(f.config(ir::ChannelType::IRSTImage)));
    std::array<std::byte,3072> image_bytes{};
    auto ib = ir::getBuffer("synthetic",f.manager); CHECK(ib->init(image_bytes.data(),image_bytes.size(),1) == ir::Return::Success);
    CHECK(image->registerBuffer(ib) == ir::Return::Success);
    CHECK(image->enable() == ir::Return::Success); CHECK(f.stack->enable() == ir::Return::Fail);
    CHECK(image->disable() == ir::Return::Success);
    CHECK(f.control->detachChannel(image) == ir::Return::Success);
    ++f.recording->declarations.source_generation; // immutable declarations cannot be replaced at restart
    const auto opens = f.recording->opens;
    CHECK(f.stack->enable() == ir::Return::Fail); CHECK(f.recording->opens == opens);
}
struct Gate {
    std::mutex mutex; std::condition_variable cv; bool entered{false}, opened{false}, expired{false};
    void hold() { std::unique_lock lock(mutex); entered = true; cv.notify_all(); if (!cv.wait_for(lock,3s,[&]{return opened;})) expired = true; }
    void wait() { std::unique_lock lock(mutex); CHECK(cv.wait_for(lock,3s,[&]{return entered;})); }
    void check() { std::lock_guard lock(mutex); CHECK(!expired); }
    void open() { std::lock_guard lock(mutex); opened = true; cv.notify_all(); }
};
#if defined(GIGE_MEL_PROVIDER_TEST_SEAMS)
void admission_ordering() {
    namespace seams = gige_mel::test_seams;
    for (auto point : {seams::StackPoint::before_admission, seams::StackPoint::after_reservation}) {
        for (bool throwing : {false,true}) {
            Fixture f; f.attach(); Gate gate;
            struct Context { seams::StackPoint point; Gate* gate; } context{point,&gate};
            seams::set_stack_hook([](seams::StackPoint current, void* opaque) noexcept {
                auto& c = *static_cast<Context*>(opaque);
                if (current == c.point) c.gate->hold();
            }, &context);
            f.listener->throws = throwing;
            auto s = f.start(); s->deliver(); gate.wait();
            auto closing = std::async(std::launch::async,[&] { return f.control->detachChannel(f.stack); });
            s->wait_stop(); // detach closed admission and reached worker retirement
            CHECK(closing.wait_for(0s) == std::future_status::timeout);
            gate.open(); CHECK(closing.wait_for(3s) == std::future_status::ready);
            CHECK(closing.get() == ir::Return::Success); gate.check();
            seams::set_stack_hook(nullptr,nullptr);
            if (point == seams::StackPoint::before_admission) {
                CHECK(f.listener->calls() == 0);
                CHECK(f.buffer->release() == ir::Return::Fail);
            } else {
                CHECK(f.listener->calls() == 1); f.exact();
                CHECK(f.buffer->release() == ir::Return::Success);
                CHECK(gige_mel::provider_statistics(f.manager).callback_failures == (throwing ? 1U : 0U));
            }
        }
    }
}
#endif
void teardown() {
    for (bool destruction : {false,true}) {
        Fixture f; f.attach(); Gate gate; test_join::SelfDetach reaper;
        f.listener->hook = [&](auto&){ if (destruction) reaper.arm(); gate.hold(); };
        auto s = f.start(); if (destruction) reaper.started(); s->deliver(); gate.wait();
        auto closing = std::async(std::launch::async,[&] {
            if (destruction) f.control.reset();
            else CHECK(f.control->detachChannel(f.stack) == ir::Return::Success);
        });
        // Source cancellation is proof teardown reached retirement (detach).
        // Control destruction instead remains pinned by the admitted callback.
        if (!destruction) {
            s->wait_stop();
            CHECK(closing.wait_for(0s) == std::future_status::timeout);
        } else {
            // Prove the last external Control reference is gone while the
            // admitted callback still pins it, before permitting callback exit.
            CHECK(closing.wait_for(3s) == std::future_status::ready);
        }
        gate.open(); CHECK(closing.wait_for(3s) == std::future_status::ready); closing.get();
        if (destruction) { reaper.join(); s->wait_stop(); }
        CHECK(!gate.expired); CHECK(f.listener->calls() == 1); f.exact();
        f.control.reset(); f.manager.reset(); // consumer memory and Buffer survive the graph
        f.exact();
        CHECK(f.buffer->release() == ir::Return::Success);
        CHECK(f.stack->enable() == ir::Return::Fail);
    }
    Fixture f; f.attach(); auto s = f.start(); CHECK(f.control->detachChannel(f.stack) == ir::Return::Success);
    s->deliver(); CHECK(f.listener->calls() == 0); // closed admission, no worker
}
}
int main() {
    try {
        test_join::prepare();
#if defined(GIGE_MEL_PROVIDER_TEST_SEAMS)
        admission_ordering();
#endif
        discovery(); payload_ownership(); independent_of_image_fov(); malformed(); callback_results(); lifecycle(); teardown();
        std::cout << "stack discovery, exact payload/header, ownership, failures, lifecycle and teardown passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}