// Consumer translation unit uses ONLY published upstream MEL headers.
#include <irmel/library/irmel-types/Control.h>
#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/scheduling/SchedulingChannel.h>
#include <iostream>
#include <stdexcept>

namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
std::shared_ptr<API_Manager> task_recording_manager();
bool task_recording_progress();
bool task_recording_cancelled();
#define CHECK(e) do { if (!(e)) throw std::runtime_error(#e); } while (false)

int main() {
    try {
        auto manager = task_recording_manager();
        auto control = ir::getControl("synthetic", manager);
        CHECK(control && control->init() == ir::Return::Success);
        std::shared_ptr<ir::C2Channel> c2;
        std::shared_ptr<ir::SchedulingChannel> scheduling;
        for (const auto& cap : control->getCapabilities()) {
            ir::Config config; config.setChanID(cap.getChanID()); config.setChannelType(cap.getChannelTypes().front());
            if (config.getChannelType() == ir::ChannelType::CommandAndControl)
                c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(config));
            if (config.getChannelType() == ir::ChannelType::Scheduling)
                scheduling = std::dynamic_pointer_cast<ir::SchedulingChannel>(control->attachChannel(config));
        }
        CHECK(c2 && scheduling);
        ir::CommandStatus status;
        CHECK(c2->registerMetadataCallback([&](ir::Channel&, const ir::CommandStatus* s) { status = *s; }) == ir::Return::Success);
        ir::ModeCmd cmd; cmd.setCommandID(41); cmd.setState(mel::MFA_State::Operate); cmd.setMode(ir::MFA_Mode::TaskSched);
        auto operational = c2->send(cmd).get();
        CHECK(operational && **operational == ir::MFA_Mode::TaskSched);
        CHECK(status.getCommandID() == 41 && status.getState() == ir::CommandState::Accepted);
        unsigned acks = 0;
        CHECK(scheduling->registerMetadataCallback([&](ir::Channel&, const ir::ExecuteTaskAck* a) {
            CHECK(a->getCommandID() == 81 && a->getReqId() == 19 + acks); ++acks;
        }) == ir::Return::Success);
        ir::TaskInfo info; info.setReqId(19); info.setAction(ir::TaskListAction::Immediate);
        ir::StareTaskParam params; params.setDurationNanoseconds(std::chrono::nanoseconds{5});
        params.setCentFrameRefAz(ir::CoordFrameRef::Aircraft); params.setCentFrameRefEl(ir::CoordFrameRef::Aircraft);
        CHECK(scheduling->exec(ir::IRSTStareTask(81, params, info)).get());
        CHECK(acks == 1 && task_recording_progress());
        info.setReqId(20); CHECK(scheduling->exec(ir::IRSTStareTask(81, params, info)).get());
        CHECK(scheduling->exec(ir::AbortTask{}).get() && task_recording_cancelled() && acks == 2);
        cmd.setState(mel::MFA_State::Standby); cmd.setMode(ir::MFA_Mode::Unused);
        auto standby = c2->send(cmd).get(); CHECK(standby && **standby == ir::MFA_Mode::Unused);
        CHECK(control->detachChannel(scheduling) == ir::Return::Success);
        std::cout << "Installed MEL-only virtual task admission/progression/cancellation passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}