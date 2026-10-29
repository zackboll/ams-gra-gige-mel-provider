#include <irmel/library/irmel-types/Control.h>
#include <irmel/library/c2/C2Channel.h>
#include "../public_health_demo.hpp"
#include "../public_bit_demo.hpp"

#include <iostream>

int main() {
    auto manager = getAPI_Manager("synthetic");
    auto control = ams::iface::irmel::getControl("synthetic", manager);
    if (!manager || !control || control->init() != ams::iface::irmel::Return::Success ||
        control->getCapabilities().size() != 3) {
        std::cerr << "Installed package did not expose the expected provider\n";
        return 1;
    }
    namespace ir = ams::iface::irmel;
    try { demonstrate_health(control); demonstrate_bit(control); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 6; }
    ir::Config config;
    for (const auto& capability : control->getCapabilities()) {
        if (capability.getChannelTypes().front() != ir::ChannelType::CommandAndControl) continue;
        config.setChannelType(ir::ChannelType::CommandAndControl);
        config.setChanID(capability.getChanID());
    }
    auto c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(config));
    if (!c2) return 2;
    unsigned statuses = 0;
    bool accepted = false;
    if (c2->registerMetadataCallback(std::function<void(ir::Channel&, const ir::CommandStatus*)>{
        [&](auto&, auto* status) {
            ++statuses;
            accepted = status->getCommandID() == 42 && status->getState() == ir::CommandState::Accepted &&
                status->getReasonID() == ir::CannotComply::NotSet && status->getReasonDescription().empty();
        }}) != ir::Return::Success) return 3;
    ir::ConfigSetCommand command;
    command.setCommandID(42);
    command.setConfig(R"({"schema":"gige-mel.camera-config","version":1,"width":64,"height":48,"offset_x":0,"offset_y":0,"frame_rate_hz":30,"exposure_ns":1000000})");
    auto result = c2->send(command).get();
    if (!result || *result.get() != ir::Return::Success || !accepted || statuses != 1) return 4;
    if (control->detachChannel(c2) != ir::Return::Success) return 5;
    std::cout << "PASS: installed package, factories, BIT catalogue/results/Health faults, Health snapshots, ConfigSet and correlated CommandStatus (published MEL only)\n";
}
