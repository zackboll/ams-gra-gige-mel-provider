#include <gige_mel/provider.hpp>
#include "support/recording_task_backend.hpp"
#include "support/recording_stack_backend.hpp"
#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/scheduling/SchedulingChannel.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/stacked-image/StackedImageChannel.h>
#include <irmel/library/stacked-image/StackedImageListener.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <barrier>
#include <condition_variable>
#include <future>
#include <iostream>
#if defined(GIGE_MEL_PROVIDER_TEST_SEAMS)
#include "../src/provider_test_seams.hpp"
#endif

namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
namespace b = gige_mel::backend;
namespace r = recording_task;
using namespace std::chrono_literals;
#define CHECK(e) do { if (!(e)) throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) + ": " #e); } while(false)

namespace {
ir::IRSTStareTask task(std::uint32_t command = 71, std::uint32_t request = 92) {
    ir::TaskInfo info; info.setReqId(request); info.setAction(ir::TaskListAction::Immediate);
    ir::StareTaskParam p; p.setDurationNanoseconds(5ns);
    p.setCentFrameRefAz(ir::CoordFrameRef::Aircraft); p.setCentFrameRefEl(ir::CoordFrameRef::Aircraft);
    return {command, p, info};
}
struct Fixture {
    std::shared_ptr<r::Recording> recording;
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::shared_ptr<ir::C2Channel> c2;
    std::shared_ptr<ir::SchedulingChannel> scheduling;
    explicit Fixture(std::shared_ptr<r::Recording> rec = std::make_shared<r::Recording>(),
                     std::shared_ptr<b::ICameraBackend> camera = b::make_synthetic_backend()) : recording(std::move(rec)) {
        manager = gige_mel::make_api_manager({}, recording ? std::shared_ptr<b::ICameraBackend>{std::make_shared<r::Backend>(recording, std::move(camera))} : b::make_synthetic_backend());
        control = ir::getControl("synthetic", manager); CHECK(control && control->init() == ir::Return::Success);
        for (const auto& cap : control->getCapabilities()) {
            ir::Config config; config.setChanID(cap.getChanID()); config.setChannelType(cap.getChannelTypes().front());
            if (config.getChannelType() == ir::ChannelType::CommandAndControl)
                c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(config));
            if (config.getChannelType() == ir::ChannelType::Scheduling)
                scheduling = std::dynamic_pointer_cast<ir::SchedulingChannel>(control->attachChannel(config));
        }
        CHECK(c2);
    }
    mel::ErrorOr<std::shared_ptr<ir::MFA_Mode>> mode(bool operate, std::uint32_t id = 42) {
        ir::ModeCmd cmd; cmd.setCommandID(id); cmd.setState(operate ? mel::MFA_State::Operate : mel::MFA_State::Standby);
        cmd.setMode(operate ? ir::MFA_Mode::TaskSched : ir::MFA_Mode::Unused); return c2->send(cmd).get();
    }
};

void positive() {
    Fixture f; CHECK(f.scheduling);
    const auto cap = f.scheduling->getCapabilities();
    CHECK(cap.getChanID().getUUID().back() == 5 && cap.getTaskScheduleDepth() == 0);
    CHECK(cap.getChannelMetadataCapabilities() == std::set({ir::ChannelMetadataCapabilityType::ChannelCommsTestRep,
        ir::ChannelMetadataCapabilityType::ExecuteTaskAck}));
    ir::CommandStatus status; unsigned statuses = 0;
    CHECK(f.c2->registerMetadataCallback([&](ir::Channel&, const ir::CommandStatus* s) { status = *s; ++statuses; }) == ir::Return::Success);
    CHECK(f.mode(false) && f.recording->opens == 0);
    auto mode = f.mode(true, UINT32_MAX); CHECK(mode && *mode.get() == ir::MFA_Mode::TaskSched);
    CHECK(status.getCommandID() == UINT32_MAX && status.getState() == ir::CommandState::Accepted);
    CHECK(f.recording->owned && f.recording->opens == 1 && f.recording->snapshot().phase == b::TaskPhase::idle);
    CHECK(f.mode(true) && f.recording->opens == 1);
    unsigned acks = 0;
    CHECK(f.scheduling->registerMetadataCallback([&](ir::Channel&, const ir::ExecuteTaskAck* a) {
        CHECK(a->getCommandID() == 0 && a->getReqId() == UINT32_MAX); ++acks;
        CHECK(!f.mode(false)); CHECK(!f.scheduling->exec(ir::AbortTask{}).get());
        throw std::runtime_error("consumer exception");
    }) == ir::Return::Success);
    CHECK(f.scheduling->exec(task(0, UINT32_MAX)).get()); CHECK(acks == 1);
    CHECK(f.recording->snapshot().phase == b::TaskPhase::admitted);
    CHECK(f.recording->snapshot().task->command_id == 0 && f.recording->snapshot().task->request_id == UINT32_MAX);
    f.recording->executing(); CHECK(f.recording->snapshot().phase == b::TaskPhase::executing);
    f.recording->advance(4ns); CHECK(f.recording->snapshot().phase == b::TaskPhase::executing);
    f.recording->advance(1ns); CHECK(f.recording->snapshot().phase == b::TaskPhase::completed);
    CHECK(!f.scheduling->exec(ir::AbortTask{}).get());
    CHECK(f.mode(false)); CHECK(!f.recording->owned && f.recording->retirements == 1);
    CHECK(statuses == 4 && gige_mel::provider_statistics(f.manager).callback_failures == 1);
}

void negatives() {
    Fixture defaults(nullptr); CHECK(!defaults.scheduling);
    ir::CommandStatus status;
    CHECK(defaults.c2->registerMetadataCallback([&](ir::Channel&, const ir::CommandStatus* s) { status = *s; }) == ir::Return::Success);
    auto unsupported = defaults.mode(true, 99); CHECK(!unsupported && unsupported.getError().getCode() == mel::ErrorCode::Unsupported);
    CHECK(status.getCommandID() == 99 && status.getReasonID() == ir::CannotComply::CapabilityUnavailable);
    auto invalid = std::make_shared<r::Recording>(); invalid->declarations.atomic_immediate_replacement = false;
    Fixture unqualified(invalid); CHECK(!unqualified.scheduling && !unqualified.mode(true));
    Fixture f; CHECK(!f.scheduling->exec(task()).get());
    f.recording->fail_open = true; CHECK(!f.mode(true) && !f.recording->owned);
    f.recording->fail_open = false; CHECK(f.mode(true));
    unsigned acks = 0;
    CHECK(f.scheduling->registerMetadataCallback([&](ir::Channel&, const ir::ExecuteTaskAck*) { ++acks; }) == ir::Return::Success);
    CHECK(f.scheduling->exec(task()).get());
    for (auto action : {ir::TaskListAction::Normal, ir::TaskListAction::Next, ir::TaskListAction::Abort, static_cast<ir::TaskListAction>(999)}) {
        auto t = task(1, 3); auto info = t.getTaskInfo(); info.setAction(action); t.setTaskInfo(info);
        CHECK(!f.scheduling->exec(t).get());
    }
    for (int i = 0; i < 12; ++i) {
        auto t = task(3, 4); auto info = t.getTaskInfo(); auto p = t.getParams();
        switch (i) {
        case 0: info.setSystemTime(-1ns); break;
        case 1: info.setSystemTime(1ns); break;
        case 2: info.setPriority(1); break;
        case 3: info.setPrecedenceWithinPriority(1); break;
        case 4: p.setDurationNanoseconds(0ns); break;
        case 5: p.setDurationNanoseconds(-1ns); break;
        case 6: p.setDurationNanoseconds(11s); break;
        case 7: p.setCentFrameRefAz(ir::CoordFrameRef::Inertial); break;
        case 8: p.setCentFrameRefEl(ir::CoordFrameRef::Inertial); break;
        case 9: p.setCenter({1, 0}); break;
        case 10: p.setCenter({std::numeric_limits<double>::quiet_NaN(), 0}); break;
        case 11: p.setCenter({0, std::numeric_limits<double>::infinity()}); break;
        }
        t.setTaskInfo(info); t.setTaskParam(p); CHECK(!f.scheduling->exec(t).get());
    }
    CHECK(!f.scheduling->exec(task(99, 92)).get()); // request ID, not command ID, is duplicate key
    CHECK(acks == 1 && f.recording->admissions == 1 && f.recording->snapshot().task->request_id == 92);
    f.recording->fail_admit = true; CHECK(!f.scheduling->exec(task(71, 0)).get());
    CHECK(f.recording->snapshot().task->request_id == 92 && acks == 1);
    f.recording->fail_admit = false; CHECK(f.scheduling->exec(task(71, 0)).get());
    CHECK(f.recording->replacements == 1 && f.recording->cancellations == 1 && acks == 2);
    f.recording->executing(); f.recording->fail_cancel = true;
    CHECK(!f.scheduling->exec(ir::AbortTask{}).get() && !f.mode(false));
    CHECK(f.recording->owned && f.recording->snapshot().phase == b::TaskPhase::executing);
    f.recording->fail_cancel = false;
    ir::TaskInfo unrelated; unrelated.setReqId(12345);
    CHECK(f.scheduling->exec(ir::AbortTask(unrelated)).get()); // only current task, hidden IDs ignored
    CHECK(f.recording->snapshot().phase == b::TaskPhase::cancelled);
    CHECK(!f.scheduling->exec(ir::IRSTScanTask{}).get());
    CHECK(!f.scheduling->exec(ir::IRSTTrackTask{}).get());
    CHECK(!f.scheduling->send(ir::PrioritizedTrackListCmd{}).get());
    CHECK(f.scheduling->registerMetadataCallback([](ir::Channel&, const ir::TaskExecutingRep*) {}) == ir::Return::NotSupported);
    CHECK(f.scheduling->registerMetadataCallback([](ir::Channel&, const ir::TaskEvents*) {}) == ir::Return::NotSupported);
    CHECK(f.mode(false) && !f.recording->owned);
    CHECK(f.mode(true)); CHECK(f.scheduling->exec(task()).get()); f.recording->fail();
    CHECK(f.mode(false) && !f.recording->owned);
}

void lifecycle() {
    Fixture f; CHECK(f.mode(true));
    std::promise<void> entered, release; auto released = release.get_future().share();
    unsigned callbacks = 0;
    CHECK(f.scheduling->registerMetadataCallback([&](ir::Channel&, const ir::ExecuteTaskAck*) {
        ++callbacks; entered.set_value(); released.wait();
    }) == ir::Return::Success);
    auto submitting = std::async(std::launch::async, [&] { return f.scheduling->exec(task()).get(); });
    CHECK(entered.get_future().wait_for(2s) == std::future_status::ready);
    std::promise<void> detaching;
    auto detached = std::async(std::launch::async, [&] { detaching.set_value(); return f.control->detachChannel(f.scheduling); });
    detaching.get_future().wait();
    f.recording->wait_retired(); // actual detach source retirement proves it reached drain
    CHECK(detached.wait_for(0s) != std::future_status::ready);
    release.set_value(); CHECK(submitting.get()); CHECK(detached.get() == ir::Return::Success);
    CHECK(!f.recording->owned && !f.scheduling->exec(task(2, 3)).get() && callbacks == 1);
    Fixture concurrent; CHECK(concurrent.mode(true));
    std::barrier gate(3);
    auto a = std::async(std::launch::async, [&] { gate.arrive_and_wait(); return concurrent.scheduling->exec(task(1, 7)).get(); });
    auto c = std::async(std::launch::async, [&] { gate.arrive_and_wait(); return concurrent.scheduling->exec(task(2, 7)).get(); });
    gate.arrive_and_wait(); const bool av = bool(a.get()), cv = bool(c.get()); CHECK(av != cv);
    CHECK(concurrent.recording->admissions == 1);
    CHECK(concurrent.mode(false) && concurrent.recording->snapshot().phase == b::TaskPhase::cancelled);
    CHECK(concurrent.mode(true)); CHECK(concurrent.scheduling->exec(task()).get());
    concurrent.recording->fail_cancel = true; concurrent.recording->fail_observe = true;
    CHECK(!concurrent.mode(false)); concurrent.control.reset();
    CHECK(!concurrent.recording->owned && concurrent.recording->snapshot().phase == b::TaskPhase::failed);
    CHECK(!concurrent.scheduling->exec(task(3, 4)).get());
}

void capacity_and_reentry() {
    Fixture f; CHECK(f.mode(true));
    for (std::uint32_t i = 0; i < 16; ++i) CHECK(f.scheduling->exec(task(0, i)).get());
    CHECK(!f.scheduling->exec(task(0, 16)).get());
    CHECK(f.recording->admissions == 16 && f.recording->replacements == 15);
    ir::ModeCmd invalid; invalid.setState(mel::MFA_State::OperateRxOnly); invalid.setMode(ir::MFA_Mode::TaskSched);
    CHECK(!f.c2->send(invalid).get() && f.recording->snapshot().task->request_id == 15);
    invalid.setState(mel::MFA_State::Operate); invalid.setMode(ir::MFA_Mode::ScanVolumeSched);
    CHECK(!f.c2->send(invalid).get() && f.recording->snapshot().task->request_id == 15);
    Fixture other;
    CHECK(other.c2->registerMetadataCallback([&](ir::Channel&, const ir::CommandStatus*) {
        CHECK(!f.scheduling->exec(ir::AbortTask{}).get());
        CHECK(!f.scheduling->exec(task(1, 100)).get()); CHECK(!f.mode(false));
    }) == ir::Return::Success);
    CHECK(other.mode(false)); CHECK(f.recording->snapshot().task->request_id == 15);
    CHECK(f.mode(false)); CHECK(f.mode(true)); CHECK(f.scheduling->exec(task(0, 0)).get());
    CHECK(f.mode(false)); // duplicate history reset only for new owned session
}

struct Listener final : ir::StackedImageListener {
    std::promise<void> received;
    void onImage(const ir::Channel&, const ir::FrameHeader&, std::shared_ptr<ir::Buffer>) override { received.set_value(); }
    ir::Return onImageStack(ir::Channel&, const ir::StackFrameHeader&, std::shared_ptr<ir::Buffer>) override {
        received.set_value(); return ir::Return::Success;
    }
};

void acquisition_exclusion() {
    for (auto type : {ir::ChannelType::IRSTImage, ir::ChannelType::StackedImage}) {
        auto stacks = std::make_shared<recording_stack::Recording>();
        Fixture f(std::make_shared<r::Recording>(), std::make_shared<recording_stack::Backend>(stacks));
        auto listener = std::make_shared<Listener>();
        std::shared_ptr<ir::Channel> channel;
        for (const auto& cap : f.control->getCapabilities()) if (cap.getChannelTypes() == std::vector{type}) {
            ir::Config config; config.setChannelType(type); config.setChanID(cap.getChanID()); config.setImgLstnr(listener);
            channel = f.control->attachChannel(config);
        }
        CHECK(channel);
        std::array<std::byte, 3072> bytes{};
        auto buffer = ir::getBuffer("synthetic", f.manager);
        CHECK(buffer->init(bytes.data(), bytes.size(), 7) == ir::Return::Success);
        CHECK(channel->registerBuffer(buffer) == ir::Return::Success);
        CHECK(f.mode(true)); CHECK(channel->enable() == ir::Return::Fail);
        CHECK(f.mode(false)); CHECK(channel->enable() == ir::Return::Success);
        if (type == ir::ChannelType::StackedImage) stacks->latest->deliver();
        CHECK(listener->received.get_future().wait_for(3s) == std::future_status::ready);
        const auto held = bytes;
        CHECK(!f.mode(true)); CHECK(f.recording->opens == 1);
        CHECK(channel->disable() == ir::Return::Success);
        CHECK(f.mode(true)); CHECK(f.scheduling->exec(task()).get());
        CHECK(f.mode(false) && bytes == held); // tasking never releases/overwrites held bytes
        CHECK(buffer->release() == ir::Return::Success);
        CHECK(f.control->detachChannel(channel) == ir::Return::Success);
    }
}

void health() {
    Fixture f;
    std::shared_ptr<ir::HealthStatusChannel> health_channel;
    for (const auto& cap : f.control->getCapabilities()) if (cap.getChannelTypes() == std::vector{ir::ChannelType::HealthAndStatus}) {
        ir::Config config; config.setChannelType(ir::ChannelType::HealthAndStatus); config.setChanID(cap.getChanID());
        health_channel = std::dynamic_pointer_cast<ir::HealthStatusChannel>(f.control->attachChannel(config));
    }
    CHECK(health_channel);
    std::mutex mutex; std::condition_variable changed;
    mel::MFA_State state = mel::MFA_State::Unknown;
    CHECK(health_channel->registerMetadataCallback([&](ir::Channel&, const mel::MFA_Status* s) {
        { std::lock_guard lock(mutex); state = s->getMFAState(); } changed.notify_all();
    }) == ir::Return::Success);
    CHECK(health_channel->enable() == ir::Return::Success);
    auto wait = [&](mel::MFA_State expected) {
        std::unique_lock lock(mutex); CHECK(changed.wait_for(lock, 3s, [&] { return state == expected; }));
    };
    wait(mel::MFA_State::Standby); CHECK(f.mode(true)); wait(mel::MFA_State::Operate);
    CHECK(f.scheduling->exec(task()).get()); CHECK(f.mode(false)); wait(mel::MFA_State::Standby);
    CHECK(health_channel->disable() == ir::Return::Success);
    CHECK(f.control->detachChannel(health_channel) == ir::Return::Success);
}

void control_pin() {
    Fixture f; CHECK(f.mode(true));
    std::promise<void> entered, release; auto released = release.get_future().share();
    CHECK(f.scheduling->registerMetadataCallback([&](ir::Channel&, const ir::ExecuteTaskAck*) {
        entered.set_value(); released.wait();
    }) == ir::Return::Success);
    auto submit = std::async(std::launch::async, [&] { return f.scheduling->exec(task()).get(); });
    CHECK(entered.get_future().wait_for(3s) == std::future_status::ready);
    f.control.reset(); // submission pins it through callback; no destruction yet
    { std::lock_guard lock(f.recording->mutex); CHECK(f.recording->owned); }
    release.set_value(); CHECK(submit.get()); f.recording->wait_retired();
    CHECK(!f.scheduling->exec(task(1, 2)).get());
}

#if defined(GIGE_MEL_PROVIDER_TEST_SEAMS)
void reserved_admission() {
    Fixture f; CHECK(f.mode(true));
    std::promise<void> reserved, release; auto released = release.get_future().share();
    struct Gate { std::promise<void>& reserved; std::shared_future<void> released; } gate{reserved, released};
    unsigned calls = 0;
    CHECK(f.scheduling->registerMetadataCallback([&](ir::Channel&, const ir::ExecuteTaskAck*) { ++calls; }) == ir::Return::Success);
    auto submit = std::async(std::launch::async, [&] {
        gige_mel::test_seams::set_task_hook([](void* p) noexcept {
            auto& g = *static_cast<Gate*>(p); g.reserved.set_value(); g.released.wait();
        }, &gate);
        auto result = f.scheduling->exec(task()).get();
        gige_mel::test_seams::set_task_hook(nullptr, nullptr); return result;
    });
    CHECK(reserved.get_future().wait_for(3s) == std::future_status::ready);
    auto detach = std::async(std::launch::async, [&] { return f.control->detachChannel(f.scheduling); });
    f.recording->wait_retired(); CHECK(calls == 0 && detach.wait_for(0s) != std::future_status::ready);
    release.set_value(); CHECK(submit.get()); CHECK(detach.get() == ir::Return::Success && calls == 1);
    CHECK(!f.scheduling->exec(task(3, 4)).get() && calls == 1);
}
#endif
} // namespace

int main() {
    try {
        positive(); negatives(); lifecycle(); capacity_and_reentry(); acquisition_exclusion(); health(); control_pin();
#if defined(GIGE_MEL_PROVIDER_TEST_SEAMS)
        reserved_admission();
#endif
        std::cout << "task scheduling contracts passed\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}