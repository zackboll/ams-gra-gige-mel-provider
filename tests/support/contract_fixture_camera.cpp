// Independent test-only implementation of the project backend contract. See the
// header for the documented fixture semantics. It shares no code with, and does
// not delegate to, src/backends/synthetic.cpp.
#include "contract_fixture_camera.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace contract_fixture {
namespace {

namespace be = gige_mel::backend;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::chrono::nanoseconds minimum_exposure = 100us;
constexpr std::chrono::milliseconds maximum_receive_wait = 24h;

be::Error fail(be::ErrorCode code, std::string message) { return be::Error{code, std::move(message)}; }

be::CameraCapabilities fixture_capabilities() {
    be::CameraCapabilities caps;
    caps.sensor_width = 384;
    caps.sensor_height = 256;
    caps.width_increment = 4;
    caps.height_increment = 2;
    caps.offset_x_increment = 2;
    caps.offset_y_increment = 2;
    caps.minimum_frame_rate_hz = 10;
    caps.maximum_frame_rate_hz = 90;
    caps.minimum_exposure = minimum_exposure;
    caps.mono8 = true;
    caps.external_trigger = false;
    caps.external_buffer_acquisition = false;
    return caps;
}

be::CameraConfiguration fixture_initial_configuration() {
    return {80, 60, 4, 6, 40, std::chrono::nanoseconds{1'250'000}};
}

std::chrono::nanoseconds frame_period(std::uint32_t rate_hz) {
    return std::chrono::nanoseconds{1'000'000'000LL / rate_hz};
}

// One bounded source pool, allocated by start(). `leased` is guarded by the
// owning Shared::mutex; slot bytes are written only by receive() while the slot
// is not leased, so a lease's bytes are immutable until its owner is released.
struct Pool {
    Pool(std::size_t slots, std::size_t bytes, std::shared_ptr<Observation> observed)
        : storage(slots, std::vector<std::byte>(bytes)), leased(slots, false), observation(std::move(observed)) {
        ++observation->pools_created;
    }
    ~Pool() { ++observation->pools_retired; }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    std::vector<std::vector<std::byte>> storage;
    std::vector<bool> leased;
    std::shared_ptr<Observation> observation;
};

// Per-device state shared by the device, its stream and every lease owner.
struct Shared {
    explicit Shared(std::shared_ptr<Observation> observed) : observation(std::move(observed)) {}
    ~Shared() { ++observation->shared_states_retired; }
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;

    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<Observation> observation;
    // Everything below is guarded by mutex.
    bool closed{false};
    be::CameraConfiguration configuration{fixture_initial_configuration()};
    std::uint64_t next_native_id{first_native_frame_id};
    std::uint64_t next_stream_generation{first_stream_generation};
    std::int64_t virtual_clock_ns{virtual_clock_origin_ns};
    std::uint64_t live_stream{0}; // token of the one live stream object; 0 if none
    std::uint64_t next_stream_token{1};
    bool running{false};
    bool stop_requested{false};
    std::shared_ptr<Pool> pool; // current pool; dropped by logical close
    be::CameraConfiguration running_configuration{};
    std::uint64_t configuration_generation{0};
    std::uint64_t stream_generation{0};
    Clock::time_point next_due{};
    be::StreamStatistics statistics{};
};

bool any_leased(const std::shared_ptr<Pool>& pool) {
    return pool && std::find(pool->leased.begin(), pool->leased.end(), true) != pool->leased.end();
}

// Opaque lease owner: retains the pool storage and the shared state that final
// release touches, independent of stream/device/backend lifetime. Its
// destructor may run on any thread and never throws.
struct LeaseOwner {
    LeaseOwner(std::shared_ptr<Shared> state, std::shared_ptr<Pool> source, std::size_t index) noexcept
        : shared(std::move(state)), pool(std::move(source)), slot(index) {}
    ~LeaseOwner() {
        {
            std::lock_guard lock(shared->mutex);
            pool->leased[slot] = false;
        }
        shared->changed.notify_all();
    }
    LeaseOwner(const LeaseOwner&) = delete;
    LeaseOwner& operator=(const LeaseOwner&) = delete;

    std::shared_ptr<Shared> shared;
    std::shared_ptr<Pool> pool;
    std::size_t slot;
};

class FixtureStream final : public be::IFrameStream {
public:
    FixtureStream(std::shared_ptr<Shared> shared, std::uint64_t token, std::size_t slots)
        : shared_(std::move(shared)), token_(token), slots_(slots) {}
    ~FixtureStream() override {
        stop();
        {
            std::lock_guard lock(shared_->mutex);
            if (shared_->live_stream == token_) shared_->live_stream = 0;
        }
        ++shared_->observation->streams_retired;
    }
    FixtureStream(const FixtureStream&) = delete;
    FixtureStream& operator=(const FixtureStream&) = delete;

    be::Status start(std::uint64_t configuration_generation) override {
        std::lock_guard lock(shared_->mutex);
        if (shared_->closed) return fail(be::ErrorCode::disconnected, "contract fixture camera is logically closed");
        if (shared_->live_stream != token_) return fail(be::ErrorCode::invalid_state, "contract fixture stream is no longer current");
        if (shared_->running) return fail(be::ErrorCode::invalid_state, "contract fixture stream is already running");
        if (any_leased(shared_->pool))
            return fail(be::ErrorCode::resource_exhausted, "release outstanding contract fixture source leases before restarting");
        const auto& c = shared_->configuration;
        try {
            // The replaced pool has no leases, so its destructor takes no lock.
            shared_->pool = std::make_shared<Pool>(slots_, row_stride(c.width) * c.height, shared_->observation);
        } catch (const std::bad_alloc&) {
            return fail(be::ErrorCode::resource_exhausted, "could not allocate the contract fixture source pool");
        }
        shared_->running_configuration = c;
        shared_->configuration_generation = configuration_generation;
        shared_->stream_generation = shared_->next_stream_generation++;
        shared_->statistics = {};
        shared_->stop_requested = false;
        shared_->running = true;
        shared_->next_due = Clock::now() + c.exposure;
        return {};
    }

    be::Result<be::FrameLease> receive(std::chrono::milliseconds timeout) override {
        if (timeout < 0ms) return fail(be::ErrorCode::invalid_argument, "receive timeout must not be negative");
        const auto deadline = Clock::now() + std::min(timeout, maximum_receive_wait);
        std::unique_lock lock(shared_->mutex);
        bool starvation_counted = false;
        for (;;) {
            if (shared_->closed) return fail(be::ErrorCode::disconnected, "contract fixture camera was logically closed");
            if (shared_->live_stream != token_ || !shared_->running || shared_->stop_requested)
                return fail(be::ErrorCode::stopped, "contract fixture stream is stopped");
            auto& pool = *shared_->pool;
            const auto free_slot = std::find(pool.leased.begin(), pool.leased.end(), false);
            const auto now = Clock::now();
            if (free_slot != pool.leased.end() && now >= shared_->next_due)
                return produce(lock, free_slot - pool.leased.begin(), now);
            if (free_slot == pool.leased.end() && !starvation_counted) {
                ++shared_->statistics.pool_starvation_events;
                starvation_counted = true;
            }
            if (now >= deadline) {
                ++shared_->statistics.timeout_count;
                return fail(be::ErrorCode::timeout, "timed out waiting for a contract fixture frame or free source slot");
            }
            const auto wake = free_slot == pool.leased.end() ? deadline : std::min(deadline, shared_->next_due);
            ++shared_->observation->receive_waits;
            shared_->changed.wait_until(lock, wake);
        }
    }

    void request_stop() noexcept override {
        {
            std::lock_guard lock(shared_->mutex);
            if (shared_->live_stream == token_) {
                shared_->stop_requested = true;
                shared_->running = false;
            }
        }
        shared_->changed.notify_all();
    }
    void stop() noexcept override { request_stop(); }

    be::StreamStatistics statistics() const override {
        std::lock_guard lock(shared_->mutex);
        return shared_->statistics;
    }

private:
    be::Result<be::FrameLease> produce(std::unique_lock<std::mutex>& lock, std::ptrdiff_t position, Clock::time_point now);

    std::shared_ptr<Shared> shared_;
    std::uint64_t token_;
    std::size_t slots_;
};

// Fills a free slot and returns its lease. The caller holds the device mutex.
be::Result<be::FrameLease> FixtureStream::produce(std::unique_lock<std::mutex>& lock, std::ptrdiff_t position,
                                                  Clock::time_point now) {
    const auto index = static_cast<std::size_t>(position);
    std::shared_ptr<LeaseOwner> owner;
    try {
        owner = std::make_shared<LeaseOwner>(shared_, shared_->pool, index);
    } catch (const std::bad_alloc&) {
        return fail(be::ErrorCode::resource_exhausted, "could not allocate a contract fixture lease");
    }
    // Nothing below throws, so the owner (whose destructor takes this mutex) is
    // never destroyed while the mutex is held.
    auto& pool = *shared_->pool;
    pool.leased[index] = true;
    const auto c = shared_->running_configuration;
    const auto stride = row_stride(c.width);
    const auto id = shared_->next_native_id;
    shared_->next_native_id += native_frame_id_step;
    auto& bytes = pool.storage[index];
    for (std::uint32_t y = 0; y < c.height; ++y) {
        auto* row = bytes.data() + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < c.width; ++x)
            row[x] = static_cast<std::byte>(pixel(c.offset_x + x, c.offset_y + y, id));
        std::fill(row + c.width, row + stride, padding_byte);
    }
    be::FrameDescriptor d;
    d.width = c.width;
    d.height = c.height;
    d.row_stride_bytes = stride;
    d.storage_bits_per_pixel = 8;
    d.significant_bits_per_pixel = 8;
    d.bands = 1;
    d.pixel_format = be::PixelFormat::mono8;
    d.byte_order = be::ByteOrder::not_applicable;
    d.status = be::FrameStatus::complete;
    d.native_frame_id = id;
    d.stream_generation = shared_->stream_generation;
    d.configuration_generation = shared_->configuration_generation;
    d.timestamp.value = std::chrono::nanoseconds{shared_->virtual_clock_ns};
    d.timestamp.domain = be::ClockDomain::device;
    d.timestamp.event = be::TimestampEvent::integration_start;
    d.timestamp.valid = true;
    d.timestamp.clock_generation = clock_generation;
    d.configuration = c;
    shared_->virtual_clock_ns += frame_period(c.frame_rate_hz).count();
    shared_->next_due = now + frame_period(c.frame_rate_hz);
    ++shared_->statistics.delivered_frames;
    be::FrameLease lease{d, std::span<const std::byte>{bytes}, std::move(owner)};
    lock.unlock();
    return lease;
}

class FixtureDevice final : public be::ICameraDevice {
public:
    FixtureDevice(std::shared_ptr<Shared> shared, std::size_t slots) : shared_(std::move(shared)), slots_(slots) {}
    ~FixtureDevice() override {
        (void)close();
        ++shared_->observation->devices_retired;
    }
    FixtureDevice(const FixtureDevice&) = delete;
    FixtureDevice& operator=(const FixtureDevice&) = delete;

    be::CameraCapabilities capabilities() const override { return fixture_capabilities(); }

    be::Result<be::CameraConfiguration> configuration() const override {
        std::lock_guard lock(shared_->mutex);
        if (shared_->closed) return fail(be::ErrorCode::disconnected, "contract fixture camera is logically closed");
        return shared_->configuration;
    }

    // Writes ROI, then rate, then exposure. Each group is validated just before
    // it is written, so a later rejection leaves earlier groups applied.
    be::Result<be::CameraConfiguration> apply(const be::CameraConfiguration& c) override {
        std::lock_guard lock(shared_->mutex);
        if (shared_->closed) return fail(be::ErrorCode::disconnected, "contract fixture camera is logically closed");
        if (shared_->running) return fail(be::ErrorCode::invalid_state, "stop the contract fixture stream before applying configuration");
        const auto caps = fixture_capabilities();
        const bool roi_valid = c.width > 0 && c.height > 0 && c.width % caps.width_increment == 0 &&
            c.height % caps.height_increment == 0 && c.offset_x % caps.offset_x_increment == 0 &&
            c.offset_y % caps.offset_y_increment == 0 && c.width <= caps.sensor_width &&
            c.height <= caps.sensor_height && c.offset_x <= caps.sensor_width - c.width &&
            c.offset_y <= caps.sensor_height - c.height;
        if (!roi_valid)
            return fail(be::ErrorCode::invalid_argument,
                        "fixture ROI needs width multiple of 4, height multiple of 2 and even offsets inside 384x256; nothing written");
        auto& actual = shared_->configuration;
        actual.width = c.width;
        actual.height = c.height;
        actual.offset_x = c.offset_x;
        actual.offset_y = c.offset_y;
        if (c.frame_rate_hz < caps.minimum_frame_rate_hz || c.frame_rate_hz > caps.maximum_frame_rate_hz)
            return fail(be::ErrorCode::invalid_argument, "fixture frame rate must be 10-90 Hz; ROI was already written");
        actual.frame_rate_hz = c.frame_rate_hz;
        if (c.exposure < caps.minimum_exposure || c.exposure > frame_period(c.frame_rate_hz))
            return fail(be::ErrorCode::invalid_argument,
                        "fixture exposure must be >= 100 us and within one frame period; ROI and rate were already written");
        actual.exposure = c.exposure;
        return actual;
    }

    be::Result<std::unique_ptr<be::IFrameStream>> create_stream() override {
        std::lock_guard lock(shared_->mutex);
        if (shared_->closed) return fail(be::ErrorCode::disconnected, "contract fixture camera is logically closed");
        if (shared_->live_stream != 0) return fail(be::ErrorCode::busy, "the contract fixture supports one live stream object");
        const auto token = shared_->next_stream_token++;
        std::unique_ptr<be::IFrameStream> stream;
        try {
            stream = std::make_unique<FixtureStream>(shared_, token, slots_);
        } catch (const std::bad_alloc&) {
            return fail(be::ErrorCode::resource_exhausted, "could not allocate a contract fixture stream");
        }
        shared_->live_stream = token;
        return stream;
    }

    // Logical close: wakes receive, refuses further I/O and drops the device's
    // pool reference. Outstanding leases keep their pool and shared state.
    be::Status close() override {
        std::shared_ptr<Pool> retired; // released after the mutex
        {
            std::lock_guard lock(shared_->mutex);
            shared_->closed = true;
            shared_->running = false;
            shared_->stop_requested = true;
            retired = std::move(shared_->pool);
        }
        shared_->changed.notify_all();
        return {};
    }

private:
    std::shared_ptr<Shared> shared_;
    std::size_t slots_;
};

class FixtureBackend final : public be::ICameraBackend {
public:
    FixtureBackend(Options options, std::shared_ptr<Observation> observation)
        : options_(options), observation_(std::move(observation)) {}
    ~FixtureBackend() override { ++observation_->backends_retired; }
    FixtureBackend(const FixtureBackend&) = delete;
    FixtureBackend& operator=(const FixtureBackend&) = delete;

    std::string_view id() const noexcept override { return backend_id; }

    be::Result<std::vector<be::DeviceInfo>> enumerate() override {
        return std::vector<be::DeviceInfo>{{backend_id, device_id, "Contract fixture virtual Mono8 camera (test only)"}};
    }

    be::Result<std::shared_ptr<be::ICameraDevice>> open(std::string_view requested) override {
        if (requested != device_id) return fail(be::ErrorCode::not_found, "contract fixture device ID must be fixture-camera-17");
        if (options_.source_slots == 0 || options_.source_slots > 16)
            return fail(be::ErrorCode::invalid_argument, "contract fixture source pool must have 1-16 slots");
        std::lock_guard lock(mutex_);
        if (const auto current = opened_.lock()) {
            std::lock_guard device_lock(current->mutex);
            if (!current->closed) return fail(be::ErrorCode::busy, "contract fixture camera is already open");
        }
        try {
            auto shared = std::make_shared<Shared>(observation_);
            std::shared_ptr<be::ICameraDevice> device = std::make_shared<FixtureDevice>(shared, options_.source_slots);
            opened_ = shared;
            return device;
        } catch (const std::bad_alloc&) {
            return fail(be::ErrorCode::resource_exhausted, "could not allocate the contract fixture device");
        }
    }

private:
    Options options_;
    std::shared_ptr<Observation> observation_;
    std::mutex mutex_;
    std::weak_ptr<Shared> opened_; // observation only; never extends lifetime
};

} // namespace

std::shared_ptr<gige_mel::backend::ICameraBackend> make_backend(Options options, std::shared_ptr<Observation> observation) {
    if (!observation) observation = std::make_shared<Observation>();
    return std::make_shared<FixtureBackend>(options, std::move(observation));
}

} // namespace contract_fixture
