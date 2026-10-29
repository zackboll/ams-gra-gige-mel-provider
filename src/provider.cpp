#include <gige_mel/provider.hpp>
#include "provider_internal.hpp"

#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace gige_mel {
namespace {
namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
using namespace std::chrono_literals;

template <class T> mel::RequestFor<T> ready(T value) {
    std::promise<mel::ErrorOr<std::shared_ptr<T>>> promise;
    auto result = promise.get_future();
    promise.set_value(mel::ErrorOr<std::shared_ptr<T>>(std::make_shared<T>(std::move(value))));
    return result;
}

template <class T> mel::RequestFor<T> failed(mel::ErrorCode code, std::string message) {
    std::promise<mel::ErrorOr<std::shared_ptr<T>>> promise;
    auto result = promise.get_future();
    promise.set_value(mel::ErrorOr<std::shared_ptr<T>>(mel::Error(code, std::move(message))));
    return result;
}

mel::ErrorCode map_error(backend::ErrorCode code) {
    using B = backend::ErrorCode;
    switch (code) {
    case B::invalid_argument: return mel::ErrorCode::InvalidParameters;
    case B::not_found: return mel::ErrorCode::InvalidId;
    case B::unsupported: return mel::ErrorCode::Unsupported;
    case B::resource_exhausted: return mel::ErrorCode::InsufficientLocalResources;
    case B::disconnected: case B::io_error: return mel::ErrorCode::InsufficientRemoteResources;
    case B::timeout: case B::busy: case B::stopped: case B::invalid_state:
        return mel::ErrorCode::InvalidState;
    }
    return mel::ErrorCode::InvalidState;
}

class ChannelCore;
class ControlImpl;

struct Runtime {
    ProviderSettings settings;
    std::shared_ptr<backend::ICameraBackend> backend;
    std::shared_ptr<backend::ICameraDevice> device;
    std::shared_ptr<const int> affinity{std::make_shared<const int>(0)};
    // Mutations serialize here. A worker never takes this lock. Callbacks run
    // outside it; their reentrant mutations are rejected before acquiring it.
    std::mutex operations;
    mutable std::mutex state_mutex;
    backend::CameraConfiguration configuration;
    backend::CameraCapabilities camera_capabilities;
    bool initialized{false};
    bool configuration_valid{false};
    bool image_active{false};
    std::uint64_t configuration_generation{1};
    std::atomic<std::weak_ptr<ControlImpl>> control;
    std::weak_ptr<ChannelCore> image_channel; // guarded by operations
    std::atomic<std::uint64_t> delivered{0}, dropped{0}, rejected{0}, callback_failures{0}, stream_failures{0};
    std::string last_error;

    ~Runtime() {
        if (device) {
            try { (void)device->close(); } catch (...) { /* destructors must not throw */ }
        }
    }

    void report(std::string message) {
        std::lock_guard lock(state_mutex);
        last_error = std::move(message);
    }
};

// The guard also covers synchronous metadata callbacks. A nested callback
// restores the previous guard, rather than clearing another callback's state.
thread_local const Runtime* callback_runtime = nullptr;
struct CallbackGuard {
    const Runtime* previous;
    explicit CallbackGuard(const Runtime* runtime) : previous(callback_runtime) { callback_runtime = runtime; }
    ~CallbackGuard() { callback_runtime = previous; }
};

bool valid_layout(const backend::CameraConfiguration& c, const backend::CameraCapabilities& caps) {
    return caps.mono8 && c.width > 0 && c.height > 0 && c.width <= caps.sensor_width && c.height <= caps.sensor_height &&
        c.offset_x <= caps.sensor_width - c.width && c.offset_y <= caps.sensor_height - c.height &&
        static_cast<std::uint64_t>(c.width) * c.height <= std::numeric_limits<std::uint32_t>::max();
}

ir::ChannelCapability capability(const Runtime& runtime, ir::ChannelType type,
                                 const mel::UCI_ID& id, const ir::Config* config = nullptr) {
    std::lock_guard lock(runtime.state_mutex);
    const auto& c = runtime.configuration;
    ir::ChannelCapability cap;
    cap.setChanID(id);
    cap.setWidth(c.width);
    cap.setHeight(c.height);
    cap.setBitDepth(8); // significant bits; FrameHeader separately reports storage bits
    cap.setRowPitch(c.width);
    cap.setBufferSize(c.width * c.height);
    cap.setImageSize(c.width * c.height);
    cap.setNumberOfBands(1);
    cap.setFormat(ir::PixelFormat::Mono);
    cap.setChannelTypes({type});
    std::set<ir::ChannelMetadataCapabilityType> metadata{ir::ChannelMetadataCapabilityType::ChannelCommsTestRep};
    if (type == ir::ChannelType::IRSTImage) metadata.insert(ir::ChannelMetadataCapabilityType::CameraCommandResp);
    cap.setChannelMetadataCapabilities(metadata);
    if (config) {
        cap.setPlatform(config->getPlatform());
        cap.setSensorLocation(config->getSensorLocation());
    }
    // No navigation, NUC, distortion or tracking claims. TaskScheduleDepth's
    // default zero is not a conformance claim: see the explicitly partial profile.
    return cap;
}

class HostBuffer final : public ir::Buffer {
public:
    explicit HostBuffer(std::shared_ptr<const int> affinity) : affinity_(std::move(affinity)) {}

    ir::Return init(void* address, std::size_t size, std::int64_t context) override {
        std::lock_guard lock(mutex_);
        if (owner_ || state_ != State::available) return ir::Return::Fail;
        if (!address || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            return ir::Return::BadPointer;
        const auto begin = reinterpret_cast<std::uintptr_t>(address);
        if (size > std::numeric_limits<std::uintptr_t>::max() - begin) return ir::Return::BadPointer;
        address_ = address;
        size_ = size;
        context_ = context;
        return ir::Return::Success;
    }
    void* getBufferAddress() const override { std::lock_guard lock(mutex_); return address_; }
    void* getImageAddress() const override { return getBufferAddress(); }
    std::int64_t getSize() const override { std::lock_guard lock(mutex_); return static_cast<std::int64_t>(size_); }
    std::int64_t getContext() const override { std::lock_guard lock(mutex_); return context_; }

    ir::Return release() override {
        std::lock_guard lock(mutex_);
        if (state_ != State::delivered) return ir::Return::Fail;
        state_ = State::available;
        return ir::Return::Success;
    }
    ir::Return getFlags(std::vector<ir::BufferFlag>& flags) const override {
        std::lock_guard lock(mutex_);
        flags = flags_;
        return ir::Return::Success;
    }
    void addFlag(ir::BufferFlag flag) override {
        std::lock_guard lock(mutex_);
        if (!owner_ && state_ == State::available && std::find(flags_.begin(), flags_.end(), flag) == flags_.end())
            flags_.push_back(flag);
    }
    void setFlags(const std::vector<ir::BufferFlag>& flags) override {
        std::lock_guard lock(mutex_);
        if (!owner_ && state_ == State::available) flags_ = flags;
    }

    ir::Return bind(const void* owner, const std::shared_ptr<const int>& affinity, std::size_t minimum_size) {
        std::lock_guard lock(mutex_);
        if (affinity != affinity_) return ir::Return::BadPointer;
        if (!address_ || size_ < minimum_size) return ir::Return::BadPointer;
        if (owner_ || state_ != State::available) return ir::Return::Fail;
        if (flags_.size() != 1 || flags_[0] != ir::BufferFlag::Host) return ir::Return::NotSupported;
        owner_ = owner;
        return ir::Return::Success;
    }
    void unbind() { std::lock_guard lock(mutex_); owner_ = nullptr; }
    bool outstanding() const { std::lock_guard lock(mutex_); return state_ != State::available; }
    bool acquire(std::size_t minimum_size) {
        std::lock_guard lock(mutex_);
        if (!owner_ || state_ != State::available || size_ < minimum_size) return false;
        state_ = State::filling;
        return true;
    }
    void publish() { std::lock_guard lock(mutex_); state_ = State::delivered; }
    void cancel_fill() { std::lock_guard lock(mutex_); if (state_ == State::filling) state_ = State::available; }

private:
    enum class State { available, filling, delivered };
    std::shared_ptr<const int> affinity_;
    mutable std::mutex mutex_;
    void* address_{nullptr};
    std::size_t size_{0};
    std::int64_t context_{0};
    const void* owner_{nullptr};
    State state_{State::available};
    std::vector<ir::BufferFlag> flags_{ir::BufferFlag::Host};
};

struct StreamState {
    std::shared_ptr<Runtime> runtime;
    std::unique_ptr<backend::IFrameStream> stream;
    std::shared_ptr<ir::ImageListener> listener;
    std::vector<std::shared_ptr<HostBuffer>> buffers;
    backend::CameraConfiguration configuration;
    std::uint64_t generation{};
    mel::ComponentLocation location;
    std::atomic<bool> stopping{false};
    std::atomic<bool> failed{false};
};

class ChannelCore : public virtual ir::Channel, public std::enable_shared_from_this<ChannelCore> {
public:
    ChannelCore(std::shared_ptr<Runtime> runtime, ir::Config config)
        : runtime_(std::move(runtime)), config_(std::move(config)) {}
    ~ChannelCore() override {
        // Public detach/disable normally joins first. If a consumer destroys its
        // control graph from a callback, state captured by the thread still owns
        // every object it needs; never join the current thread.
        stop_locked();
        for (auto& buffer : buffers_) buffer->unbind();
    }

    ir::ChannelType type() const { return config_.getChannelType(); }
    bool is_detached() const { return detached_.load(); }

    mel::RequestFor<ir::Return> sendKeepAliveRep() override {
        if (is_detached()) return failed<ir::Return>(mel::ErrorCode::InvalidState, "channel is detached");
        return ready(ir::Return::Success);
    }
    mel::RequestFor<ir::ChannelCommsTestRep> send(ir::ChannelCommsTestReq request) override {
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin || is_detached()) return failed<ir::ChannelCommsTestRep>(mel::ErrorCode::InvalidState, "channel is detached");
        const auto self = shared_from_this();
        ir::ChannelCommsTestRep reply(request.getCommandID(), request.getRequestID());
        auto result = ready(reply);
        std::function<void(ir::Channel&, const ir::ChannelCommsTestRep*)> callback;
        { std::lock_guard lock(callback_mutex_); callback = comms_callback_; }
        invoke(callback, reply);
        return result;
    }

    ir::Return registerBuffer(std::shared_ptr<ir::Buffer> buffer) override {
        if (callback_runtime == runtime_.get()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached() || enabled_) return ir::Return::Fail;
        if (type() != ir::ChannelType::IRSTImage) return ir::Return::NotSupported;
        auto host = std::dynamic_pointer_cast<HostBuffer>(buffer);
        if (!host) return ir::Return::BadPointer;
        if (buffers_.size() >= runtime_->settings.maximum_registered_buffers) return ir::Return::Fail;
        const auto minimum_size = static_cast<std::size_t>(getCapabilities().getBufferSize());
        const auto status = host->bind(this, runtime_->affinity, minimum_size);
        if (status != ir::Return::Success) return status;
        const auto start = reinterpret_cast<std::uintptr_t>(host->getBufferAddress());
        const auto end = start + static_cast<std::size_t>(host->getSize());
        for (const auto& registered : buffers_) {
            const auto other = reinterpret_cast<std::uintptr_t>(registered->getBufferAddress());
            const auto other_end = other + static_cast<std::size_t>(registered->getSize());
            if (start < other_end && other < end) { host->unbind(); return ir::Return::Fail; }
        }
        try { buffers_.push_back(host); }
        catch (...) { host->unbind(); throw; }
        return ir::Return::Success;
    }
    ir::Return unregisterBuffer(std::shared_ptr<ir::Buffer> buffer) override {
        if (callback_runtime == runtime_.get()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached() || enabled_) return ir::Return::Fail;
        const auto found = std::find(buffers_.begin(), buffers_.end(), buffer);
        if (found == buffers_.end()) return ir::Return::BadPointer;
        if ((*found)->outstanding()) return ir::Return::Fail;
        (*found)->unbind();
        buffers_.erase(found);
        return ir::Return::Success;
    }
    ir::Return enable() override {
        if (callback_runtime == runtime_.get()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        if (enabled_) return stream_state_ && stream_state_->failed ? ir::Return::Fail : ir::Return::Success;
        if (type() != ir::ChannelType::IRSTImage) { enabled_ = true; return ir::Return::Success; }
        if (buffers_.empty() || !config_.getImgLstnr()) return ir::Return::Fail;

        auto state = std::make_shared<StreamState>();
        state->runtime = runtime_;
        state->buffers = buffers_;
        state->listener = config_.getImgLstnr();
        state->location = config_.getSensorLocation();
        {
            std::lock_guard lock(runtime_->state_mutex);
            if (!runtime_->configuration_valid || runtime_->image_active) return ir::Return::Fail;
            state->configuration = runtime_->configuration;
            state->generation = runtime_->configuration_generation;
        }
        const auto bytes = static_cast<std::size_t>(state->configuration.width) * state->configuration.height;
        for (const auto& buffer : buffers_) {
            if (buffer->outstanding() || static_cast<std::size_t>(buffer->getSize()) < bytes) return ir::Return::Fail;
        }
        try {
            auto created = runtime_->device->create_stream();
            if (!created) { runtime_->report(created.error().message); return ir::Return::Fail; }
            state->stream = std::move(created).value();
            auto started = state->stream->start(state->generation);
            if (!started) { runtime_->report(started.error().message); return ir::Return::Fail; }
            {
                std::lock_guard lock(runtime_->state_mutex);
                runtime_->image_active = true;
            }
            stream_state_ = state;
            enabled_ = true;
            worker_ = std::thread([state, weak = weak_from_this()] { run(state, weak); });
            return ir::Return::Success;
        } catch (const std::exception& error) {
            state->stopping = true;
            if (state->stream) state->stream->stop();
            stream_state_.reset();
            enabled_ = false;
            { std::lock_guard lock(runtime_->state_mutex); runtime_->image_active = false; }
            runtime_->report(error.what());
            return ir::Return::Fail;
        }
    }
    ir::Return disable() override {
        if (callback_runtime == runtime_.get()) return ir::Return::Fail;
        // Pin before taking operations: a callback dropping the last external
        // Control must not run its destructor while this thread joins it.
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        stop_locked();
        return ir::Return::Success;
    }
    ir::ChannelCapability getCapabilities() const override {
        return capability(*runtime_, type(), config_.getChanID(), &config_);
    }
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::ChannelCommsTestRep* const)> callback) override {
        return register_callback(comms_callback_, std::move(callback));
    }

    void detach_locked() {
        detached_ = true;
        stop_locked();
        if (callback_runtime != runtime_.get()) {
            std::unique_lock lock(callback_mutex_);
            callback_cv_.wait(lock, [this] { return active_metadata_callbacks_ == 0; });
        }
        for (auto& buffer : buffers_) buffer->unbind();
        buffers_.clear();
    }

protected:
    template <class T> using Callback = std::function<void(ir::Channel&, const T*)>;

    template <class T> ir::Return register_callback(Callback<T>& slot, Callback<T> callback) {
        if (callback_runtime == runtime_.get() || is_detached()) return ir::Return::Fail;
        if (!callback) return ir::Return::BadPointer;
        std::lock_guard lock(callback_mutex_);
        if (is_detached()) return ir::Return::Fail;
        if (slot) return ir::Return::Fail;
        slot = std::move(callback);
        return ir::Return::Success;
    }
    template <class T> void invoke(const Callback<T>& callback, const T& message) {
        if (!callback) return;
        {
            std::lock_guard lock(callback_mutex_);
            if (is_detached()) return;
            ++active_metadata_callbacks_;
        }
        CallbackGuard guard(runtime_.get());
        try { callback(*this, &message); }
        catch (...) {
            ++runtime_->callback_failures;
            try { runtime_->report("metadata callback threw; response remains available through its future"); } catch (...) {}
        }
        { std::lock_guard lock(callback_mutex_); --active_metadata_callbacks_; }
        callback_cv_.notify_all();
    }
    template <class T> mel::RequestFor<T> unsupported(const char* name) {
        return failed<T>(is_detached() ? mel::ErrorCode::InvalidState : mel::ErrorCode::Unsupported,
                         std::string(name) + ": not available in the experimental image-stream profile");
    }

    mel::RequestFor<ir::CameraCommandResp> camera_command(const ir::CameraCommand& command) {
        if (callback_runtime == runtime_.get())
            return failed<ir::CameraCommandResp>(mel::ErrorCode::InvalidState, "schedule camera changes outside callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::CameraCommandResp>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        std::shared_ptr<ChannelCore> image_recipient;
        ir::CameraCommandResp reply;
        reply.setCommandID(command.getCommandID());
        // size is left at zero: this C++ object has no specified serialized wire
        // encoding. sizeof(CameraCommandResp) would not be a portable wire size.
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<ir::CameraCommandResp>(mel::ErrorCode::InvalidState, "channel is detached");
            image_recipient = runtime_->image_channel.lock();
            {
                std::lock_guard lock(runtime_->state_mutex);
                if (runtime_->image_active)
                    return failed<ir::CameraCommandResp>(mel::ErrorCode::InvalidState, "disable image acquisition before configuring");
            }
            backend::CameraConfiguration requested;
            requested.width = command.getWidth();
            requested.height = command.getHeight();
            requested.offset_x = command.getColumnOffset();
            requested.offset_y = command.getRowOffset();
            requested.frame_rate_hz = command.getFrameRate();
            requested.exposure = command.getIntegrationTime();
            if (command.getImageType() != ir::ImageType::Staring || command.getNumberOfStages() != 0 ||
                !valid_layout(requested, runtime_->camera_capabilities)) {
                runtime_->report("CameraCommand cannot be satisfied: requires a complete, valid Mono8 staring configuration");
            } else {
                auto applied = runtime_->device->apply(requested);
                // Query after every attempted apply. A future hardware adapter
                // may have partially changed the device before reporting error.
                auto actual = runtime_->device->configuration();
                {
                    std::lock_guard lock(runtime_->state_mutex);
                    ++runtime_->configuration_generation;
                    runtime_->configuration_valid = actual && valid_layout(actual.value(), runtime_->camera_capabilities);
                    if (runtime_->configuration_valid) runtime_->configuration = actual.value();
                }
                if (!actual) {
                    runtime_->report(actual.error().message);
                    return failed<ir::CameraCommandResp>(map_error(actual.error().code), actual.error().message);
                }
                if (!applied) {
                    runtime_->report(applied.error().message);
                    if (applied.error().code != backend::ErrorCode::invalid_argument && applied.error().code != backend::ErrorCode::unsupported)
                        return failed<ir::CameraCommandResp>(map_error(applied.error().code), applied.error().message);
                } else {
                    reply.setCommandSuccess(actual.value() == requested);
                    if (!reply.getCommandSuccess()) runtime_->report("camera readback differs from requested configuration");
                }
            }
        }
        reply.setTimestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()));
        auto result = ready(reply);
        // Camera responses are an Image callback even when C2 submitted the
        // command. No operation/state locks are held across user code.
        if (image_recipient) image_recipient->on_camera_reply(reply);
        return result;
    }

    virtual void on_camera_reply(const ir::CameraCommandResp&) {}
    std::shared_ptr<Runtime> runtime_;
    ir::Config config_;
    std::mutex callback_mutex_;

private:
    void stop_locked() {
        const bool owned_stream = static_cast<bool>(stream_state_);
        if (stream_state_) {
            stream_state_->stopping = true;
            stream_state_->stream->request_stop();
        }
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) worker_.detach();
            else worker_.join();
        }
        if (stream_state_) stream_state_->stream->stop();
        stream_state_.reset();
        if (owned_stream) {
            std::lock_guard lock(runtime_->state_mutex);
            runtime_->image_active = false;
        }
        enabled_ = false;
    }

    static void run(const std::shared_ptr<StreamState>& state, const std::weak_ptr<ChannelCore>& weak) noexcept {
        try {
            std::optional<std::uint64_t> stream_generation;
            std::optional<std::uint64_t> clock_generation;
            std::optional<std::chrono::nanoseconds> last_timestamp;
            while (!state->stopping) {
                auto result = state->stream->receive(50ms);
                if (!result) {
                    if (result.error().code == backend::ErrorCode::timeout) continue;
                    if (state->stopping || result.error().code == backend::ErrorCode::stopped) break;
                    ++state->runtime->stream_failures;
                    state->failed = true;
                    state->runtime->report(result.error().message);
                    break;
                }
                auto frame = std::move(result).value();
                if (state->stopping) break;
                const auto& d = frame.descriptor();
                const auto& c = state->configuration;
                const bool layout_matches = d.width == c.width && d.height == c.height && d.configuration == c &&
                    d.configuration_generation == state->generation && (!stream_generation || d.stream_generation == *stream_generation) &&
                    d.pixel_format == backend::PixelFormat::mono8 && d.storage_bits_per_pixel == 8 &&
                    d.significant_bits_per_pixel == 8 && d.bands == 1 && d.row_stride_bytes >= d.width &&
                    d.height > 0 && d.row_stride_bytes <= std::numeric_limits<std::size_t>::max() / d.height;
                const bool timestamp_matches = d.timestamp.valid && d.timestamp.domain == state->runtime->settings.timestamp_domain &&
                    d.timestamp.event == backend::TimestampEvent::integration_start;
                if (!layout_matches || !timestamp_matches || d.status != backend::FrameStatus::complete ||
                    frame.bytes().size() < d.row_stride_bytes * d.height) {
                    ++state->runtime->rejected;
                    state->runtime->report("rejected incomplete frame or mismatched layout, generation, or timestamp contract");
                    continue;
                }
                stream_generation = d.stream_generation;
                if ((clock_generation && *clock_generation != d.timestamp.clock_generation) ||
                    (last_timestamp && d.timestamp.value < *last_timestamp)) {
                    ++state->runtime->stream_failures;
                    state->failed = true;
                    state->runtime->report("timestamp epoch changed or clock regressed; disable and resolve the time mapping before restart");
                    break;
                }
                clock_generation = d.timestamp.clock_generation;
                last_timestamp = d.timestamp.value;
                const auto bytes = static_cast<std::size_t>(d.width) * d.height;
                std::shared_ptr<HostBuffer> target;
                for (const auto& buffer : state->buffers) if (buffer->acquire(bytes)) { target = buffer; break; }
                if (!target) { ++state->runtime->dropped; continue; }
                struct FillGuard {
                    std::shared_ptr<HostBuffer> buffer;
                    ~FillGuard() { buffer->cancel_fill(); }
                } fill_guard{target};
                auto* destination = static_cast<std::byte*>(target->getBufferAddress());
                for (std::size_t row = 0; row < d.height; ++row)
                    std::memcpy(destination + row * d.width, frame.bytes().data() + row * d.row_stride_bytes, d.width);

                ir::FrameHeader header;
                header.setSystemTime(d.timestamp.value);
                header.setIntegrationTime(c.exposure);
                header.setWidth(d.width);
                header.setHeight(d.height);
                header.setBitsPerPixel(8);
                header.setNumBands(1);
                header.setFormat(ir::PixelFormat::Mono);
                header.setFrameID(static_cast<std::uint32_t>(d.native_frame_id & 0xffffffffULL));
                header.setSubframeID(0);
                header.setSubframeTotal(1);
                header.setImageType(ir::ImageType::Staring);
                header.setImageFlip(ir::ImageFlip::None);
                header.setFlags({ir::ImageFlag::StareSnapshot});
                header.setColumnOffset(c.offset_x);
                header.setRowOffset(c.offset_y);
                header.setHorizontalFieldOfView(d.width * state->runtime->settings.horizontal_radians_per_pixel);
                header.setVerticalFieldOfView(d.height * state->runtime->settings.vertical_radians_per_pixel);
                header.setFace({state->location, 0});
                const auto control_pin = state->runtime->control.load().lock();
                const auto channel = weak.lock();
                if (!control_pin || !channel || channel->is_detached() || state->stopping) break;
                target->publish();
                ++state->runtime->delivered;
                CallbackGuard guard(state->runtime.get());
                try { state->listener->onImage(*channel, header, target); }
                catch (...) {
                    // The callback may have retained the buffer before throwing.
                    // Never auto-release it and overwrite a live consumer lease.
                    ++state->runtime->callback_failures;
                    state->failed = true;
                    state->runtime->report("image callback threw; acquisition stopped, delivered buffer still requires release");
                    state->stopping = true;
                    state->stream->request_stop();
                }
            }
        } catch (const std::exception& error) {
            ++state->runtime->stream_failures;
            state->failed = true;
            try { state->runtime->report(error.what()); } catch (...) {}
        } catch (...) {
            ++state->runtime->stream_failures;
            state->failed = true;
        }
        state->stream->stop();
    }

    std::atomic<bool> detached_{false};
    bool enabled_{false}; // guarded by Runtime::operations
    Callback<ir::ChannelCommsTestRep> comms_callback_;
    std::condition_variable callback_cv_;
    std::size_t active_metadata_callbacks_{0};
    std::vector<std::shared_ptr<HostBuffer>> buffers_;
    std::shared_ptr<StreamState> stream_state_;
    std::thread worker_;
};

// Every upstream pure virtual is implemented. Unsupported surfaces return the
// real MEL errors, never fabricated success or an unfulfilled promise.
#define GIGE_UNSUPPORTED_SEND(Request, Response) \
    mel::RequestFor<Response> send(Request) override { return unsupported<Response>(#Request); }
#define GIGE_UNSUPPORTED_CALLBACK(Message) \
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const Message* const)>) override { \
        return ir::Return::NotSupported; \
    }

class ImageChannelImpl final : public ChannelCore, public ir::ImageChannel {
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::send;
    using ChannelCore::registerMetadataCallback;
    mel::RequestFor<ir::CameraCommandResp> send(ir::CameraCommand command) override { return camera_command(command); }
    GIGE_UNSUPPORTED_SEND(mel::NavigationReport, ir::NavigationReportResp)
    GIGE_UNSUPPORTED_CALLBACK(ir::BadPixelList)
    GIGE_UNSUPPORTED_CALLBACK(ir::OpticalDistortionMap)
    GIGE_UNSUPPORTED_CALLBACK(ir::LineOfSightReport)
    GIGE_UNSUPPORTED_CALLBACK(ir::LineOfSightQuaternion)
    GIGE_UNSUPPORTED_CALLBACK(ir::LineOfSightEuler)
    GIGE_UNSUPPORTED_CALLBACK(ir::NavigationReportResp)
    GIGE_UNSUPPORTED_CALLBACK(ir::CandidateObjectMessage)
    GIGE_UNSUPPORTED_CALLBACK(ir::CandidateObjectPreProcMessage)
    GIGE_UNSUPPORTED_CALLBACK(ir::NUC_TempData)
    ir::Return registerMetadataCallback(const std::function<void(ir::Channel&, const ir::LOS3D_KinematicsType* const)>&) override {
        return ir::Return::NotSupported;
    }
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::CameraCommandResp* const)> callback) override {
        return register_callback(camera_callback_, std::move(callback));
    }

private:
    void on_camera_reply(const ir::CameraCommandResp& reply) override {
        Callback<ir::CameraCommandResp> callback;
        { std::lock_guard lock(callback_mutex_); callback = camera_callback_; }
        invoke(callback, reply);
    }
    Callback<ir::CameraCommandResp> camera_callback_;
};

class C2ChannelImpl final : public ChannelCore, public ir::C2Channel {
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::send;
    using ChannelCore::registerMetadataCallback;
    mel::RequestFor<ir::CameraCommandResp> send(ir::CameraCommand command) override { return camera_command(command); }
    GIGE_UNSUPPORTED_SEND(ir::BIT_Command, ir::Return)
    GIGE_UNSUPPORTED_SEND(ir::CalibrationConfigurationCmd, mel::CalibrationConfiguration)
    GIGE_UNSUPPORTED_SEND(ir::CalibrationStatusCmd, mel::CalibrationStatus)
    GIGE_UNSUPPORTED_SEND(ir::CameraProtectCmd, ir::CameraProtectCmdResp)
    GIGE_UNSUPPORTED_SEND(ir::EraseCommand, ir::EraseCommandType)
    GIGE_UNSUPPORTED_SEND(ir::SystemTrackDataResponse, ir::CommandStatus)
    GIGE_UNSUPPORTED_SEND(ir::ModeCmd, ir::MFA_Mode)
    GIGE_UNSUPPORTED_SEND(ir::ConfigSetCommand, ir::Return)
    GIGE_UNSUPPORTED_CALLBACK(mel::BIT_Configuration)
    GIGE_UNSUPPORTED_CALLBACK(ir::CommandStatus)
    GIGE_UNSUPPORTED_CALLBACK(mel::CalibrationConfiguration)
    GIGE_UNSUPPORTED_CALLBACK(ir::CameraProtectCmdResp)
    GIGE_UNSUPPORTED_CALLBACK(mel::CalibrationStatus)
    GIGE_UNSUPPORTED_CALLBACK(mel::BIT_Status)
};
#undef GIGE_UNSUPPORTED_SEND
#undef GIGE_UNSUPPORTED_CALLBACK

class ControlImpl final : public ir::Control {
public:
    explicit ControlImpl(std::shared_ptr<Runtime> runtime) : runtime_(std::move(runtime)) {}
    ~ControlImpl() override {
        std::lock_guard operation(runtime_->operations);
        for (const auto& channel : channels_) channel->detach_locked();
    }

    ir::Return init(const std::string& aperture_config_id) override {
        if (callback_runtime == runtime_.get()) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (!aperture_config_id.empty() && aperture_config_id != "mono8-bootstrap") return ir::Return::NotSupported;
        try {
            if (!runtime_->initialized) {
                auto opened = runtime_->backend->open(runtime_->settings.device_id);
                if (!opened) { runtime_->report(opened.error().message); return ir::Return::Fail; }
                auto device = std::move(opened).value();
                auto actual = device->configuration();
                const auto caps = device->capabilities();
                if (!actual || !valid_layout(actual.value(), caps)) {
                    (void)device->close();
                    runtime_->report("backend initial configuration does not satisfy the Mono8 profile");
                    return ir::Return::Fail;
                }
                {
                    std::lock_guard lock(runtime_->state_mutex);
                    runtime_->device = std::move(device);
                    runtime_->configuration = actual.value();
                    runtime_->camera_capabilities = caps;
                    runtime_->configuration_valid = true;
                    runtime_->initialized = true;
                }
            }
            if (capabilities_.empty()) {
                mel::UCI_ID image_id, c2_id;
                std::array<std::uint8_t, mel::UUID_SIZE> image_uuid{}, c2_uuid{};
                image_uuid.back() = 1;
                c2_uuid.back() = 2;
                image_id.setUUID(image_uuid);
                c2_id.setUUID(c2_uuid);
                image_id.setDescriptiveLabel(runtime_->settings.instance + "/image");
                c2_id.setDescriptiveLabel(runtime_->settings.instance + "/c2");
                capabilities_.push_back(capability(*runtime_, ir::ChannelType::IRSTImage, image_id));
                capabilities_.push_back(capability(*runtime_, ir::ChannelType::CommandAndControl, c2_id));
            }
            return ir::Return::Success;
        } catch (const std::exception& error) {
            runtime_->report(error.what());
            return ir::Return::Fail;
        }
    }
    const std::vector<ir::ChannelCapability>& getCapabilities() const override { return capabilities_; }
    mel::VersionInfo getVersionInfo() const override {
        return mel::VersionInfo(4, 1, "ams-gra-gige-mel-provider",
            "0.1.0 experimental Mono8 image-stream profile against pinned MEL 4.0; not full MEL conformance");
    }
    std::shared_ptr<ir::Channel> attachChannel(const ir::Config& config) override {
        if (callback_runtime == runtime_.get()) return {};
        std::lock_guard operation(runtime_->operations);
        if (!runtime_->initialized) return {};
        const auto type = config.getChannelType();
        if (type != ir::ChannelType::IRSTImage && type != ir::ChannelType::CommandAndControl) return {};
        const auto found = std::find_if(capabilities_.begin(), capabilities_.end(), [&](const auto& cap) {
            return cap.getChanID().getUUID() == config.getChanID().getUUID() && cap.getChannelTypes().front() == type;
        });
        if (found == capabilities_.end()) return {};
        if (std::any_of(channels_.begin(), channels_.end(), [type](const auto& channel) { return channel->type() == type; })) return {};
        if (type == ir::ChannelType::IRSTImage && !config.getImgLstnr()) return {};
        std::shared_ptr<ChannelCore> channel;
        if (type == ir::ChannelType::IRSTImage) channel = std::make_shared<ImageChannelImpl>(runtime_, config);
        else channel = std::make_shared<C2ChannelImpl>(runtime_, config);
        channels_.push_back(channel);
        if (type == ir::ChannelType::IRSTImage) runtime_->image_channel = channel;
        return channel;
    }
    ir::Return detachChannel(std::shared_ptr<ir::Channel> channel) override {
        if (callback_runtime == runtime_.get()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        const auto found = std::find(channels_.begin(), channels_.end(), channel);
        if (found == channels_.end()) return ir::Return::BadPointer;
        (*found)->detach_locked();
        channels_.erase(found);
        return ir::Return::Success;
    }

private:
    std::shared_ptr<Runtime> runtime_;
    // An immutable discovery snapshot once init returns. Per-channel capability
    // queries report current configuration, avoiding races through const& data.
    std::vector<ir::ChannelCapability> capabilities_;
    std::vector<std::shared_ptr<ChannelCore>> channels_;
};

class Manager final : public API_Manager {
public:
    explicit Manager(std::shared_ptr<Runtime> value) : runtime(std::move(value)) {}
    std::shared_ptr<Runtime> runtime;
};

} // namespace

std::shared_ptr<API_Manager> make_api_manager(ProviderSettings settings, std::shared_ptr<backend::ICameraBackend> backend) {
    if (!backend || settings.instance.empty() || settings.device_id.empty() || settings.maximum_registered_buffers == 0 ||
        settings.maximum_registered_buffers > 256 || !std::isfinite(settings.horizontal_radians_per_pixel) ||
        !std::isfinite(settings.vertical_radians_per_pixel) || settings.horizontal_radians_per_pixel <= 0 ||
        settings.vertical_radians_per_pixel <= 0 || settings.timestamp_domain == backend::ClockDomain::unspecified) return {};
    auto runtime = std::make_shared<Runtime>();
    runtime->settings = std::move(settings);
    runtime->backend = std::move(backend);
    return std::make_shared<Manager>(std::move(runtime));
}

ProviderStatistics provider_statistics(const std::shared_ptr<API_Manager>& manager) {
    const auto concrete = std::dynamic_pointer_cast<Manager>(manager);
    if (!concrete) return {};
    const auto& r = *concrete->runtime;
    std::lock_guard lock(r.state_mutex);
    return {r.delivered.load(), r.dropped.load(), r.rejected.load(), r.callback_failures.load(), r.stream_failures.load(), r.last_error};
}

std::shared_ptr<ir::Control> control_for(std::string_view instance, const std::shared_ptr<API_Manager>& manager) {
    const auto concrete = std::dynamic_pointer_cast<Manager>(manager);
    if (!concrete || concrete->runtime->settings.instance != instance) return {};
    const auto& runtime = concrete->runtime;
    if (callback_runtime == runtime.get()) return {};
    std::lock_guard operation(runtime->operations);
    if (auto control = runtime->control.load().lock()) return control;
    auto control = std::make_shared<ControlImpl>(runtime);
    runtime->control.store(control);
    return control;
}
std::shared_ptr<ir::Buffer> buffer_for(std::string_view instance, const std::shared_ptr<API_Manager>& manager) {
    const auto concrete = std::dynamic_pointer_cast<Manager>(manager);
    if (!concrete || concrete->runtime->settings.instance != instance) return {};
    return std::make_shared<HostBuffer>(concrete->runtime->affinity);
}

} // namespace gige_mel
