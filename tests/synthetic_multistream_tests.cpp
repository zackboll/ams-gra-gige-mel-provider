#include <gige_mel/backend.hpp>

#include <array>
#include <barrier>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using namespace gige_mel::backend;

namespace {

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__ ":") + std::to_string(__LINE__) + ": " #x); } while (false)

template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

template<class T> void expect(const Result<T>& result, ErrorCode code) {
    CHECK(!result);
    CHECK(result.error().code == code);
}

constexpr StreamId zero{0}, one{1};
constexpr ImageProductId mono8{100}, mono12{200};
constexpr std::array only_zero{zero}, only_one{one};
constexpr std::array both{zero, one};
constexpr std::string_view device_id = "synthetic-multi-0";

void topology() {
    const auto backend = make_synthetic_multi_stream_backend(1);
    CHECK(backend->id() == "synthetic-multi");
    const auto devices = take(backend->enumerate());
    CHECK(devices.size() == 1 && devices.front().device_id == device_id);
    expect(backend->open("synthetic-0"), ErrorCode::not_found);
    expect(make_synthetic_multi_stream_backend(0)->open(device_id), ErrorCode::invalid_argument);
    const auto device = take(backend->open(device_id));
    expect(backend->open(device_id), ErrorCode::busy);
    auto t = take(device->stream_topology());
    const auto again = take(device->stream_topology());
    CHECK(t.streams.size() == 2 && t.products.size() == 2);
    CHECK(t.streams[0].id == zero && t.streams[1].id == one);
    CHECK(again.streams[0].id == t.streams[0].id && again.streams[1].id == t.streams[1].id);
    for (std::size_t i = 0; i < t.products.size(); ++i) {
        CHECK(t.products[i].source == t.streams[i].id);
        CHECK(t.products[i].id == again.products[i].id);
    }
    const auto& a = t.products[0];
    const auto& b = t.products[1];
    CHECK(a.id == mono8 && a.pixel_format == PixelFormat::mono8);
    CHECK(b.id == mono12 && b.pixel_format == PixelFormat::mono12);
    CHECK((a.configuration == CameraConfiguration{64, 48, 0, 0, 10}));
    CHECK((b.configuration == CameraConfiguration{80, 60, 0, 0, 5}));
    CHECK(a.row_stride_bytes == 64 && a.span_bytes == 3072);
    CHECK(b.row_stride_bytes == 168 && b.span_bytes == 10072);
    CHECK(validate_stream_topology(t));

    auto invalid = t;
    invalid.streams[1].id = zero;
    expect(validate_stream_topology(invalid), ErrorCode::invalid_argument);
    invalid = t;
    invalid.products[1].id = a.id;
    expect(validate_stream_topology(invalid), ErrorCode::invalid_argument);
    invalid = t;
    invalid.products[1].source = StreamId{88};
    expect(validate_stream_topology(invalid), ErrorCode::invalid_argument);
    invalid = t;
    invalid.products[1].row_stride_bytes = std::numeric_limits<std::size_t>::max();
    expect(validate_stream_topology(invalid), ErrorCode::invalid_argument);
    invalid = t;
    invalid.products[1].span_bytes = *b.span_bytes - 1;
    expect(validate_stream_topology(invalid), ErrorCode::invalid_argument);

    // A future auxiliary product on stream 0 is another mapping, not another receiver.
    auto auxiliary = b;
    auxiliary.source = zero;
    auxiliary.id = ImageProductId{300};
    t.products.push_back(auxiliary);
    CHECK(validate_stream_topology(t));

    expect(device->configuration(), ErrorCode::unsupported);
    expect(device->apply(CameraConfiguration{}), ErrorCode::unsupported);
    expect(device->create_stream(), ErrorCode::unsupported);
    expect(device->start_acquisition({}, 1), ErrorCode::invalid_argument);
    expect(device->start_acquisition(std::array{one, one}, 1), ErrorCode::invalid_argument);
    expect(device->start_acquisition(std::array{zero, StreamId{88}}, 1), ErrorCode::not_found);
    // Neither discovery nor rejected selection reserved a receiver or started a run.
    auto receivers = take(device->start_acquisition(both, 7));
    CHECK(receivers.size() == 2);
    for (auto& receiver : receivers) {
        auto frame = take(receiver->receive(2s));
        CHECK(frame.descriptor().stream_generation == 1);
        CHECK(frame.descriptor().native_frame_id == 1);
    }
}

void check_frame(const FrameLease& frame, const ImageProductDescriptor& product, std::uint64_t config_generation) {
    const auto& d = frame.descriptor();
    const auto& c = product.configuration;
    const auto layout = image_layout(product.pixel_format, c.width, c.height);
    CHECK(layout);
    CHECK(d.source == product.source && d.product == product.id);
    CHECK(d.configuration == c && d.configuration_generation == config_generation);
    CHECK(d.width == c.width && d.height == c.height && d.bands == 1);
    CHECK(d.pixel_format == product.pixel_format && d.byte_order == layout->byte_order);
    CHECK(d.significant_bits_per_pixel == layout->significant_bits && d.storage_bits_per_pixel == layout->storage_bits);
    CHECK(d.row_stride_bytes == product.row_stride_bytes && frame.bytes().size() == product.span_bytes);
    CHECK(d.status == FrameStatus::complete && d.timestamp.valid);
    CHECK(d.timestamp.domain == ClockDomain::host_monotonic && d.timestamp.event == TimestampEvent::integration_start);
    CHECK(d.timestamp.clock_generation == 1 && d.timestamp.value > 0ns);
    const auto id = d.native_frame_id;
    for (std::uint32_t y = 0; y < c.height; ++y) {
        for (std::uint32_t x = 0; x < c.width; ++x) {
            const auto row = y * d.row_stride_bytes;
            if (product.id == mono12) {
                const auto lo = std::to_integer<unsigned>(frame.bytes()[row + 2 * x]);
                const auto hi = std::to_integer<unsigned>(frame.bytes()[row + 2 * x + 1]);
                CHECK((lo | (hi << 8)) == ((17 * x + 31 * y + id) & 4095));
            } else {
                CHECK(std::to_integer<unsigned>(frame.bytes()[row + x]) == ((x + y + id) & 255));
            }
        }
    }
}

void concurrent_delivery_and_restart() {
    auto device = take(make_synthetic_multi_stream_backend(1)->open(device_id));
    const auto t = take(device->stream_topology());
    auto receivers = take(device->start_acquisition(std::array{one, zero}, 11)); // deliberately reversed
    auto& fast = receivers[1];
    auto& slow = receivers[0];
    const auto collect = [&](IFrameStream& receiver, std::size_t index, std::chrono::nanoseconds period) {
        std::chrono::nanoseconds previous{};
        for (std::uint64_t i = 1; i <= 3; ++i) {
            auto frame = take(receiver.receive(2s));
            check_frame(frame, t.products[index], 11);
            const auto& d = frame.descriptor();
            CHECK(d.stream_generation == 1 && d.native_frame_id == i);
            if (i > 1) CHECK(d.timestamp.value - previous >= period);
            // A held lease exhausts only this receiver's single-slot pool.
            expect(receiver.receive(0ms), ErrorCode::timeout);
            previous = d.timestamp.value;
        }
    };
    auto fast_worker = std::async(std::launch::async, [&] { collect(*fast, 0, 100ms); });
    auto slow_worker = std::async(std::launch::async, [&] { collect(*slow, 1, 200ms); });
    fast_worker.get();
    slow_worker.get();

    auto held = std::optional{take(fast->receive(2s))};
    expect(fast->receive(0ms), ErrorCode::timeout); // entire Stream 0 pool pinned
    {
        const auto frame = take(slow->receive(2s));
        check_frame(frame, t.products[1], 11);
        CHECK(frame.descriptor().native_frame_id == 4);
    }
    fast->stop();
    fast->stop();
    expect(fast->receive(0ms), ErrorCode::stopped);
    expect(device->start_acquisition(only_zero, 12), ErrorCode::busy); // stopped handle still admitted
    fast.reset();
    expect(device->start_acquisition(only_zero, 12), ErrorCode::resource_exhausted);
    {
        const auto frame = take(slow->receive(2s));
        CHECK(frame.descriptor().stream_generation == 1 && frame.descriptor().native_frame_id == 5);
    }
    check_frame(*held, t.products[0], 11);
    held.reset();
    auto joined = take(device->start_acquisition(only_zero, 14));
    {
        auto frame = take(joined.front()->receive(2s));
        check_frame(frame, t.products[0], 14);
        CHECK(frame.descriptor().stream_generation == 2 && frame.descriptor().native_frame_id == 5);
    }
    joined.clear();
    CHECK(take(slow->receive(2s)).descriptor().stream_generation == 1);
    slow->stop(); // last active receiver retires
    expect(slow->receive(0ms), ErrorCode::stopped);
    slow.reset();
    auto restarted = take(device->start_acquisition(only_one, 13));
    auto snapshot = take(restarted.front()->receive(2s));
    check_frame(snapshot, t.products[1], 13);
    CHECK(snapshot.descriptor().stream_generation == 2 && snapshot.descriptor().native_frame_id == 7);
}

void partial_startup() {
    auto device = take(make_synthetic_multi_stream_backend(1)->open(device_id));
    const auto t = take(device->stream_topology());
    {
        auto slow = take(device->start_acquisition(only_one, 21));
        auto held = take(slow.front()->receive(2s));
        slow.clear(); // no live receivers; one source pool is still pinned
        // Stream 0 stages successfully before preparation of Stream 1 fails.
        expect(device->start_acquisition(both, 22), ErrorCode::resource_exhausted);
        check_frame(held, t.products[1], 21);
        auto fast = take(device->start_acquisition(only_zero, 23));
        expect(device->start_acquisition(only_zero, 24), ErrorCode::busy);
        expect(device->start_acquisition(only_one, 24), ErrorCode::resource_exhausted);
        auto first = take(fast.front()->receive(2s));
        CHECK(first.descriptor().stream_generation == 1 && first.descriptor().native_frame_id == 1);
        CHECK(first.descriptor().configuration_generation == 23);
    }
    auto receivers = take(device->start_acquisition(both, 25));
    for (auto& receiver : receivers) {
        auto frame = take(receiver->receive(2s));
        CHECK(frame.descriptor().stream_generation == 2 && frame.descriptor().native_frame_id == 2);
    }
}

void wait_for_pool_wait(IFrameStream& stream) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (stream.statistics().pool_starvation_events == 0) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

void cancellation_and_close() {
    const auto backend = make_synthetic_multi_stream_backend(1);
    for (unsigned round = 0; round < 8; ++round) {
        auto device = take(backend->open(device_id));
        const auto t = take(device->stream_topology());
        auto receivers = take(device->start_acquisition(both, 30));
        auto a = take(receivers[0]->receive(2s));
        auto b = take(receivers[1]->receive(2s));

        auto wait0 = std::async(std::launch::async, [&] { return receivers[0]->receive(5s); });
        auto wait1 = std::async(std::launch::async, [&] { return receivers[1]->receive(5s); });
        wait_for_pool_wait(*receivers[0]);
        wait_for_pool_wait(*receivers[1]);
        receivers[0]->request_stop();
        CHECK(wait0.wait_for(2s) == std::future_status::ready);
        expect(wait0.get(), ErrorCode::stopped);
        CHECK(wait1.wait_for(0ms) == std::future_status::timeout);

        std::barrier start(3);
        std::jthread stop([&] { start.arrive_and_wait(); receivers[0]->stop(); });
        std::jthread close([&] { start.arrive_and_wait(); (void)device->close(); });
        start.arrive_and_wait();
        CHECK(device->close());
        stop.join();
        close.join();
        CHECK(wait1.wait_for(2s) == std::future_status::ready);
        expect(wait1.get(), ErrorCode::disconnected);
        expect(device->stream_topology(), ErrorCode::disconnected);
        expect(device->start_acquisition(both, 31), ErrorCode::disconnected);
        for (auto& receiver : receivers) expect(receiver->receive(0ms), ErrorCode::disconnected);
        receivers.clear();
        device.reset();
        auto reopened = take(backend->open(device_id));
        auto fresh = take(reopened->start_acquisition(both, 32));
        for (auto& receiver : fresh) CHECK(take(receiver->receive(2s)).descriptor().stream_generation == 1);
        check_frame(a, t.products[0], 30);
        check_frame(b, t.products[1], 30);
        std::jthread release([retained = std::move(a)] {}); // retire on a different thread
    }
}

} // namespace

int main() {
    try {
        topology();
        concurrent_delivery_and_restart();
        partial_startup();
        cancellation_and_close();
        std::cout << "Synthetic multi-stream topology, acquisition, rollback and ownership passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}