#include "synthetic_profile.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
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
};

// All mutable transport state is serialized by the owning device's mutex.
// Image generation happens outside it, once a slot is pinned by a lease.
struct SourceState {
    explicit SourceState(ImageProductDescriptor descriptor) : product(descriptor) {}

    std::condition_variable changed;
    ImageProductDescriptor product;
    CameraConfiguration running_configuration;
    std::size_t row_stride{};
    bool running{false};
    bool receiver_open{false};
    std::uint64_t stream_generation{0};
    std::uint64_t configuration_generation{0};
    std::uint64_t next_frame_id{1};
    Clock::time_point next_delivery{};
    std::vector<Slot> slots;
    StreamStatistics statistics;
};

struct DeviceState {
    DeviceState(const SyntheticOptions& options, const StreamTopology& topology)
        : multi(topology.streams.size() > 1), pool_size(options.frame_pool_size) {
        // This fixture has exactly one product per receiver. The common
        // topology and lease contracts also admit several products per source.
        for (const auto& product : topology.products) sources.emplace_back(product);
    }

    const bool multi;
    const std::size_t pool_size;
    std::mutex mutex;
    bool closed{false};
    // Camera acquisition in the fixture is exactly the union of active
    // receivers: 0 -> N starts it; N -> 0 stops it. A native adapter can
    // associate commands and uncertainty with those device-level transitions.
    std::size_t active_receivers{0};
    std::deque<SourceState> sources; // fixed membership after device construction

    Result<std::vector<Slot>> prepare(const SourceState& source) const {
        if (std::any_of(source.slots.begin(), source.slots.end(),
                        [](const Slot& slot) { return slot.in_use; })) {
            return error(ErrorCode::resource_exhausted,
                         "Release outstanding backend frames before restarting this source pool");
        }
        const auto& p = source.product;
        const auto layout = *image_layout(p.pixel_format, p.configuration.width, p.configuration.height);
        std::vector<Slot> slots(pool_size);
        for (auto& slot : slots) slot.bytes.resize(p.span_bytes.value_or(layout.image_bytes));
        return slots;
    }

    void activate(SourceState& source, std::vector<Slot>& slots, std::uint64_t configuration_generation,
                  Clock::time_point start_time) noexcept {
        source.slots.swap(slots);
        source.running_configuration = source.product.configuration;
        const auto& c = source.running_configuration;
        source.row_stride = source.product.row_stride_bytes.value_or(
            image_layout(source.product.pixel_format, c.width, c.height)->row_bytes);
        source.configuration_generation = configuration_generation;
        ++source.stream_generation;
        source.running = true;
        ++active_receivers;
        source.next_delivery = start_time + c.exposure;
        source.changed.notify_all();
    }

    void retire(SourceState& source) noexcept {
        if (source.running) {
            source.running = false;
            --active_receivers;
        }
        source.changed.notify_all();
    }
};

// The token, rather than the stream/device object, retains the storage owner.
// Its destructor can run on any consumer thread and never calls a dead object.
struct FrameOwner {
    std::shared_ptr<DeviceState> state;
    std::size_t source_index;
    std::size_t slot;

    FrameOwner(std::shared_ptr<DeviceState> owner, std::size_t source, std::size_t index)
        : state(std::move(owner)), source_index(source), slot(index) {}

    ~FrameOwner() noexcept {
        auto& source = state->sources[source_index];
        {
            std::lock_guard lock(state->mutex);
            // A pool can only be replaced when none of its slots are pinned.
            source.slots[slot].in_use = false;
        }
        source.changed.notify_all();
    }
};

class SyntheticStream final : public IFrameStream {
public:
    SyntheticStream(std::shared_ptr<DeviceState> state, std::size_t source_index)
        : state_(std::move(state)), source_index_(source_index) {}

    // Called only at commit, while the device mutex is held. Unadmitted
    // handles may be destroyed during staging unwind, without taking a lock.
    void admit() noexcept {
        source().receiver_open = true;
        admitted_ = true;
    }

    ~SyntheticStream() override {
        if (!admitted_) return;
        std::lock_guard lock(state_->mutex);
        state_->retire(source());
        source().receiver_open = false;
    }

    Status start(std::uint64_t configuration_generation) override {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) {
            return error(ErrorCode::disconnected, "Synthetic camera is closed");
        }
        if (state_->multi) return error(ErrorCode::unsupported, "Use selected device acquisition");
        auto& s = source();
        if (s.running) return error(ErrorCode::invalid_state, "Stream is already running");
        try {
            auto slots = state_->prepare(s);
            if (!slots) return slots.error();
            state_->activate(s, slots.value(), configuration_generation, Clock::now());
        } catch (const std::bad_alloc&) {
            return error(ErrorCode::resource_exhausted, "Could not allocate the synthetic frame pool");
        }
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
        auto& s = source();
        const auto generation = s.stream_generation;
        bool reported_starvation = false;

        for (;;) {
            if (state_->closed) {
                return error(ErrorCode::disconnected, "Synthetic camera was closed");
            }
            if (state_->active_receivers == 0 || !s.running || generation != s.stream_generation) {
                return error(ErrorCode::stopped, "Synthetic stream is stopped");
            }
            const auto free_slot = std::find_if(s.slots.begin(), s.slots.end(),
                                                [](const Slot& slot) { return !slot.in_use; });
            const auto now = Clock::now();
            if (free_slot != s.slots.end() && now >= s.next_delivery) {
                const auto index = static_cast<std::size_t>(free_slot - s.slots.begin());
                std::shared_ptr<FrameOwner> owner;
                try {
                    owner = std::make_shared<FrameOwner>(state_, source_index_, index);
                } catch (const std::bad_alloc&) {
                    return error(ErrorCode::resource_exhausted, "Could not allocate a frame lease");
                }
                const auto config = s.running_configuration;
                const auto frame_id = s.next_frame_id++;
                const auto format = s.product.pixel_format;
                const auto layout = *image_layout(format, config.width, config.height);
                const auto stride = s.row_stride;
                free_slot->in_use = true;

                FrameDescriptor descriptor;
                descriptor.width = config.width;
                descriptor.height = config.height;
                descriptor.row_stride_bytes = stride;
                descriptor.pixel_format = format;
                descriptor.storage_bits_per_pixel = layout.storage_bits;
                descriptor.significant_bits_per_pixel = layout.significant_bits;
                descriptor.byte_order = layout.byte_order;
                descriptor.native_frame_id = frame_id;
                descriptor.stream_generation = generation;
                descriptor.configuration_generation = s.configuration_generation;
                descriptor.configuration = config;
                descriptor.source = s.product.source;
                descriptor.product = s.product.id;
                descriptor.timestamp.value =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()) -
                    config.exposure;
                descriptor.timestamp.domain = ClockDomain::host_monotonic;
                descriptor.timestamp.event = TimestampEvent::integration_start;
                descriptor.timestamp.valid = true;
                descriptor.timestamp.clock_generation = 1;

                ++s.statistics.delivered_frames;
                s.next_delivery = now + std::chrono::nanoseconds{1'000'000'000} /
                                                  config.frame_rate_hz;
                const auto bytes = std::span{free_slot->bytes};
                // A committed slot pins the pool across stop/close. Do the
                // pixel work without blocking commands or other receivers.
                lock.unlock();
                const auto sample_bytes = layout.storage_bits / 8;
                for (std::uint32_t y = 0; y < config.height; ++y) {
                    for (std::uint32_t x = 0; x < config.width; ++x) {
                        const auto offset = y * stride + static_cast<std::size_t>(x) * sample_bytes;
                        const auto value = synthetic::sample(format, x, y, frame_id);
                        bytes[offset] = static_cast<std::byte>(value & 0xffU);
                        if (sample_bytes == 2) bytes[offset + 1] = static_cast<std::byte>(value >> 8);
                    }
                }
                return FrameLease{descriptor, bytes, std::move(owner)};
            }
            if (free_slot == s.slots.end() && !reported_starvation) {
                ++s.statistics.pool_starvation_events;
                reported_starvation = true;
            }
            if (now >= deadline) {
                ++s.statistics.timeout_count;
                return error(ErrorCode::timeout, "Timed out waiting for a synthetic frame or a free slot");
            }
            const auto wake = free_slot == s.slots.end()
                                  ? deadline
                                  : std::min(deadline, s.next_delivery);
            s.changed.wait_until(lock, wake);
        }
    }

    void request_stop() noexcept override {
        std::lock_guard lock(state_->mutex);
        state_->retire(source());
    }

    void stop() noexcept override { request_stop(); }

    StreamStatistics statistics() const override {
        std::lock_guard lock(state_->mutex);
        return source().statistics;
    }

private:
    SourceState& source() const noexcept { return state_->sources[source_index_]; }
    std::shared_ptr<DeviceState> state_;
    const std::size_t source_index_;
    bool admitted_{false};
};

class SyntheticDevice final : public ICameraDevice {
public:
    explicit SyntheticDevice(std::shared_ptr<DeviceState> state) : state_(std::move(state)) {}
    ~SyntheticDevice() override { (void)close(); }

    CameraCapabilities capabilities() const override {
        CameraCapabilities caps;
        if (state_->multi) {
            // No authoritative legacy Image view. Use stream_topology().
            caps.mono8 = false;
            caps.sensor_width = caps.sensor_height = 0;
            return caps;
        }
        // The deterministic generator introduces no defective pixels; this is
        // a source invariant, not a measurement or physical-sensor calibration.
        caps.known_empty_defect_map = true;
        caps.pixel_format = state_->sources.front().product.pixel_format;
        caps.mono8 = caps.pixel_format == PixelFormat::mono8;
        return caps;
    }

    Result<CameraConfiguration> configuration() const override {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) {
            return error(ErrorCode::disconnected, "Synthetic camera is closed");
        }
        if (state_->multi) return error(ErrorCode::unsupported, "No device-wide image configuration; discover products");
        return state_->sources.front().product.configuration;
    }

    Result<CameraConfiguration> apply(const CameraConfiguration& config) override {
        if (!state_->multi) {
            if (auto valid = validate(config); !valid) return valid.error();
        }
        std::lock_guard lock(state_->mutex);
        if (state_->closed) {
            return error(ErrorCode::disconnected, "Synthetic camera is closed");
        }
        if (state_->multi) return error(ErrorCode::unsupported, "Synthetic multi-stream configurations are fixed");
        if (state_->active_receivers != 0) {
            return error(ErrorCode::invalid_state, "Stop the stream before applying configuration");
        }
        state_->sources.front().product.configuration = config;
        return config;
    }

    Result<std::unique_ptr<IFrameStream>> create_stream() override {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) {
            return error(ErrorCode::disconnected, "Synthetic camera is closed");
        }
        if (state_->multi) return error(ErrorCode::unsupported, "Use selected device acquisition");
        if (state_->sources.front().receiver_open) {
            return error(ErrorCode::busy, "Only one live stream is supported per synthetic device");
        }
        try {
            auto stream = std::make_unique<SyntheticStream>(state_, 0);
            stream->admit();
            return std::unique_ptr<IFrameStream>(std::move(stream));
        } catch (const std::bad_alloc&) {
            return error(ErrorCode::resource_exhausted, "Could not allocate a synthetic stream");
        }
    }

    Result<StreamTopology> stream_topology() const override {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) return error(ErrorCode::disconnected, "Synthetic camera is closed");
        StreamTopology topology;
        for (const auto& source : state_->sources) {
            topology.streams.push_back({source.product.source});
            topology.products.push_back(source.product);
        }
        return topology;
    }

    Result<std::vector<std::unique_ptr<IFrameStream>>> start_acquisition(
        std::span<const StreamId> selected, std::uint64_t configuration_generation) override {
        if (auto valid = validate_stream_selection(selected); !valid) return valid.error();
        std::lock_guard lock(state_->mutex);
        if (state_->closed) return error(ErrorCode::disconnected, "Synthetic camera is closed");

        struct Pending {
            std::size_t index;
            std::vector<Slot> slots;
            std::unique_ptr<SyntheticStream> receiver;
        };
        try {
            std::vector<Pending> pending;
            // Resolve all IDs before any source allocation. Selection order
            // need not be topology order or reflect the numeric ID values.
            for (auto id : selected) {
                const auto found = std::find_if(state_->sources.begin(), state_->sources.end(),
                    [&](const auto& source) { return source.product.source == id; });
                if (found == state_->sources.end()) return error(ErrorCode::not_found, "Unknown synthetic stream ID");
                if (found->receiver_open) return error(ErrorCode::busy, "Synthetic receiver already admitted");
                pending.push_back({static_cast<std::size_t>(found - state_->sources.begin()), {}, {}});
            }
            std::vector<std::unique_ptr<IFrameStream>> receivers;
            receivers.reserve(pending.size());
            for (auto& source : pending) {
                auto slots = state_->prepare(state_->sources[source.index]);
                if (!slots) return slots.error();
                source.slots = std::move(slots).value();
                source.receiver = std::make_unique<SyntheticStream>(state_, source.index);
            }

            // All potentially failing work has completed. Commit under the
            // device lock using one start time, then publish the ready handles.
            const auto now = Clock::now();
            for (auto& source : pending) {
                state_->activate(state_->sources[source.index], source.slots, configuration_generation, now);
                source.receiver->admit();
                receivers.push_back(std::move(source.receiver));
            }
            return receivers;
        } catch (const std::bad_alloc&) {
            return error(ErrorCode::resource_exhausted, "Could not prepare synthetic acquisition receivers");
        }
    }

    Status close() override {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
        for (auto& source : state_->sources) state_->retire(source);
        return {};
    }

private:
    std::shared_ptr<DeviceState> state_;
};

class SyntheticBackend final : public ICameraBackend {
public:
    explicit SyntheticBackend(SyntheticOptions options, bool multi = false)
        : options_(options), multi_(multi) {}

    std::string_view id() const noexcept override { return multi_ ? "synthetic-multi" : "synthetic"; }

    Result<std::vector<DeviceInfo>> enumerate() override {
        if (multi_) return std::vector<DeviceInfo>{{"synthetic-multi", "synthetic-multi-0",
                                                   "Deterministic two-stream camera"}};
        return std::vector<DeviceInfo>{{"synthetic", "synthetic-0",
            options_.pixel_format == PixelFormat::mono12 ? "Deterministic Mono12 camera" : "Deterministic Mono8 camera"}};
    }

    Result<std::shared_ptr<ICameraDevice>> open(std::string_view device_id) override {
        if (device_id != (multi_ ? "synthetic-multi-0" : "synthetic-0"))
            return error(ErrorCode::not_found, "Unknown synthetic device ID for the selected fixture");
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
            auto topology = multi_ ? synthetic::multi_stream_topology() : StreamTopology{{{StreamId{}}}, {
                {ImageProductId{}, StreamId{}, options_.pixel_format, options_.initial_configuration, {}, {}}
            }};
            if (auto valid_topology = validate_stream_topology(topology); !valid_topology) {
                if (!multi_) return error(ErrorCode::unsupported, "Unsupported synthetic pixel profile");
                return valid_topology.error();
            }
            auto state = std::make_shared<DeviceState>(options_, topology);
            std::shared_ptr<ICameraDevice> device = std::make_shared<SyntheticDevice>(state);
            opened_device_ = std::move(state);
            return device;
        } catch (const std::bad_alloc&) {
            return error(ErrorCode::resource_exhausted, "Could not allocate a synthetic camera");
        }
    }

private:
    SyntheticOptions options_;
    const bool multi_;
    std::mutex mutex_;
    std::weak_ptr<DeviceState> opened_device_;
};

} // namespace

std::shared_ptr<ICameraBackend> make_synthetic_backend(SyntheticOptions options) {
    return std::make_shared<SyntheticBackend>(options);
}

std::shared_ptr<ICameraBackend> make_synthetic_multi_stream_backend(std::size_t frame_pool_size) {
    return std::make_shared<SyntheticBackend>(SyntheticOptions{frame_pool_size}, true);
}

} // namespace gige_mel::backend
