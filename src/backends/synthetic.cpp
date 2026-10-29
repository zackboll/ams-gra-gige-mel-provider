#include <gige_mel/backend.hpp>

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>

namespace gige_mel::backend {
namespace {

using Clock = std::chrono::steady_clock;

Error error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

Status validate(const CameraConfiguration& config) {
    if (config.width == 0 || config.height == 0 || config.width > 640 ||
        config.height > 480 || config.offset_x > 640 - config.width ||
        config.offset_y > 480 - config.height) {
        return error(ErrorCode::invalid_argument, "ROI must fit the 640x480 synthetic sensor");
    }
    if (config.frame_rate_hz < 1 || config.frame_rate_hz > 120) {
        return error(ErrorCode::invalid_argument, "Frame rate must be in [1,120] Hz");
    }
    const auto period = std::chrono::nanoseconds{1'000'000'000} / config.frame_rate_hz;
    if (config.exposure.count() <= 0 || config.exposure > period) {
        return error(ErrorCode::invalid_argument, "Exposure must be positive and no longer than one frame period");
    }
    return {};
}

struct Slot {
    std::vector<std::byte> bytes;
    bool in_use{false};
    std::uint64_t generation{0};
};

struct DeviceState {
    mutable std::mutex mutex;
    std::condition_variable changed;
    CameraConfiguration configuration;
    CameraConfiguration running_configuration;
    std::size_t pool_size;
    bool closed{false};
    bool running{false};
    bool stop_requested{false};
    std::uint64_t stream_identity{0};
    std::uint64_t next_stream_identity{1};
    std::uint64_t stream_generation{0};
    std::uint64_t configuration_generation{0};
    std::uint64_t next_frame_id{1};
    Clock::time_point next_delivery{};
    std::vector<Slot> slots;
    StreamStatistics statistics;

    DeviceState(CameraConfiguration initial, std::size_t count)
        : configuration(initial), running_configuration(initial), pool_size(count) {}
};

// The token, rather than the stream/device object, retains the storage owner.
// Its destructor can run on any consumer thread and never calls a dead object.
struct FrameOwner {
    std::shared_ptr<DeviceState> state;
    std::size_t slot;
    std::uint64_t generation;

    FrameOwner(std::shared_ptr<DeviceState> owner, std::size_t index, std::uint64_t epoch)
        : state(std::move(owner)), slot(index), generation(epoch) {}

    ~FrameOwner() noexcept {
        {
            std::lock_guard lock(state->mutex);
            if (slot < state->slots.size() &&
                state->slots[slot].generation == generation) {
                state->slots[slot].in_use = false;
            }
        }
        state->changed.notify_all();
    }
};

class SyntheticStream final : public IFrameStream {
public:
    SyntheticStream(std::shared_ptr<DeviceState> state, std::uint64_t identity)
        : state_(std::move(state)), identity_(identity) {}

    ~SyntheticStream() override {
        stop();
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stream_identity == identity_) {
                state_->stream_identity = 0;
            }
        }
        state_->changed.notify_all();
    }

    Status start(std::uint64_t configuration_generation) override {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) {
            return error(ErrorCode::disconnected, "Synthetic camera is closed");
        }
        if (state_->stream_identity != identity_ || state_->running) {
            return error(ErrorCode::invalid_state, "Stream is already running or no longer current");
        }
        if (std::any_of(state_->slots.begin(), state_->slots.end(),
                        [](const Slot& slot) { return slot.in_use; })) {
            return error(ErrorCode::resource_exhausted,
                         "Release outstanding backend frames before restarting the bounded pool");
        }
        try {
            std::vector<Slot> slots(state_->pool_size);
            const auto bytes = static_cast<std::size_t>(state_->configuration.width) *
                               state_->configuration.height;
            for (auto& slot : slots) {
                slot.bytes.resize(bytes);
                slot.generation = state_->stream_generation + 1;
            }
            state_->slots.swap(slots);
        } catch (const std::bad_alloc&) {
            return error(ErrorCode::resource_exhausted, "Could not allocate the synthetic frame pool");
        }
        state_->running_configuration = state_->configuration;
        state_->configuration_generation = configuration_generation;
        ++state_->stream_generation;
        state_->stop_requested = false;
        state_->running = true;
        state_->next_delivery = Clock::now() + state_->running_configuration.exposure;
        state_->changed.notify_all();
        return {};
    }

    Result<FrameLease> receive(std::chrono::milliseconds timeout) override {
        if (timeout < std::chrono::milliseconds::zero()) {
            return error(ErrorCode::invalid_argument, "Receive timeout must not be negative");
        }
        // Saturate instead of overflowing when a caller requests a huge wait.
        const auto now_at_entry = Clock::now();
        const auto maximum_wait = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::time_point::max() - now_at_entry);
        const auto deadline = timeout >= maximum_wait ? Clock::time_point::max()
                                                       : now_at_entry + timeout;
        std::unique_lock lock(state_->mutex);
        const auto generation = state_->stream_generation;
        bool reported_starvation = false;

        for (;;) {
            if (state_->closed) {
                return error(ErrorCode::disconnected, "Synthetic camera was closed");
            }
            if (!state_->running || state_->stop_requested ||
                state_->stream_identity != identity_ ||
                generation != state_->stream_generation) {
                return error(ErrorCode::stopped, "Synthetic stream is stopped");
            }
            const auto free_slot = std::find_if(state_->slots.begin(), state_->slots.end(),
                                                [](const Slot& slot) { return !slot.in_use; });
            const auto now = Clock::now();
            if (free_slot != state_->slots.end() && now >= state_->next_delivery) {
                const auto index = static_cast<std::size_t>(free_slot - state_->slots.begin());
                std::shared_ptr<FrameOwner> owner;
                try {
                    owner = std::make_shared<FrameOwner>(state_, index, generation);
                } catch (const std::bad_alloc&) {
                    return error(ErrorCode::resource_exhausted, "Could not allocate a frame lease");
                }
                const auto config = state_->running_configuration;
                const auto frame_id = state_->next_frame_id++;
                for (std::uint32_t y = 0; y < config.height; ++y) {
                    for (std::uint32_t x = 0; x < config.width; ++x) {
                        free_slot->bytes[static_cast<std::size_t>(y) * config.width + x] =
                            static_cast<std::byte>((x + y + frame_id) & 0xffU);
                    }
                }
                free_slot->in_use = true;

                FrameDescriptor descriptor;
                descriptor.width = config.width;
                descriptor.height = config.height;
                descriptor.row_stride_bytes = config.width;
                descriptor.native_frame_id = frame_id;
                descriptor.stream_generation = generation;
                descriptor.configuration_generation = state_->configuration_generation;
                descriptor.configuration = config;
                descriptor.timestamp.value =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()) -
                    config.exposure;
                descriptor.timestamp.domain = ClockDomain::host_monotonic;
                descriptor.timestamp.event = TimestampEvent::integration_start;
                descriptor.timestamp.valid = true;
                descriptor.timestamp.clock_generation = 1;

                ++state_->statistics.delivered_frames;
                state_->next_delivery = now + std::chrono::nanoseconds{1'000'000'000} /
                                                  config.frame_rate_hz;
                const auto bytes = std::span<const std::byte>{free_slot->bytes};
                // Nothing after the owner creation can allocate or throw. Move
                // its last reference out before releasing the state mutex.
                FrameLease lease{descriptor, bytes, std::move(owner)};
                lock.unlock();
                return lease;
            }
            if (free_slot == state_->slots.end() && !reported_starvation) {
                ++state_->statistics.pool_starvation_events;
                reported_starvation = true;
            }
            if (now >= deadline) {
                ++state_->statistics.timeout_count;
                return error(ErrorCode::timeout, "Timed out waiting for a synthetic frame or a free slot");
            }
            const auto wake = free_slot == state_->slots.end()
                                  ? deadline
                                  : std::min(deadline, state_->next_delivery);
            state_->changed.wait_until(lock, wake);
        }
    }

    void request_stop() noexcept override {
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stream_identity == identity_) {
                state_->stop_requested = true;
                state_->running = false;
            }
        }
        state_->changed.notify_all();
    }

    void stop() noexcept override { request_stop(); }

    StreamStatistics statistics() const override {
        std::lock_guard lock(state_->mutex);
        return state_->statistics;
    }

private:
    std::shared_ptr<DeviceState> state_;
    std::uint64_t identity_;
};

class SyntheticDevice final : public ICameraDevice {
public:
    explicit SyntheticDevice(std::shared_ptr<DeviceState> state) : state_(std::move(state)) {}
    ~SyntheticDevice() override { (void)close(); }

    CameraCapabilities capabilities() const override { return {}; }

    Result<CameraConfiguration> configuration() const override {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) {
            return error(ErrorCode::disconnected, "Synthetic camera is closed");
        }
        return state_->configuration;
    }

    Result<CameraConfiguration> apply(const CameraConfiguration& config) override {
        const auto valid = validate(config);
        if (!valid) {
            return valid.error();
        }
        std::lock_guard lock(state_->mutex);
        if (state_->closed) {
            return error(ErrorCode::disconnected, "Synthetic camera is closed");
        }
        if (state_->running) {
            return error(ErrorCode::invalid_state, "Stop the stream before applying configuration");
        }
        state_->configuration = config;
        return state_->configuration;
    }

    Result<std::unique_ptr<IFrameStream>> create_stream() override {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) {
            return error(ErrorCode::disconnected, "Synthetic camera is closed");
        }
        if (state_->stream_identity != 0) {
            return error(ErrorCode::busy, "Only one live stream is supported per synthetic device");
        }
        const auto identity = state_->next_stream_identity++;
        std::unique_ptr<IFrameStream> stream;
        try {
            stream = std::make_unique<SyntheticStream>(state_, identity);
        } catch (const std::bad_alloc&) {
            return error(ErrorCode::resource_exhausted, "Could not allocate a synthetic stream");
        }
        state_->stream_identity = identity;
        return stream;
    }

    Status close() override {
        {
            std::lock_guard lock(state_->mutex);
            state_->closed = true;
            state_->running = false;
            state_->stop_requested = true;
        }
        state_->changed.notify_all();
        return {};
    }

private:
    std::shared_ptr<DeviceState> state_;
};

class SyntheticBackend final : public ICameraBackend {
public:
    explicit SyntheticBackend(SyntheticOptions options) : options_(options) {}

    std::string_view id() const noexcept override { return "synthetic"; }

    Result<std::vector<DeviceInfo>> enumerate() override {
        return std::vector<DeviceInfo>{{"synthetic", "synthetic-0", "Deterministic Mono8 camera"}};
    }

    Result<std::shared_ptr<ICameraDevice>> open(std::string_view device_id) override {
        if (device_id != "synthetic-0") {
            return error(ErrorCode::not_found, "Synthetic device ID must be synthetic-0");
        }
        if (options_.frame_pool_size == 0 || options_.frame_pool_size > 64) {
            return error(ErrorCode::invalid_argument, "Synthetic frame pool size must be in [1,64]");
        }
        const auto valid = validate(options_.initial_configuration);
        if (!valid) {
            return valid.error();
        }
        std::lock_guard backend_lock(mutex_);
        if (const auto current = opened_device_.lock()) {
            std::lock_guard device_lock(current->mutex);
            if (!current->closed) {
                return error(ErrorCode::busy, "Synthetic camera is already open in this backend");
            }
        }
        try {
            auto state = std::make_shared<DeviceState>(options_.initial_configuration,
                                                       options_.frame_pool_size);
            std::shared_ptr<ICameraDevice> device = std::make_shared<SyntheticDevice>(state);
            opened_device_ = std::move(state);
            return device;
        } catch (const std::bad_alloc&) {
            return error(ErrorCode::resource_exhausted, "Could not allocate a synthetic camera");
        }
    }

private:
    SyntheticOptions options_;
    std::mutex mutex_;
    std::weak_ptr<DeviceState> opened_device_;
};

} // namespace

std::shared_ptr<ICameraBackend> make_synthetic_backend(SyntheticOptions options) {
    return std::make_shared<SyntheticBackend>(options);
}

} // namespace gige_mel::backend
