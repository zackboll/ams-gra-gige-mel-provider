// A linked MEL consumer: only installed upstream declarations and the STL.
#include <irmel/library/irmel-types/Buffer.h>
#include <irmel/library/irmel-types/Control.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace ir = ams::iface::irmel;
using namespace std::chrono_literals;

namespace {
constexpr char instance[] = "synthetic";

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

class Listener final : public ir::ImageListener {
public:
    Listener(std::uint32_t width, std::uint32_t height) : width_(width), height_(height) {}

    void onImage(const ir::Channel&, const ir::FrameHeader& header,
                 std::shared_ptr<ir::Buffer> buffer) override {
        std::lock_guard lock(mutex_);
        try {
            if (!complete_) {
                require(buffer && header.getFormat() == ir::PixelFormat::Mono &&
                            header.getBitsPerPixel() == 8 && header.getNumBands() == 1 &&
                            header.getWidth() == width_ && header.getHeight() == height_,
                        "Unexpected frame layout");
                const auto bytes = std::uint64_t{width_} * height_;
                const auto* pixels = static_cast<const std::byte*>(buffer->getImageAddress());
                require(pixels && pixels == buffer->getBufferAddress() && buffer->getSize() > 0 &&
                            bytes <= static_cast<std::uint64_t>(buffer->getSize()),
                        "Invalid frame address or size");
                frame_id_ = header.getFrameID();
                image_.assign(pixels, pixels + static_cast<std::size_t>(bytes));
            }
        } catch (...) {
            failed_ = true;
        }
        // Even an unwanted extra frame or a failed copy must release its slot.
        try {
            if (!buffer || buffer->release() != ir::Return::Success) failed_ = true;
        } catch (...) {
            failed_ = true;
        }
        complete_ = true;
        ready_.notify_one();
    }

    void wait() {
        std::unique_lock lock(mutex_);
        require(ready_.wait_for(lock, 3s, [this] { return complete_; }), "No frame within three seconds");
    }

    // Called only after disable has joined the acquisition worker.
    void report() const {
        require(!failed_ && !image_.empty(), "Image validation, copy or release failed");
        std::cout << "Captured and released frame " << frame_id_ << ": " << width_ << 'x' << height_
                  << " Mono8, " << image_.size() << " bytes copied\n";
    }

private:
    const std::uint32_t width_, height_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool complete_{}, failed_{};
    std::uint32_t frame_id_{};
    std::vector<std::byte> image_;
};

struct CaptureSession {
    // Reverse destruction order also keeps host storage valid on partial setup.
    std::vector<std::byte> storage;
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::shared_ptr<Listener> listener;
    std::shared_ptr<ir::Buffer> buffer;
    std::shared_ptr<ir::Channel> channel;
    bool registered{};

    void close() {
        if (!channel) return;
        require(channel->disable() == ir::Return::Success, "Could not disable Image channel");
        if (registered) {
            require(channel->unregisterBuffer(buffer) == ir::Return::Success, "Could not unregister buffer");
            registered = false;
        }
        require(control->detachChannel(channel) == ir::Return::Success, "Could not detach Image channel");
        channel.reset();
    }

    ~CaptureSession() {
        try { close(); } catch (...) {
            // Best effort on errors; Control destruction also stops/detaches
            // attached channels. Do not destroy host storage before that drain.
            try { if (channel) (void)control->detachChannel(channel); } catch (...) {}
        }
    }
};
} // namespace

int main() {
    try {
        CaptureSession session;
        session.manager = getAPI_Manager(instance);
        require(session.manager != nullptr, "Could not create synthetic manager");
        session.control = ir::getControl(instance, session.manager);
        require(session.control != nullptr, "Could not create Control");
        require(session.control->init("") == ir::Return::Success, "Control initialization failed");

        const auto& capabilities = session.control->getCapabilities();
        const auto image = std::find_if(capabilities.begin(), capabilities.end(), [](const auto& cap) {
            const auto& types = cap.getChannelTypes();
            return std::find(types.begin(), types.end(), ir::ChannelType::IRSTImage) != types.end();
        });
        require(image != capabilities.end(), "No advertised Image channel");

        session.listener = std::make_shared<Listener>(image->getWidth(), image->getHeight());
        ir::Config config;
        config.setChanID(image->getChanID());
        config.setChannelType(ir::ChannelType::IRSTImage);
        config.setPlatform(image->getPlatform());
        config.setSensorLocation(image->getSensorLocation());
        config.setImgLstnr(session.listener);
        session.channel = session.control->attachChannel(config);
        require(session.channel != nullptr, "Could not attach advertised Image channel");

        const auto layout = session.channel->getCapabilities();
        const auto bytes = std::uint64_t{layout.getWidth()} * layout.getHeight();
        require(layout.getFormat() == ir::PixelFormat::Mono && layout.getBitDepth() == 8 &&
                    layout.getNumberOfBands() == 1 && layout.getRowPitch() == layout.getWidth() &&
                    layout.getWidth() == image->getWidth() && layout.getHeight() == image->getHeight() &&
                    bytes > 0 && bytes <= layout.getBufferSize() && bytes == layout.getImageSize(),
                "Example requires tightly packed Mono8");

        session.storage.resize(layout.getBufferSize());
        session.buffer = ir::getBuffer(instance, session.manager);
        require(session.buffer != nullptr, "Could not create Buffer");
        require(session.buffer->init(session.storage.data(), session.storage.size(), 0) == ir::Return::Success,
                "Buffer initialization failed");
        require(session.channel->registerBuffer(session.buffer) == ir::Return::Success, "Buffer registration failed");
        session.registered = true;
        require(session.channel->enable() == ir::Return::Success, "Could not enable Image channel");
        session.listener->wait();
        session.close();
        session.listener->report();
    } catch (const std::exception& error) {
        std::cerr << "capture_one: " << error.what() << '\n';
        return 1;
    }
}