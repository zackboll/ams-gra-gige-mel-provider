#include <gige_mel/provider.hpp>
#include "provider_internal.hpp"
#include "config_parser.hpp"

#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
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

ir::CannotComply command_reason(backend::ErrorCode code) {
    using B = backend::ErrorCode;
    switch (code) {
    case B::invalid_argument: return ir::CannotComply::InvalidInputParameter;
    case B::unsupported: return ir::CannotComply::CapabilityUnavailable;
    case B::not_found: return ir::CannotComply::UnknownID;
    case B::resource_exhausted: return ir::CannotComply::InsufficientResources;
    case B::disconnected: return ir::CannotComply::SubsystemUnavailable;
    case B::io_error: return ir::CannotComply::SubsystemFault;
    case B::timeout: case B::busy: case B::stopped: case B::invalid_state:
        return ir::CannotComply::StateOrSettings;
    }
    return ir::CannotComply::Other;
}

struct ConfigurationOutcome {
    bool success{false};
    // invalid_argument/unsupported from apply are ordinary negative responses;
    // schema errors and transport/readback failures are error-only futures.
    std::optional<backend::Error> error;
    ir::CannotComply reason{ir::CannotComply::Other};
    std::string description;

    static ConfigurationOutcome failure(const backend::Error& failure, bool error_only = true) {
        return {false, error_only ? std::optional{failure} : std::nullopt,
                command_reason(failure.code), failure.message};
    }
    ir::CommandStatus status(std::uint32_t id) const {
        ir::CommandStatus result;
        result.setCommandID(id);
        result.setState(success ? ir::CommandState::Accepted : ir::CommandState::Rejected);
        result.setReasonID(success ? ir::CannotComply::NotSet : reason);
        result.setReasonDescription(success ? "" : description);
        return result;
    }
};

class ChannelCore;
class ControlImpl;
struct StreamState;

struct Runtime {
    ProviderSettings settings;
    std::shared_ptr<backend::ICameraBackend> backend;
    std::string backend_id; // immutable, acquired before any publisher exists
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
    std::weak_ptr<StreamState> acquisition; // guarded by state_mutex
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

// Stack frames live on the callback's stack: admission cannot be stranded by
// allocation failure, and nested callbacks retain every runtime in the chain.
struct CallbackGuard;
thread_local const CallbackGuard* callback_context = nullptr;
struct CallbackGuard {
    const Runtime* const runtime;
    const CallbackGuard* const previous;
    explicit CallbackGuard(const Runtime* value) noexcept
        : runtime(value), previous(callback_context) { callback_context = this; }
    ~CallbackGuard() { callback_context = previous; }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

bool in_provider_callback() noexcept { return callback_context != nullptr; }

bool in_runtime_callback(const Runtime* runtime) noexcept {
    for (auto frame = callback_context; frame; frame = frame->previous) {
        if (frame->runtime == runtime) return true;
    }
    return false;
}

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
    if (type != ir::ChannelType::HealthAndStatus) {
        cap.setWidth(c.width);
        cap.setHeight(c.height);
        cap.setBitDepth(8); // significant bits; FrameHeader separately reports storage bits
        cap.setRowPitch(c.width);
        cap.setBufferSize(c.width * c.height);
        cap.setImageSize(c.width * c.height);
        cap.setNumberOfBands(1);
        cap.setFormat(ir::PixelFormat::Mono);
    }
    cap.setChannelTypes({type});
    std::set<ir::ChannelMetadataCapabilityType> metadata{ir::ChannelMetadataCapabilityType::ChannelCommsTestRep};
    if (type == ir::ChannelType::IRSTImage) metadata.insert(ir::ChannelMetadataCapabilityType::CameraCommandResp);
    if (type == ir::ChannelType::CommandAndControl) metadata.insert(ir::ChannelMetadataCapabilityType::CommandStatus);
    if (type == ir::ChannelType::HealthAndStatus) {
        metadata.insert(ir::ChannelMetadataCapabilityType::MFAStatus);
        metadata.insert(ir::ChannelMetadataCapabilityType::MFAStatusDetailed);
    }
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
        // Derived auxiliary cleanup belongs to disable/detach and the derived
        // destructor, never a virtual hook reached from this base destructor.
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
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        if (type() == ir::ChannelType::HealthAndStatus) return ir::Return::NotSupported;
        if (enabled_) return ir::Return::Fail;
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
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        if (type() == ir::ChannelType::HealthAndStatus) return ir::Return::NotSupported;
        if (enabled_) return ir::Return::Fail;
        const auto found = std::find(buffers_.begin(), buffers_.end(), buffer);
        if (found == buffers_.end()) return ir::Return::BadPointer;
        if ((*found)->outstanding()) return ir::Return::Fail;
        (*found)->unbind();
        buffers_.erase(found);
        return ir::Return::Success;
    }
    ir::Return enable() override {
        if (in_provider_callback()) return ir::Return::Fail;
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
                runtime_->acquisition = state;
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
        if (in_provider_callback()) return ir::Return::Fail;
        // Pin before taking operations: a callback dropping the last external
        // Control must not run its destructor while this thread joins it.
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        stop_auxiliary_locked();
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
        stop_auxiliary_locked();
        stop_locked();
        // Only defensive destruction involving this runtime may skip its own
        // drain. External detach must still wait for every admitted callback.
        if (!in_runtime_callback(runtime_.get())) {
            std::unique_lock lock(callback_mutex_);
            callback_cv_.wait(lock, [this] { return active_metadata_callbacks_ == 0; });
        }
        for (auto& buffer : buffers_) buffer->unbind();
        buffers_.clear();
    }

protected:
    template <class T> using Callback = std::function<void(ir::Channel&, const T*)>;

    template <class T> ir::Return register_callback(Callback<T>& slot, Callback<T> callback) {
        if (in_provider_callback() || is_detached()) return ir::Return::Fail;
        if (!callback) return ir::Return::BadPointer;
        std::lock_guard lock(callback_mutex_);
        if (is_detached()) return ir::Return::Fail;
        if (slot) return ir::Return::Fail;
        slot = std::move(callback);
        return ir::Return::Success;
    }
    template <class T> void invoke(const Callback<T>& callback, const T& message,
                                  const std::atomic<bool>* admission = nullptr) {
        if (!callback) return;
        {
            std::lock_guard lock(callback_mutex_);
            if (is_detached() || (admission && !admission->load())) return;
            ++active_metadata_callbacks_;
        }
        CallbackGuard guard(runtime_.get());
        try { callback(*this, &message); }
        catch (...) {
            ++runtime_->callback_failures;
            try { runtime_->report(admission ? "Health metadata callback threw; periodic reporting continues" :
                "metadata callback threw; response remains available through its future"); } catch (...) {}
        }
        { std::lock_guard lock(callback_mutex_); --active_metadata_callbacks_; }
        callback_cv_.notify_all();
    }
    template <class T> mel::RequestFor<T> unsupported(const char* name) {
        return failed<T>(is_detached() ? mel::ErrorCode::InvalidState : mel::ErrorCode::Unsupported,
                         std::string(name) + ": not available in the experimental image-stream profile");
    }

    // Callback-free shared transaction. Caller holds operations, has admitted
    // this submission and pins Control/channel through result and dispatch.
    ConfigurationOutcome configure_locked(const backend::CameraConfiguration& requested, bool profile_valid = true) {
        {
            std::lock_guard lock(runtime_->state_mutex);
            if (runtime_->image_active)
                return ConfigurationOutcome::failure({backend::ErrorCode::invalid_state, "disable image acquisition before configuring"});
        }
        if (!profile_valid || !valid_layout(requested, runtime_->camera_capabilities)) {
            const std::string message = "camera configuration cannot be satisfied: requires a complete, valid Mono8 staring configuration";
            runtime_->report(message);
            return ConfigurationOutcome::failure({backend::ErrorCode::invalid_argument, message}, false);
        }
        auto applied = runtime_->device->apply(requested);
        // Even an apply failure may have partially mutated the device.
        auto actual = runtime_->device->configuration();
        bool valid;
        {
            std::lock_guard lock(runtime_->state_mutex);
            ++runtime_->configuration_generation;
            valid = actual && valid_layout(actual.value(), runtime_->camera_capabilities);
            runtime_->configuration_valid = valid;
            if (valid) runtime_->configuration = actual.value();
        }
        ConfigurationOutcome outcome;
        if (!actual) outcome = ConfigurationOutcome::failure(actual.error());
        else if (!applied) {
            const auto code = applied.error().code;
            outcome = ConfigurationOutcome::failure(applied.error(),
                code != backend::ErrorCode::invalid_argument && code != backend::ErrorCode::unsupported);
        } else if (!valid || actual.value() != requested) {
            outcome.description = "camera readback differs from requested configuration or violates the supported layout";
        } else outcome.success = true;
        if (!outcome.success) runtime_->report(outcome.description);
        return outcome;
    }

    mel::RequestFor<ir::Return> config_set(const ir::ConfigSetCommand& command) {
        // Must precede parsing, operation locks and callback admission, including
        // cross-provider callbacks: a rejection must not recursively send status.
        if (in_provider_callback())
            return failed<ir::Return>(mel::ErrorCode::InvalidState, "schedule configuration outside callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::Return>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        ConfigurationOutcome outcome;
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<ir::Return>(mel::ErrorCode::InvalidState, "channel is detached");
            const auto parsed = parse_camera_config(command.getConfig());
            outcome = parsed ? configure_locked(parsed.value()) : ConfigurationOutcome::failure(parsed.error());
        }
        auto result = outcome.error ? failed<ir::Return>(map_error(outcome.error->code), outcome.description)
                                    : ready(outcome.success ? ir::Return::Success : ir::Return::Fail);
        on_command_status(outcome.status(command.getCommandID()));
        return result;
    }

    mel::RequestFor<ir::CameraCommandResp> camera_command(const ir::CameraCommand& command) {
        if (in_provider_callback())
            return failed<ir::CameraCommandResp>(mel::ErrorCode::InvalidState, "schedule camera changes outside callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::CameraCommandResp>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        std::shared_ptr<ChannelCore> image_recipient;
        ConfigurationOutcome outcome;
        ir::CameraCommandResp reply;
        reply.setCommandID(command.getCommandID());
        // size is left at zero: this C++ object has no specified serialized wire
        // encoding. sizeof(CameraCommandResp) would not be a portable wire size.
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<ir::CameraCommandResp>(mel::ErrorCode::InvalidState, "channel is detached");
            image_recipient = runtime_->image_channel.lock();
            backend::CameraConfiguration requested;
            requested.width = command.getWidth();
            requested.height = command.getHeight();
            requested.offset_x = command.getColumnOffset();
            requested.offset_y = command.getRowOffset();
            requested.frame_rate_hz = command.getFrameRate();
            requested.exposure = command.getIntegrationTime();
            outcome = configure_locked(requested,
                command.getImageType() == ir::ImageType::Staring && command.getNumberOfStages() == 0);
        }
        if (outcome.error) {
            auto result = failed<ir::CameraCommandResp>(map_error(outcome.error->code), outcome.description);
            on_command_status(outcome.status(command.getCommandID()));
            return result;
        }
        reply.setCommandSuccess(outcome.success);
        reply.setTimestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()));
        auto result = ready(reply);
        // Camera responses are an Image callback even when C2 submitted the
        // command. No operation/state locks are held across user code.
        if (image_recipient) image_recipient->on_camera_reply(reply);
        on_command_status(outcome.status(command.getCommandID()));
        return result;
    }

    virtual void on_camera_reply(const ir::CameraCommandResp&) {}
    virtual void on_command_status(const ir::CommandStatus&) {}
    // Called by disable AND internal detach, including Control destruction.
    virtual void stop_auxiliary_locked() {}
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
            runtime_->acquisition.reset();
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
                    // Classify at the cancellation observation: a backend stop
                    // alone does not establish provider-requested shutdown.
                    if (state->stopping) break;
                    if (result.error().code == backend::ErrorCode::timeout) continue;
                    ++state->runtime->stream_failures;
                    state->failed = true;
                    state->runtime->report(result.error().code == backend::ErrorCode::stopped && result.error().message.empty()
                        ? "backend stream stopped without provider cancellation" : result.error().message);
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
    mel::RequestFor<ir::Return> send(ir::ConfigSetCommand command) override { return config_set(command); }
    GIGE_UNSUPPORTED_CALLBACK(mel::BIT_Configuration)
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::CommandStatus* const)> callback) override {
        return register_callback(status_callback_, std::move(callback));
    }
    GIGE_UNSUPPORTED_CALLBACK(mel::CalibrationConfiguration)
    GIGE_UNSUPPORTED_CALLBACK(ir::CameraProtectCmdResp)
    GIGE_UNSUPPORTED_CALLBACK(mel::CalibrationStatus)
    GIGE_UNSUPPORTED_CALLBACK(mel::BIT_Status)
private:
    void on_command_status(const ir::CommandStatus& status) override {
        Callback<ir::CommandStatus> callback;
        { std::lock_guard lock(callback_mutex_); callback = status_callback_; }
        invoke(callback, status);
    }
    Callback<ir::CommandStatus> status_callback_;
};
// One core-state capture supplies both reports. Counters remain independently
// atomic samples; no per-frame transaction lock is imposed by Health.
struct HealthSnapshot {
    bool initialized, configuration_valid, active, failed;
    std::uint64_t generation;
    ProviderStatistics statistics;
};

HealthSnapshot health_snapshot(const Runtime& r) {
    std::lock_guard lock(r.state_mutex);
    const auto acquisition = r.acquisition.lock();
    return {r.initialized, r.configuration_valid, r.image_active,
        acquisition && acquisition->failed.load(), r.configuration_generation,
        {r.delivered.load(), r.dropped.load(), r.rejected.load(),
         r.callback_failures.load(), r.stream_failures.load(), r.last_error}};
}

class HealthChannelImpl final : public ChannelCore, public ir::HealthStatusChannel {
    struct Publisher {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<bool> running{true};
        bool prompted{true};
    };
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::send;
    using ChannelCore::registerMetadataCallback;
    ~HealthChannelImpl() override { stop_auxiliary_locked(); }

    ir::Return enable() override {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        if (publisher_ && publisher_->running) return ir::Return::Success;
        try {
            auto state = std::make_shared<Publisher>();
            { std::lock_guard lock(callback_mutex_); publisher_ = state; }
            worker_ = std::thread([state, weak = weak_from_this(), runtime = runtime_] {
                run(state, weak, runtime);
            });
            return ir::Return::Success;
        } catch (const std::exception& error) {
            stop_auxiliary_locked();
            runtime_->report(error.what());
            return ir::Return::Fail;
        }
    }
    ir::Return registerMetadataCallback(Callback<mel::MFA_Status> callback) override {
        const auto result = register_callback(status_callback_, std::move(callback));
        if (result == ir::Return::Success) prompt();
        return result;
    }
    ir::Return registerMetadataCallback(Callback<mel::MFA_StatusDetailed> callback) override {
        const auto result = register_callback(detailed_callback_, std::move(callback));
        if (result == ir::Return::Success) prompt();
        return result;
    }
    GIGE_UNSUPPORTED_CALLBACK(ir::LFStatus)
    GIGE_UNSUPPORTED_CALLBACK(mel::BIT_Status)
    GIGE_UNSUPPORTED_CALLBACK(ir::SubsystemStatusResp)
    GIGE_UNSUPPORTED_CALLBACK(mel::DiscreteStatus)
    GIGE_UNSUPPORTED_CALLBACK(mel::MFA_SecurityAuditRecord)
    GIGE_UNSUPPORTED_CALLBACK(ir::NUC_TempData)

private:
    void prompt() {
        std::shared_ptr<Publisher> state;
        { std::lock_guard lock(callback_mutex_); state = publisher_; }
        if (!state) return;
        { std::lock_guard lock(state->mutex); state->prompted = true; }
        state->changed.notify_all();
    }
    void stop_auxiliary_locked() override {
        std::shared_ptr<Publisher> state;
        { std::lock_guard lock(callback_mutex_); state = publisher_; }
        if (state) {
            { std::lock_guard lock(state->mutex); state->running = false; }
            state->changed.notify_all();
        }
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) worker_.detach();
            else worker_.join();
        }
    }
    void publish(const Publisher& publisher) {
        Callback<mel::MFA_Status> status_callback;
        Callback<mel::MFA_StatusDetailed> detailed_callback;
        {
            std::lock_guard lock(callback_mutex_);
            status_callback = status_callback_;
            detailed_callback = detailed_callback_;
        }
        const auto s = health_snapshot(*runtime_);
        const auto state = !s.initialized ? mel::MFA_State::Unknown :
            !s.configuration_valid || (s.active && s.failed) ? mel::MFA_State::Degraded :
            s.active ? mel::MFA_State::Operate : mel::MFA_State::Standby;
        const auto description = !s.initialized ? "Provider not initialized" :
            !s.configuration_valid ? "Actual camera configuration is not known valid" :
            s.active && s.failed ? "Owned Image acquisition has observed a terminal failure; explicitly disable before recovery" :
            s.active ? "Image acquisition is logically owned without an observed terminal failure" :
            "Idle with known valid configuration; explicit Image enable may be attempted (not a physical connection probe)";
        mel::MFA_Status status(state, description,
            "Experimental Mono8 image-stream profile; no task scheduling, scanning, BIT, calibration or ModeCmd",
            mel::StateTransitionStatus::NotTransitioning,
            mel::About("ams-gra-gige-mel-provider (software provider)", "", "0.1.0", "", ""), {});
        mel::MFA_StatusDetailed detailed;
        const auto add = [&](const char* name, std::string value) {
            detailed.addStatus(mel::NameValuePair(std::string("gige_mel.") + name, std::move(value)));
        };
        add("health_schema", "1");
        add("backend_id", runtime_->backend_id);
        add("device_id", runtime_->settings.device_id);
        add("configuration_valid", s.configuration_valid ? "true" : "false");
        add("configuration_generation", std::to_string(s.generation));
        add("acquisition_state", !s.active ? "idle" : s.failed ? "faulted" : "running");
        add("delivered_frames", std::to_string(s.statistics.delivered_frames));
        add("dropped_no_buffer", std::to_string(s.statistics.dropped_no_buffer));
        add("rejected_frames", std::to_string(s.statistics.rejected_frames));
        add("callback_failures", std::to_string(s.statistics.callback_failures));
        add("stream_failures", std::to_string(s.statistics.stream_failures));
        add("last_error_history", s.statistics.last_error);
        invoke(status_callback, status, &publisher.running);
        invoke(detailed_callback, detailed, &publisher.running);
    }
    static void run(const std::shared_ptr<Publisher>& state, const std::weak_ptr<ChannelCore>& weak,
                    const std::shared_ptr<Runtime>& runtime) noexcept {
        while (state->running) {
            {
                std::lock_guard lock(state->mutex);
                state->prompted = false;
            }
            try {
                // Pins are scoped to preparation/dispatch, NEVER the wait. Their
                // destruction may itself tear down Control on this worker.
                const auto control_pin = runtime->control.load().lock();
                const auto channel = std::dynamic_pointer_cast<HealthChannelImpl>(weak.lock());
                if (!control_pin || !channel || channel->is_detached()) break;
                channel->publish(*state);
            } catch (...) {
                try { runtime->report("Health publication preparation failed"); } catch (...) {}
            }
            std::unique_lock lock(state->mutex);
            state->changed.wait_for(lock, 100ms, [&] { return !state->running || state->prompted; });
        }
    }
    Callback<mel::MFA_Status> status_callback_;
    Callback<mel::MFA_StatusDetailed> detailed_callback_;
    std::shared_ptr<Publisher> publisher_; // writes under callback_mutex AND operations
    std::thread worker_; // operations; defensive destructor on its own worker
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
        if (in_provider_callback()) return ir::Return::Fail;
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
                mel::UCI_ID image_id, c2_id, health_id;
                std::array<std::uint8_t, mel::UUID_SIZE> image_uuid{}, c2_uuid{};
                image_uuid.back() = 1;
                c2_uuid.back() = 2;
                image_id.setUUID(image_uuid);
                c2_id.setUUID(c2_uuid);
                image_id.setDescriptiveLabel(runtime_->settings.instance + "/image");
                c2_id.setDescriptiveLabel(runtime_->settings.instance + "/c2");
                auto health_uuid = c2_uuid;
                health_uuid.back() = 3;
                health_id.setUUID(health_uuid);
                health_id.setDescriptiveLabel(runtime_->settings.instance + "/health");
                capabilities_.push_back(capability(*runtime_, ir::ChannelType::IRSTImage, image_id));
                capabilities_.push_back(capability(*runtime_, ir::ChannelType::CommandAndControl, c2_id));
                capabilities_.push_back(capability(*runtime_, ir::ChannelType::HealthAndStatus, health_id));
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
        if (in_provider_callback()) return {};
        std::lock_guard operation(runtime_->operations);
        if (!runtime_->initialized) return {};
        const auto type = config.getChannelType();
        if (type != ir::ChannelType::IRSTImage && type != ir::ChannelType::CommandAndControl &&
            type != ir::ChannelType::HealthAndStatus) return {};
        const auto found = std::find_if(capabilities_.begin(), capabilities_.end(), [&](const auto& cap) {
            return cap.getChanID().getUUID() == config.getChanID().getUUID() && cap.getChannelTypes().front() == type;
        });
        if (found == capabilities_.end()) return {};
        if (std::any_of(channels_.begin(), channels_.end(), [type](const auto& channel) { return channel->type() == type; })) return {};
        if (type == ir::ChannelType::IRSTImage && !config.getImgLstnr()) return {};
        std::shared_ptr<ChannelCore> channel;
        if (type == ir::ChannelType::IRSTImage) channel = std::make_shared<ImageChannelImpl>(runtime_, config);
        else if (type == ir::ChannelType::CommandAndControl) channel = std::make_shared<C2ChannelImpl>(runtime_, config);
        else channel = std::make_shared<HealthChannelImpl>(runtime_, config);
        channels_.push_back(channel);
        if (type == ir::ChannelType::IRSTImage) runtime_->image_channel = channel;
        return channel;
    }
    ir::Return detachChannel(std::shared_ptr<ir::Channel> channel) override {
        if (in_provider_callback()) return ir::Return::Fail;
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
    runtime->backend_id = backend->id();
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
    if (in_provider_callback()) return {};
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
