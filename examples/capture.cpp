// Standalone native MEL consumer. This example uses only upstream headers and
// dynamically loads the provider; it does not link to its implementation.
#include <irmel/library/irmel-types/Control.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace ir = ams::iface::irmel;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

class SharedLibrary final {
public:
    explicit SharedLibrary(const char* path) : handle_(::dlopen(path, RTLD_NOW | RTLD_LOCAL)) {
        if (!handle_) {
            const char* error = ::dlerror();
            throw std::runtime_error(error ? error : "dlopen failed");
        }
    }
    ~SharedLibrary() { if (handle_) { ::dlclose(handle_); } }
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    template <class Function>
    Function symbol(const char* name) const {
        ::dlerror();
        void* address = ::dlsym(handle_, name);
        const char* error = ::dlerror();
        if (error || !address) {
            throw std::runtime_error(std::string("Cannot resolve ") + name + ": " + (error ? error : "null symbol"));
        }
        return reinterpret_cast<Function>(address); // POSIX dlsym contract.
    }

private:
    void* handle_{};
};

struct CaptureSession final {
    // Declare storage first so it is also destroyed last on partial failure.
    std::vector<std::vector<std::byte>> storage;
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::vector<std::shared_ptr<ir::Buffer>> buffers;
    std::shared_ptr<ir::Channel> channel;

    ~CaptureSession() {
        if (channel) {
            try { (void)channel->disable(); } catch (...) {}
            for (const auto& buffer : buffers) {
                try { (void)channel->unregisterBuffer(buffer); } catch (...) {}
            }
            if (control) {
                try { (void)control->detachChannel(channel); } catch (...) {}
            }
        }
        channel.reset();
        buffers.clear();
        control.reset();
        manager.reset();
    }

    void close() {
        check(channel->disable() == ir::Return::Success, "Could not stop acquisition");
        for (const auto& buffer : buffers) {
            check(channel->unregisterBuffer(buffer) == ir::Return::Success, "Could not unregister a buffer");
        }
        check(control->detachChannel(channel) == ir::Return::Success, "Could not detach image channel");
        channel.reset();
        buffers.clear();
        control.reset();
        manager.reset();
    }
};

struct Snapshot {
    ir::FrameHeader header;
    std::vector<std::byte> pixels;
};

class SnapshotListener final : public ir::ImageListener {
public:
    void onImage(const ir::Channel&, const ir::FrameHeader& header,
                 std::shared_ptr<ir::Buffer> buffer) override {
        std::string error;
        try {
            check(buffer != nullptr, "Provider delivered a null buffer");
            std::lock_guard lock(mutex_);
            if (!complete_) {
                check(header.getBitsPerPixel() == 8 && header.getNumBands() == 1 &&
                          header.getFormat() == ir::PixelFormat::Mono,
                      "This PGM example supports single-band Mono8 only");
                const auto bytes = static_cast<std::uint64_t>(header.getWidth()) * header.getHeight();
                check(bytes > 0 && buffer->getSize() >= 0 &&
                          bytes <= static_cast<std::uint64_t>(buffer->getSize()),
                      "Invalid frame size");
                check(buffer->getImageAddress() == buffer->getBufferAddress(),
                      "This example requires tightly packed images at the registered buffer base");
                const auto* data = static_cast<const std::byte*>(buffer->getImageAddress());
                check(data != nullptr, "Provider delivered a null image address");
                snapshot_.header = header;
                snapshot_.pixels.assign(data, data + static_cast<std::size_t>(bytes));
            }
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Unexpected image callback failure";
        }

        // The example owns a copy after this point. Explicitly release every
        // delivered slot, including any extra frames racing the main thread.
        if (buffer) {
            try {
                if (buffer->release() != ir::Return::Success && error.empty()) {
                    error = "Could not release image buffer";
                }
            } catch (...) {
                error = "Buffer release threw an exception";
            }
        }
        {
            std::lock_guard lock(mutex_);
            if (!error.empty() && error_.empty()) { error_ = std::move(error); }
            complete_ = true;
        }
        cv_.notify_all();
    }

    Snapshot wait() {
        std::unique_lock lock(mutex_);
        check(cv_.wait_for(lock, 3s, [&] { return complete_; }), "Timed out waiting for a frame");
        check(error_.empty(), error_);
        return snapshot_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool complete_{};
    std::string error_;
    Snapshot snapshot_;
};

Snapshot capture(const char* library_path) {
    // Keep the DSO loaded until all MEL objects, callbacks, and deleters die.
    SharedLibrary library(library_path);
    const auto get_manager = library.symbol<decltype(&getAPI_Manager)>("getAPI_Manager");
    const auto get_control = library.symbol<decltype(&ir::getControl)>("getControl");
    const auto get_buffer = library.symbol<decltype(&ir::getBuffer)>("getBuffer");
    CaptureSession session;
    session.manager = get_manager("synthetic");
    check(session.manager != nullptr, "Could not create the synthetic provider instance");
    session.control = get_control("synthetic", session.manager);
    check(session.control != nullptr, "Could not create MEL Control");
    check(session.control->init("") == ir::Return::Success, "Provider initialization failed");

    const auto capabilities = session.control->getCapabilities();
    const auto selected = std::find_if(capabilities.begin(), capabilities.end(), [](const auto& capability) {
        const auto& types = capability.getChannelTypes();
        return std::find(types.begin(), types.end(), ir::ChannelType::IRSTImage) != types.end();
    });
    check(selected != capabilities.end(), "Provider did not advertise an image channel");
    check(selected->getFormat() == ir::PixelFormat::Mono && selected->getBitDepth() == 8 &&
              selected->getNumberOfBands() == 1 && selected->getRowPitch() == selected->getWidth() &&
              selected->getBufferSize() >= static_cast<std::uint64_t>(selected->getWidth()) * selected->getHeight(),
          "This example requires the tightly packed Mono8 profile");

    auto listener = std::make_shared<SnapshotListener>();
    ir::Config config;
    config.setChanID(selected->getChanID());
    config.setChannelType(ir::ChannelType::IRSTImage);
    config.setPlatform(selected->getPlatform());
    config.setSensorLocation(selected->getSensorLocation());
    config.setImgLstnr(listener);
    session.channel = session.control->attachChannel(config);
    check(session.channel != nullptr, "Could not attach image channel");

    session.storage.resize(4);
    for (std::size_t i = 0; i < session.storage.size(); ++i) {
        auto& memory = session.storage[i];
        memory.resize(selected->getBufferSize());
        auto buffer = get_buffer("synthetic", session.manager);
        check(buffer != nullptr, "Could not create MEL Buffer");
        check(buffer->init(memory.data(), memory.size(), static_cast<std::int64_t>(i)) == ir::Return::Success,
              "Buffer initialization failed");
        session.buffers.push_back(buffer);
        check(session.channel->registerBuffer(buffer) == ir::Return::Success, "Buffer registration failed");
    }
    check(session.channel->enable() == ir::Return::Success, "Could not start acquisition");
    auto snapshot = listener->wait();
    session.close();
    return snapshot; // Client-owned bytes and value metadata can outlive the DSO.
}

void write_pgm(const char* path, const Snapshot& snapshot) {
    std::ofstream output(path, std::ios::binary);
    check(output.is_open(), std::string("Cannot open output file: ") + path);
    output << "P5\n" << snapshot.header.getWidth() << ' ' << snapshot.header.getHeight() << "\n255\n";
    output.write(reinterpret_cast<const char*>(snapshot.pixels.data()),
                 static_cast<std::streamsize>(snapshot.pixels.size()));
    output.close();
    check(static_cast<bool>(output), "Writing the PGM file failed");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0] << " /path/to/libams_gige_mel.so [capture.pgm]\n";
        return 2;
    }
    try {
        const auto snapshot = capture(argv[1]);
        const char* output = argc == 3 ? argv[2] : "capture.pgm";
        write_pgm(output, snapshot);
        std::cout << "Wrote " << snapshot.header.getWidth() << 'x' << snapshot.header.getHeight()
                  << " Mono8 frame " << snapshot.header.getFrameID() << " to " << output << '\n'
                  << "Integration start: " << snapshot.header.getSystemTime().count()
                  << " ns (synthetic monotonic clock; no UTC epoch).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Capture failed: " << error.what() << '\n';
        return 1;
    }
}
