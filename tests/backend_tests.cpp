#include <gige_mel/backend.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace gige_mel::backend;

namespace {

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression)) {                                                                         \
            throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) +           \
                                     ": " #expression);                                              \
        }                                                                                            \
    } while (false)

struct Camera {
    std::shared_ptr<ICameraBackend> backend;
    std::shared_ptr<ICameraDevice> device;
    std::unique_ptr<IFrameStream> stream;

    explicit Camera(std::size_t pool_size = 4) {
        SyntheticOptions options;
        options.frame_pool_size = pool_size;
        options.initial_configuration.frame_rate_hz = 120;
        backend = make_synthetic_backend(options);
        auto opened = backend->open("synthetic-0");
        CHECK(opened);
        device = std::move(opened).value();
        auto created = device->create_stream();
        CHECK(created);
        stream = std::move(created).value();
    }

    FrameLease next() {
        auto received = stream->receive(500ms);
        CHECK(received);
        return std::move(received).value();
    }
};

void wait_for_starvation(IFrameStream& stream) {
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (stream.statistics().pool_starvation_events == 0) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

void discovery_and_exclusive_open() {
    auto backend = make_synthetic_backend();
    CHECK(backend->id() == "synthetic");
    const auto found = backend->enumerate();
    CHECK(found);
    CHECK(found.value().size() == 1);
    CHECK(found.value().front().device_id == "synthetic-0");
    const auto missing = backend->open("missing");
    CHECK(!missing);
    CHECK(missing.error().code == ErrorCode::not_found);
    auto opened = backend->open("synthetic-0");
    CHECK(opened);
    CHECK(opened.value()->configuration().value() == CameraConfiguration{});
    const auto duplicate = backend->open("synthetic-0");
    CHECK(!duplicate);
    CHECK(duplicate.error().code == ErrorCode::busy);
    CHECK(opened.value()->close());
    CHECK(opened.value()->close());
    CHECK(backend->open("synthetic-0"));

    SyntheticOptions invalid;
    invalid.frame_pool_size = 0;
    CHECK(!make_synthetic_backend(invalid)->open("synthetic-0"));
    invalid.frame_pool_size = 65;
    CHECK(!make_synthetic_backend(invalid)->open("synthetic-0"));
    invalid.frame_pool_size = 1;
    invalid.initial_configuration.width = 0;
    CHECK(!make_synthetic_backend(invalid)->open("synthetic-0"));
}

void configuration_validation_is_atomic() {
    Camera camera;
    auto config = camera.device->configuration().value();
    config.width = 32;
    config.height = 20;
    config.offset_x = 608;
    config.offset_y = 460;
    config.exposure = 2ms;
    const auto actual = camera.device->apply(config);
    CHECK(actual);
    CHECK(actual.value() == config);

    auto invalid = config;
    invalid.offset_x = 609;
    CHECK(!camera.device->apply(invalid));
    invalid = config;
    invalid.width = UINT32_MAX;
    CHECK(!camera.device->apply(invalid));
    invalid = config;
    invalid.exposure = 9ms;
    CHECK(!camera.device->apply(invalid));
    invalid = config;
    invalid.frame_rate_hz = 0;
    CHECK(!camera.device->apply(invalid));
    invalid = config;
    invalid.exposure = 0ns;
    CHECK(!camera.device->apply(invalid));
    CHECK(camera.device->configuration().value() == config);
    CHECK(!camera.device->create_stream());

    CHECK(camera.stream->start(7));
    CHECK(!camera.stream->start(7));
    CHECK(!camera.device->apply(config));
    auto frame = camera.next();
    CHECK(frame.descriptor().configuration == config);
    CHECK(frame.descriptor().configuration_generation == 7);
    camera.stream->stop();
    CHECK(camera.device->apply(config));
}

void frame_metadata_and_retention() {
    Camera camera;
    CHECK(!camera.stream->receive(0ms));
    CHECK(camera.stream->start(42));
    auto first = camera.next();
    const auto& metadata = first.descriptor();
    CHECK(metadata.width == 64);
    CHECK(metadata.height == 48);
    CHECK(metadata.row_stride_bytes == 64);
    CHECK(metadata.configuration_generation == 42);
    CHECK(metadata.stream_generation == 1);
    CHECK(metadata.storage_bits_per_pixel == 8);
    CHECK(metadata.significant_bits_per_pixel == 8);
    CHECK(metadata.bands == 1);
    CHECK(metadata.status == FrameStatus::complete);
    CHECK(metadata.timestamp.valid);
    CHECK(metadata.timestamp.domain == ClockDomain::host_monotonic);
    CHECK(metadata.timestamp.event == TimestampEvent::integration_start);
    CHECK(metadata.timestamp.clock_generation == 1);
    CHECK(first.bytes().size() == 64 * 48);
    for (std::uint32_t y = 0; y < metadata.height; ++y) {
        for (std::uint32_t x = 0; x < metadata.width; ++x) {
            const auto expected = static_cast<std::byte>((x + y + metadata.native_frame_id) & 0xffU);
            CHECK(first.bytes()[static_cast<std::size_t>(y) * metadata.width + x] == expected);
        }
    }
    const std::vector<std::byte> snapshot(first.bytes().begin(), first.bytes().end());
    const auto second = camera.next();
    CHECK(second.descriptor().native_frame_id == metadata.native_frame_id + 1);
    CHECK(second.descriptor().timestamp.value > metadata.timestamp.value);
    CHECK(std::equal(first.bytes().begin(), first.bytes().end(), snapshot.begin()));
    CHECK(first.bytes().data() != second.bytes().data());
    const auto negative_timeout = camera.stream->receive(-1ms);
    CHECK(!negative_timeout);
    CHECK(negative_timeout.error().code == ErrorCode::invalid_argument);
}

void bounded_pool_release_and_generation() {
    Camera camera(2);
    CHECK(camera.stream->start(1));
    std::optional<FrameLease> first{camera.next()};
    std::optional<FrameLease> second{camera.next()};
    const auto exhausted = camera.stream->receive(5ms);
    CHECK(!exhausted);
    CHECK(exhausted.error().code == ErrorCode::timeout);
    CHECK(camera.stream->statistics().pool_starvation_events == 1);
    CHECK(camera.stream->statistics().timeout_count == 1);
    first.reset();
    first.emplace(camera.next());
    CHECK(first->descriptor().native_frame_id == 3);
    camera.stream->stop();
    auto config = camera.device->configuration().value();
    config.width = 10;
    config.height = 8;
    CHECK(camera.device->apply(config));
    const auto still_held = camera.stream->start(2);
    CHECK(!still_held);
    CHECK(still_held.error().code == ErrorCode::resource_exhausted);
    CHECK(second->descriptor().width == 64);
    CHECK(second->descriptor().configuration_generation == 1);
    CHECK(second->bytes().size() == 64 * 48);
    first.reset();
    second.reset();
    CHECK(camera.stream->start(2));
    const auto changed = camera.next();
    CHECK(changed.descriptor().width == 10);
    CHECK(changed.descriptor().height == 8);
    CHECK(changed.descriptor().stream_generation == 2);
    CHECK(changed.descriptor().configuration_generation == 2);
    CHECK(changed.bytes().size() == 80);
}

void releasing_a_lease_wakes_receive() {
    Camera camera(1);
    CHECK(camera.stream->start(1));
    std::optional<FrameLease> held{camera.next()};
    auto received = std::async(std::launch::async, [&] { return camera.stream->receive(3s); });
    wait_for_starvation(*camera.stream);
    held.reset();
    CHECK(received.wait_for(500ms) == std::future_status::ready);
    auto result = received.get();
    CHECK(result);
    CHECK(result.value().descriptor().native_frame_id == 2);
}

void stop_wakes_receive_without_invalidating_a_lease() {
    Camera camera(1);
    CHECK(camera.stream->start(1));
    auto held = camera.next();
    const auto first_byte = held.bytes().front();
    auto received = std::async(std::launch::async, [&] { return camera.stream->receive(3s); });
    wait_for_starvation(*camera.stream);
    camera.stream->request_stop();
    CHECK(received.wait_for(500ms) == std::future_status::ready);
    const auto result = received.get();
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::stopped);
    CHECK(held.bytes().front() == first_byte);
    camera.stream->stop();
    camera.stream->stop();
}

void close_wakes_receive_and_storage_outlives_objects() {
    Camera camera(1);
    CHECK(camera.stream->start(1));
    std::optional<FrameLease> held{camera.next()};
    const std::vector<std::byte> snapshot(held->bytes().begin(), held->bytes().end());
    auto received = std::async(std::launch::async, [&] { return camera.stream->receive(3s); });
    wait_for_starvation(*camera.stream);
    CHECK(camera.device->close());
    CHECK(received.wait_for(500ms) == std::future_status::ready);
    const auto result = received.get();
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::disconnected);
    CHECK(!camera.device->configuration());
    CHECK(!camera.stream->start(2));
    camera.stream.reset();
    camera.device.reset();
    camera.backend.reset();
    CHECK(std::equal(held->bytes().begin(), held->bytes().end(), snapshot.begin()));
    held.reset();
}

void move_lease_does_not_release_early() {
    Camera camera(2);
    CHECK(camera.stream->start(1));
    auto original = camera.next();
    auto moved = std::move(original);
    CHECK(original.bytes().empty());
    CHECK(!moved.bytes().empty());
    auto second = camera.next();
    CHECK(!camera.stream->receive(0ms));
    moved = std::move(second);
    CHECK(second.bytes().empty());
    CHECK(moved.descriptor().native_frame_id == 2);
    const auto third = camera.next();
    CHECK(third.descriptor().native_frame_id == 3);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"discovery and exclusive open", discovery_and_exclusive_open},
        {"configuration validation is atomic", configuration_validation_is_atomic},
        {"metadata and retained images", frame_metadata_and_retention},
        {"bounded pool, release, and generations", bounded_pool_release_and_generation},
        {"release wakes receive", releasing_a_lease_wakes_receive},
        {"stop wakes receive", stop_wakes_receive_without_invalidating_a_lease},
        {"close and retained storage lifetime", close_wakes_receive_and_storage_outlives_objects},
        {"move-only lease ownership", move_lease_does_not_release_early},
    };
    std::size_t passed = 0;
    for (const auto& [name, run] : tests) {
        try {
            run();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& failure) {
            std::cerr << "FAIL " << name << ": " << failure.what() << '\n';
            return 1;
        }
    }
    std::cout << passed << " backend tests passed\n";
    return 0;
}
