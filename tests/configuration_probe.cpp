#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/irmel-types/Control.h>

#include <iostream>
#include <string_view>

// Public-API control: compile this same source against starting main's library
// and use --expect-unsupported, then run without that flag against the feature.
int main(int argc, char** argv) {
    namespace ir = ams::iface::irmel;
    namespace mel = ams::iface::mel;
    const bool baseline = argc == 2 && std::string_view(argv[1]) == "--expect-unsupported";
    auto manager = getAPI_Manager("synthetic");
    auto control = ir::getControl("synthetic", manager);
    if (!control || control->init() != ir::Return::Success) return 1;
    ir::Config config;
    for (const auto& capability : control->getCapabilities()) {
        if (capability.getChannelTypes().front() != ir::ChannelType::CommandAndControl) continue;
        config.setChannelType(ir::ChannelType::CommandAndControl);
        config.setChanID(capability.getChanID());
    }
    auto c2 = std::dynamic_pointer_cast<ir::C2Channel>(control->attachChannel(config));
    if (!c2) return 2;
    ir::ConfigSetCommand command;
    command.setCommandID(123);
    command.setConfig(R"({"schema":"gige-mel.camera-config","version":1,"width":64,"height":48,"offset_x":0,"offset_y":0,"frame_rate_hz":30,"exposure_ns":1000000})");
    auto without_callback = c2->send(command).get();
    unsigned statuses = 0;
    bool correlated = false;
    const auto registration = c2->registerMetadataCallback(
        std::function<void(ir::Channel&, const ir::CommandStatus*)>{[&](auto&, auto* status) {
            ++statuses;
            correlated = status->getCommandID() == 123 && status->getState() == ir::CommandState::Accepted &&
                status->getReasonID() == ir::CannotComply::NotSet && status->getReasonDescription().empty();
        }});
    auto result = c2->send(command).get();
    if (baseline) {
        if (registration != ir::Return::NotSupported || result || without_callback || statuses != 0 ||
            result.getError().getCode() != mel::ErrorCode::Unsupported ||
            without_callback.getError().getCode() != mel::ErrorCode::Unsupported) return 3;
        std::cout << "BASELINE: valid ConfigSet=Unsupported; status registration=NotSupported; statuses=0\n";
    } else {
        if (registration != ir::Return::Success || !result || !without_callback ||
            *result.get() != ir::Return::Success || *without_callback.get() != ir::Return::Success ||
            statuses != 1 || !correlated) return 4;
        std::cout << "FEATURE: ConfigSet=Success with/without callback; correlated Accepted=1\n";
    }
    return control->detachChannel(c2) == ir::Return::Success ? 0 : 5;
}