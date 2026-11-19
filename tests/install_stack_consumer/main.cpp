// Consumer uses ONLY installed upstream MEL declarations, not provider headers.
#include <irmel/library/irmel-types/Control.h>
#include <irmel/library/irmel-types/Buffer.h>
#include <irmel/library/stacked-image/StackedImageChannel.h>
#include <irmel/library/stacked-image/StackedImageListener.h>
#include <array>
#include <future>
#include <iostream>
#include <stdexcept>
namespace ir = ams::iface::irmel;
std::shared_ptr<API_Manager> recording_manager();
void deliver_recording();
struct Listener final : ir::StackedImageListener {
    std::promise<bool> delivered;
    ir::Return onImageStack(ir::Channel& c, const ir::StackFrameHeader& h, std::shared_ptr<ir::Buffer> b) override {
        bool correct = dynamic_cast<ir::StackedImageChannel*>(&c) && h.getWidth() == 4 && h.getHeight() == 3 &&
            h.getStackDepth() == 2 && h.getPackedPixelsSize() == 24 && h.getStackId() == 701;
        const auto* bytes = static_cast<const unsigned char*>(b->getImageAddress());
        for (unsigned i = 0; i < 24; ++i) correct = correct && bytes[i] == (i < 12 ? i + 1 : i + 89);
        delivered.set_value(correct);
        return ir::Return::Success; // explicitly does NOT release
    }
};
int main() {
    try {
        std::array<unsigned char,24> memory{};
        auto manager = recording_manager();
        auto control = ir::getControl("synthetic",manager);
        if (!control || control->init() != ir::Return::Success) throw std::runtime_error("init");
        auto listener = std::make_shared<Listener>();
        ir::Config config;
        bool found = false;
        for (const auto& cap : control->getCapabilities()) if (cap.getChannelTypes() == std::vector{ir::ChannelType::StackedImage}) {
            config.setChanID(cap.getChanID()); config.setChannelType(ir::ChannelType::StackedImage); config.setImgLstnr(listener); found = true;
        }
        if (!found) throw std::runtime_error("discovery");
        auto channel = std::dynamic_pointer_cast<ir::StackedImageChannel>(control->attachChannel(config));
        auto buffer = ir::getBuffer("synthetic",manager);
        if (!channel || buffer->init(memory.data(),memory.size(),44) != ir::Return::Success ||
            channel->registerBuffer(buffer) != ir::Return::Success || channel->enable() != ir::Return::Success)
            throw std::runtime_error("attach/register/enable");
        auto received = listener->delivered.get_future(); deliver_recording();
        if (received.wait_for(std::chrono::seconds(3)) != std::future_status::ready || !received.get()) throw std::runtime_error("delivery");
        if (channel->disable() != ir::Return::Success || control->detachChannel(channel) != ir::Return::Success ||
            memory[0] != 1 || memory[23] != 112 || buffer->release() != ir::Return::Success) throw std::runtime_error("ownership");
        std::cout << "Installed MEL-only consumer received exact pre-formed stack; explicit release after detach passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}