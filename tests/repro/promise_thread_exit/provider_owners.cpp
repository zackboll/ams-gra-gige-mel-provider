#include <irmel/library/irmel-types/Control.h>
#include <irmel/library/health-status/HealthStatusChannel.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>

namespace ir = ams::iface::irmel;
using namespace std::chrono_literals;

// Actual production self-detach, no pthread interposer, SDK or camera backend.
// Diagnostic counterpart to the joined test reaper, not a passing CTest gate.
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const int count = std::stoi(argv[1]);
    if (count < 1) return 2;
    for (int i = 0; i < count; ++i) {
        auto manager = getAPI_Manager("synthetic");
        auto control = ir::getControl("synthetic", manager);
        if (!control || control->init() != ir::Return::Success) std::abort();
        ir::Config config;
        for (const auto& cap : control->getCapabilities()) {
            if (cap.getChannelTypes().front() == ir::ChannelType::HealthAndStatus) {
                config.setChannelType(ir::ChannelType::HealthAndStatus);
                config.setChanID(cap.getChanID());
            }
        }
        auto health = std::dynamic_pointer_cast<ir::HealthStatusChannel>(control->attachChannel(config));
        if (!health) std::abort();
        std::weak_ptr<ir::Control> weak_control = control;
        std::weak_ptr<ir::HealthStatusChannel> weak_health = health;
        std::promise<void> enabled, body, done;
        auto proceed = enabled.get_future();
        auto completed = body.get_future();
        auto exited = done.get_future();
        bool safe = false;
        if (health->registerMetadataCallback(std::function<void(ir::Channel&, const ir::SubsystemStatusResp*)>{
            [&](auto& source, auto*) {
                proceed.wait(); // Drop enable's temporary Control owner first.
                control.reset(); health.reset();
                auto reply = source.sendKeepAliveRep().get();
                safe = !weak_control.expired() && !weak_health.expired() && reply &&
                       *reply.get() == ir::Return::Success;
                done.set_value_at_thread_exit();
                body.set_value(); // Separately order promise method completion.
            }}) != ir::Return::Success) std::abort();
        if (health->enable() != ir::Return::Success) std::abort();
        enabled.set_value();
        if (completed.wait_for(2s) != std::future_status::ready || !safe) std::abort();
        if (exited.wait_for(2s) != std::future_status::ready) std::abort();
        if (!weak_control.expired() || !weak_health.expired()) std::abort();
    }
    std::cout << count << " actual self-detached provider workers completed\n";
}