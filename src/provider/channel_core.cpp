#include "channel_core.hpp"
#include "capabilities.hpp"
#include "host_buffer.hpp"
#include "stream_state.hpp"
#include "../config_parser.hpp"
#include <algorithm>
#include <limits>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

ChannelCore::ChannelCore(std::shared_ptr<Runtime> runtime, ir::Config config)
    : runtime_(std::move(runtime)), config_(std::move(config)) {}

ChannelCore::~ChannelCore() {
    // Public detach/disable normally joins first. If a consumer destroys its
    // control graph from a callback, state captured by the thread still owns
    // every object it needs; never join the current thread.
    // Derived auxiliary cleanup belongs to disable/detach and the derived
    // destructor, never a virtual hook reached from this base destructor.
    stop_locked();
    for (auto& buffer : buffers_) buffer->unbind();
}

ir::ChannelType ChannelCore::type() const { return config_.getChannelType(); }

bool ChannelCore::is_detached() const { return detached_.load(); }

mel::RequestFor<ir::Return> ChannelCore::sendKeepAliveRep() {
    if (is_detached()) return failed<ir::Return>(mel::ErrorCode::InvalidState, "channel is detached");
    return ready(ir::Return::Success);
}

mel::RequestFor<ir::ChannelCommsTestRep> ChannelCore::send(ir::ChannelCommsTestReq request) {
    if (in_navigation_submission)
        return failed<ir::ChannelCommsTestRep>(mel::ErrorCode::InvalidState, "communications unavailable during backend navigation submission");
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

ir::Return ChannelCore::registerBuffer(std::shared_ptr<ir::Buffer> buffer) {
    if (in_provider_callback()) return ir::Return::Fail;
    const auto control_pin = runtime_->control.load().lock();
    if (!control_pin) return ir::Return::Fail;
    std::lock_guard operation(runtime_->operations);
    if (is_detached()) return ir::Return::Fail;
    if (type() == ir::ChannelType::HealthAndStatus) return ir::Return::NotSupported;
    if (enabled_) return ir::Return::Fail;
    if (type() != ir::ChannelType::IRSTImage && type() != ir::ChannelType::StackedImage) return ir::Return::NotSupported;
    auto host = std::dynamic_pointer_cast<HostBuffer>(buffer);
    if (!host) return ir::Return::BadPointer;
    if (buffers_.size() >= runtime_->settings.maximum_registered_buffers) return ir::Return::Fail;
    const auto minimum_size = static_cast<std::size_t>(getCapabilities().getBufferSize());
    const auto status = host->bind(this, runtime_->affinity, minimum_size);
    if (status != ir::Return::Success) return status;
    const auto start = reinterpret_cast<std::uintptr_t>(host->getBufferAddress());
    const auto end = start + static_cast<std::size_t>(host->getSize());
    std::erase_if(runtime_->registered_memory, [&](const auto& entry) {
        const auto registered = entry.lock();
        return !registered || registered == host || !registered->reserved();
    });
    for (const auto& entry : runtime_->registered_memory) {
        const auto registered = entry.lock();
        if (registered && registered != host && registered->overlaps_reserved(start, end)) {
            host->unbind(); return ir::Return::Fail;
        }
    }
    try { runtime_->registered_memory.push_back(host); buffers_.push_back(host); }
    catch (...) { host->unbind(); throw; }
    return ir::Return::Success;
}

ir::Return ChannelCore::unregisterBuffer(std::shared_ptr<ir::Buffer> buffer) {
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

ir::Return ChannelCore::enable() {
    if (in_provider_callback()) return ir::Return::Fail;
    const auto control_pin = runtime_->control.load().lock();
    if (!control_pin) return ir::Return::Fail;
    std::lock_guard operation(runtime_->operations);
    if (is_detached()) return ir::Return::Fail;
    if (enabled_) return stream_state_ && stream_state_->failed ? ir::Return::Fail : ir::Return::Success;
    if (type() != ir::ChannelType::IRSTImage && type() != ir::ChannelType::StackedImage) { enabled_ = true; return ir::Return::Success; }
    if (runtime_->task_session) return ir::Return::Fail;
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
    const auto layout = backend::image_layout(runtime_->camera_capabilities.pixel_format,
                                              state->configuration.width, state->configuration.height);
    if (!layout || layout->image_bytes > std::numeric_limits<std::uint32_t>::max()) return ir::Return::Fail;
    const auto bytes = type() == ir::ChannelType::StackedImage ?
        static_cast<std::size_t>(runtime_->stack_support.packed_bytes) : layout->image_bytes;
    for (const auto& buffer : buffers_) {
        if (buffer->outstanding() || static_cast<std::size_t>(buffer->getSize()) < bytes) return ir::Return::Fail;
    }
    try {
        if (type() == ir::ChannelType::StackedImage) {
            auto* source = dynamic_cast<backend::IStackedImageSource*>(runtime_->device.get());
            if (!source || source->stacked_image_support() != runtime_->stack_support) return ir::Return::Fail;
            auto opened = source->open_stacked_images(state->generation);
            if (!opened) { runtime_->report(opened.error().message); return ir::Return::Fail; }
            state->stack_stream = std::move(opened).value();
            if (!state->stack_stream) return ir::Return::Fail;
        } else {
            auto created = runtime_->device->create_stream();
            if (!created) { runtime_->report(created.error().message); return ir::Return::Fail; }
            state->stream = std::move(created).value();
            auto started = state->stream->start(state->generation);
            if (!started) { runtime_->report(started.error().message); return ir::Return::Fail; }
        }
        {
            std::lock_guard lock(runtime_->state_mutex);
            runtime_->image_active = true;
            runtime_->acquisition = state;
        }
        stream_state_ = state;
        runtime_->acquisition_channel = shared_from_this();
        enabled_ = true;
        if (!start_auxiliary_locked()) throw std::runtime_error("auxiliary source subscription failed");
        worker_ = std::thread([state, weak = weak_from_this()] {
            if (state->stack_stream) run_stacks(state, weak); else run(state, weak);
        });
        return ir::Return::Success;
    } catch (const std::exception& error) {
        stop_auxiliary_locked();
        state->stopping = true;
        if (state->stream) state->stream->stop();
        if (state->stack_stream) state->stack_stream->request_stop();
        {
            // Unpublish while the local state still owns the acquisition,
            // including thread-construction failure after publication.
            std::lock_guard lock(runtime_->state_mutex);
            runtime_->image_active = false;
            runtime_->acquisition.reset();
        }
        stream_state_.reset();
        runtime_->acquisition_channel.reset();
        enabled_ = false;
        runtime_->report(error.what());
        return ir::Return::Fail;
    }
}

ir::Return ChannelCore::disable() {
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

ir::ChannelCapability ChannelCore::getCapabilities() const {
    return capability(*runtime_, type(), config_.getChanID(), &config_);
}

ir::Return ChannelCore::registerMetadataCallback(std::function<void(ir::Channel&, const ir::ChannelCommsTestRep* const)> callback) {
    return register_callback(comms_callback_, std::move(callback));
}

void ChannelCore::detach_locked() {
    { std::lock_guard lock(callback_mutex_); detached_ = true; }
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

ConfigurationOutcome ChannelCore::configure_locked(const backend::CameraConfiguration& requested, bool profile_valid) {
    if (runtime_->task_session)
        return ConfigurationOutcome::failure({backend::ErrorCode::invalid_state, "retire tasking before configuring"});
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

mel::RequestFor<ir::Return> ChannelCore::config_set(const ir::ConfigSetCommand& command) {
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

mel::RequestFor<ir::CameraCommandResp> ChannelCore::camera_command(const ir::CameraCommand& command) {
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

void ChannelCore::on_camera_reply(const ir::CameraCommandResp&) {}

void ChannelCore::on_command_status(const ir::CommandStatus&) {}

bool ChannelCore::start_auxiliary_locked() { return true; }

void ChannelCore::stop_auxiliary_locked() {}

void ChannelCore::stop_locked() {
    const bool owned_stream = static_cast<bool>(stream_state_);
    if (stream_state_) {
        stream_state_->stopping = true;
        if (stream_state_->stream) stream_state_->stream->request_stop();
        if (stream_state_->stack_stream) stream_state_->stack_stream->request_stop();
    }
    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) worker_.detach();
        else worker_.join();
    }
    if (stream_state_ && stream_state_->stream) stream_state_->stream->stop();
    if (owned_stream) {
        // Retain the strong stream owner until no snapshot can promote the
        // weak publication. Earlier snapshots release their temporary owner
        // before this lock is acquired. No backend destruction under lock.
        std::lock_guard lock(runtime_->state_mutex);
        runtime_->image_active = false;
        runtime_->acquisition.reset();
        runtime_->acquisition_channel.reset();
    }
    stream_state_.reset();
    enabled_ = false;
}

} // namespace provider_detail
} // namespace gige_mel
