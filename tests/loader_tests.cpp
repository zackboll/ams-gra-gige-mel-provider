// This executable deliberately includes only the published MEL declarations.
// It must not link against the provider or include its implementation headers.
#include <irmel/library/irmel-types/Control.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>
#include "public_health_demo.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace ir = ams::iface::irmel;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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
            throw std::runtime_error(std::string("Missing factory ") + name + ": " + (error ? error : "null symbol"));
        }
        // POSIX permits conversion of dlsym's result to the function pointer.
        return reinterpret_cast<Function>(address);
    }

private:
    void* handle_{};
};

// Keep the caller-owned allocations until stop, unregister, detach, and object
// destruction have completed, including on exceptions and acquisition timeout.
struct Session final {
    std::vector<std::vector<std::byte>> storage;
    std::shared_ptr<API_Manager> manager;
    std::shared_ptr<ir::Control> control;
    std::vector<std::shared_ptr<ir::Buffer>> buffers;
    std::shared_ptr<ir::Channel> channel;

    ~Session() {
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

    void close_checked() {
        require(channel->disable() == ir::Return::Success, "disable failed");
        for (const auto& buffer : buffers) {
            require(channel->unregisterBuffer(buffer) == ir::Return::Success, "unregisterBuffer failed");
        }
        require(control->detachChannel(channel) == ir::Return::Success, "detachChannel failed");
        channel.reset();
        buffers.clear();
        control.reset();
        manager.reset();
    }
};

class CheckingListener final : public ir::ImageListener {
public:
    // Filled before enable(), then immutable while delivery is possible.
    std::vector<void*> registered_addresses;

    void onImage(const ir::Channel&, const ir::FrameHeader& header,
                 std::shared_ptr<ir::Buffer> buffer) override {
        std::string failure;
        try {
            require(buffer != nullptr, "callback supplied a null buffer");
            require(header.getWidth() == 64 && header.getHeight() == 48, "unexpected default frame dimensions");
            require(header.getBitsPerPixel() == 8 && header.getNumBands() == 1 &&
                        header.getFormat() == ir::PixelFormat::Mono,
                    "frame is not single-band Mono8");
            require(buffer->getSize() >= 64 * 48, "delivered buffer is too small");
            require(buffer->getImageAddress() == buffer->getBufferAddress(), "initial profile requires tightly packed base-address images");
            const auto found = std::find(registered_addresses.begin(), registered_addresses.end(), buffer->getBufferAddress());
            require(found != registered_addresses.end(), "callback did not use registered caller storage");
            const auto index = static_cast<std::size_t>(found - registered_addresses.begin());
            require(buffer->getContext() == static_cast<std::int64_t>(index), "buffer context was not preserved");
            const auto* pixels = static_cast<const std::byte*>(buffer->getImageAddress());
            require(pixels != nullptr, "null image address");
            for (std::uint32_t y = 0; y < 48; ++y) {
                for (std::uint32_t x = 0; x < 64; ++x) {
                    const auto expected = (static_cast<std::uint64_t>(x) + y + header.getFrameID()) & 0xffU;
                    require(std::to_integer<unsigned>(pixels[static_cast<std::size_t>(y) * 64 + x]) == expected,
                            "synthetic pixel pattern does not match frame metadata");
                }
            }
            require(header.getIntegrationTime().count() > 0, "missing integration time");
            require(header.getSystemTime().count() > 0, "missing synthetic integration-start timestamp");
            {
                std::lock_guard lock(mutex_);
                if (!headers_.empty()) {
                    require(header.getFrameID() > headers_.back().getFrameID(), "frame IDs did not advance");
                    require(header.getSystemTime() > headers_.back().getSystemTime(), "frame timestamps did not advance");
                }
                if (headers_.size() < 3) {
                    headers_.push_back(header); // Copy metadata; callback references are transient.
                }
            }
        } catch (const std::exception& error) {
            failure = error.what();
        } catch (...) {
            failure = "nonstandard exception in image validation";
        }

        // Always return the lease, including when validation fails. A shared_ptr
        // going out of scope is not a substitute for Buffer::release().
        if (buffer) {
            try {
                if (buffer->release() != ir::Return::Success && failure.empty()) {
                    failure = "Buffer::release failed";
                }
            } catch (...) {
                failure = "Buffer::release threw";
            }
        }
        {
            std::lock_guard lock(mutex_);
            if (!failure.empty() && error_.empty()) {
                error_ = std::move(failure);
            }
            if (error_.empty()) {
                ++completed_frames_;
            }
        }
        cv_.notify_all();
    }

    void wait_for_three_frames() {
        std::unique_lock lock(mutex_);
        require(cv_.wait_for(lock, 3s, [&] { return completed_frames_ >= 3 || !error_.empty(); }),
                "timed out waiting for three synthetic frames");
        require(error_.empty(), error_);
        require(headers_.size() == 3, "metadata snapshots were not retained");
    }

    void check_no_error() const {
        std::lock_guard lock(mutex_);
        require(error_.empty(), error_);
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t completed_frames_{};
    std::vector<ir::FrameHeader> headers_;
    std::string error_;
};

void run(const char* library_path, bool health_only, bool expect_absent) {
    // First declared, last destroyed: no MEL object or deleter may outlive DSO.
    SharedLibrary library(library_path);
    const auto manager_factory = library.symbol<decltype(&getAPI_Manager)>("getAPI_Manager");
    const auto control_factory = library.symbol<decltype(&ir::getControl)>("getControl");
    const auto buffer_factory = library.symbol<decltype(&ir::getBuffer)>("getBuffer");

    require(!manager_factory("unknown-backend"), "unknown instance unexpectedly created a manager");
    Session session;
    session.manager = manager_factory("synthetic");
    require(session.manager != nullptr, "synthetic manager factory returned null");
    require(!control_factory("unknown-backend", session.manager), "control accepted a mismatched instance");
    require(!buffer_factory("unknown-backend", session.manager), "buffer accepted a mismatched instance");
    require(!control_factory("synthetic", nullptr), "control accepted a null manager");
    session.control = control_factory("synthetic", session.manager);
    require(session.control != nullptr, "control factory returned null");
    require(session.control->init("") == ir::Return::Success, "Control::init failed");
    demonstrate_health(session.control, expect_absent);
    if (health_only) return;

    const auto capabilities = session.control->getCapabilities();
    const auto selected = std::find_if(capabilities.begin(), capabilities.end(), [](const auto& capability) {
        const auto& types = capability.getChannelTypes();
        return std::find(types.begin(), types.end(), ir::ChannelType::IRSTImage) != types.end();
    });
    require(selected != capabilities.end(), "no advertised image channel");
    require(selected->getWidth() == 64 && selected->getHeight() == 48 &&
                selected->getRowPitch() == 64 && selected->getBitDepth() == 8 &&
                selected->getNumberOfBands() == 1 && selected->getFormat() == ir::PixelFormat::Mono &&
                selected->getImageSize() == 3072 && selected->getBufferSize() == 3072,
            "default capability does not describe tightly packed 64x48 Mono8");

    auto listener = std::make_shared<CheckingListener>();
    ir::Config config;
    config.setChanID(selected->getChanID());
    config.setChannelType(ir::ChannelType::IRSTImage);
    config.setPlatform(selected->getPlatform());
    config.setSensorLocation(selected->getSensorLocation());
    config.setImgLstnr(listener);
    session.channel = session.control->attachChannel(config);
    require(session.channel != nullptr, "attachChannel failed");

    session.storage.resize(4);
    for (std::size_t i = 0; i < session.storage.size(); ++i) {
        auto& memory = session.storage[i];
        memory.resize(selected->getBufferSize());
        auto buffer = buffer_factory("synthetic", session.manager);
        require(buffer != nullptr, "buffer factory returned null");
        require(buffer->init(memory.data(), memory.size(), static_cast<std::int64_t>(i)) == ir::Return::Success,
                "Buffer::init failed");
        session.buffers.push_back(buffer);
        require(session.channel->registerBuffer(buffer) == ir::Return::Success, "registerBuffer failed");
        listener->registered_addresses.push_back(memory.data());
    }
    require(session.channel->enable() == ir::Return::Success, "enable failed");
    listener->wait_for_three_frames();
    session.close_checked();
    listener->check_no_error();
    // config/listener contain only client-owned state now; they are destroyed
    // before library at scope exit, alongside all other C++ MEL values.
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0] << " /path/to/libams_gige_mel.so\n";
        return 2;
    }
    try {
        const std::string mode = argc == 3 ? argv[2] : "";
        run(argv[1], !mode.empty(), mode == "--expect-no-health");
        std::cout << (mode == "--expect-no-health" ? "PASS: baseline Health absent and not attachable\n" :
            mode == "--health-only" ? "PASS: public MEL Health discovery, attachment and both current reports\n" :
            "PASS: native factory loading, Health snapshots, registered Mono8 delivery, and teardown\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
