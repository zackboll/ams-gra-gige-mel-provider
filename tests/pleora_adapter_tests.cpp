// Original tests: real SDK memory buffers behind a controlled native source.
// No copied SDK declarations, camera simulation, network or license bypass.
#include "backends/pleora/pleora_backend.hpp"
#include "backends/pleora/pleora_features.hpp"
#include "backends/pleora/pleora_license.hpp"
#include "backends/pleora/pleora_stream.hpp"

#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace b = gige_mel::backend;
namespace p = b::pleora;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

class Source final : public p::NativeSource {
public:
    b::CameraConfiguration config{16, 8, 0, 0, 10, 1ms};
    b::PixelFormat format{b::PixelFormat::mono8};
    PvBuffer buffer;
    bool timeout{false}, incomplete{false}, short_image{false}, fail_release{false}, fail_stop{false};
    unsigned releases{0}, starts{0}, stops{0};
    std::optional<std::size_t> payload_override;
    std::optional<std::uint32_t> operation_override;
    std::promise<void> waiting;
    bool notify{false};
    b::Result<p::RunConfiguration> start() override {
        ++starts;
        auto r = buffer.GetImage()->Alloc(short_image ? 15u : config.width, config.height,
            format == b::PixelFormat::mono12 ? PvPixelMono12 : PvPixelMono8);
        CHECK(r.IsOK());
        std::memset(buffer.GetImage()->GetDataPointer(), 3, buffer.GetImage()->GetImageSize());
        return p::RunConfiguration{config, format,
            payload_override.value_or(b::image_layout(format, config.width, config.height)->image_bytes)};
    }
    b::Result<p::NativeBuffer> next(std::uint32_t ms) override {
        if (notify) { notify = false; waiting.set_value(); }
        if (timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            return b::Error{b::ErrorCode::timeout, "controlled timeout"};
        }
        return p::NativeBuffer{&buffer, PvResult(operation_override.value_or(
            incomplete ? PvResult::Code::MISSING_PACKETS : PvResult::Code::OK))};
    }
    b::Status release(PvBuffer*) override {
        ++releases;
        if (fail_release) return b::Error{b::ErrorCode::io_error, "controlled release failure"};
        return {};
    }
    b::Status stop() override {
        ++stops;
        if (fail_stop) return b::Error{b::ErrorCode::io_error, "controlled stop failure"};
        return {};
    }
};

void leases() {
    auto source = std::make_shared<Source>();
    auto stream = p::make_stream(source);
    CHECK(stream->start(42));
    std::vector<b::FrameLease> retained;
    for (int i = 0; i < 4; ++i) {
        auto f = stream->receive(100ms);
        if (!f) throw std::runtime_error(f.error().message);
        CHECK(!f.value().descriptor().timestamp.valid);
        CHECK(f.value().descriptor().configuration_generation == 42);
        CHECK(f.value().bytes().size() == 128);
        retained.push_back(std::move(f).value());
    }
    auto full = stream->receive(0ms);
    CHECK(!full && full.error().code == b::ErrorCode::resource_exhausted);
    CHECK(stream->statistics().pool_starvation_events == 1);
    CHECK(source->releases == 5);
    std::memset(source->buffer.GetDataPointer(), 9, source->buffer.GetSize());
    for (const auto& f : retained) CHECK(f.bytes()[0] == std::byte{3});
    stream->stop();
    auto restart = stream->start(43);
    CHECK(!restart && restart.error().code == b::ErrorCode::resource_exhausted);
    CHECK(source->starts == 1);
    retained.clear();
    CHECK(stream->start(43));
    auto f = stream->receive(100ms);
    CHECK(f);
    CHECK(f.value().descriptor().stream_generation == 2);
    stream.reset(); source.reset();
    CHECK(f.value().bytes()[0] == std::byte{3});
    // Destruction with an entirely full pool must not invalidate any copy.
    auto second = std::make_shared<Source>();
    auto full_stream = p::make_stream(second);
    CHECK(full_stream->start(44));
    for (int i = 0; i < 4; ++i) {
        auto frame = full_stream->receive(10ms);
        CHECK(frame);
        retained.push_back(std::move(frame).value());
    }
    full_stream.reset(); second.reset();
    for (const auto& frame : retained)
        for (auto byte : frame.bytes()) CHECK(byte == std::byte{3});
}
void failures() {
    for (int mode = 0; mode < 5; ++mode) {
        auto source = std::make_shared<Source>();
        source->timeout = mode == 0;
        source->incomplete = mode == 1;
        source->short_image = mode == 2;
        source->fail_release = mode == 3;
        source->fail_stop = mode == 4;
        auto stream = p::make_stream(source);
        CHECK(stream->start(1));
        if (mode < 4) {
            auto frame = stream->receive(15ms);
            CHECK(!frame);
            if (mode < 2) CHECK(frame.error().code == b::ErrorCode::timeout);
            if (mode == 1) CHECK(stream->statistics().incomplete_frames > 0);
            if (mode > 0) CHECK(source->releases > 0);
        }
        stream->stop();
        if (mode >= 3) CHECK(!stream->start(2));
    }
    auto source = std::make_shared<Source>();
    source->format = b::PixelFormat::mono12;
    auto stream = p::make_stream(source);
    CHECK(stream->start(1));
    CHECK(stream->receive(10ms));
    source->buffer.GetDataPointer()[1] = 0xf0;
    CHECK(!stream->receive(10ms));
}
void cancellation() {
    auto source = std::make_shared<Source>();
    source->timeout = true; source->notify = true;
    auto waiting = source->waiting.get_future();
    auto stream = p::make_stream(source);
    CHECK(stream->start(1));
    auto receiver = std::async(std::launch::async, [&] { return stream->receive(10s); });
    waiting.wait();
    auto begin = std::chrono::steady_clock::now();
    stream->request_stop();
    stream->stop();
    auto result = receiver.get();
    CHECK(!result && result.error().code == b::ErrorCode::stopped);
    CHECK(std::chrono::steady_clock::now() - begin < 500ms);
    CHECK(!stream->receive(-1ms));
    CHECK(stream->start(2));
    source->notify = false;
    auto blocked = std::async(std::launch::async, [&] { return stream->receive(10s); });
    auto observer = std::async(std::launch::async, [&] {
        for (int i = 0; i < 20; ++i) (void)stream->statistics();
    });
    stream->stop(); // stop itself, without an earlier explicit request_stop
    auto stopped = blocked.get();
    observer.get();
    CHECK(!stopped && stopped.error().code == b::ErrorCode::stopped);
}
void budgets_and_native_errors() {
    for (auto bytes : {std::size_t{0}, std::size_t{127}, std::size_t{64 * 1024 * 1024 + 1}}) {
        auto source = std::make_shared<Source>();
        source->payload_override = bytes;
        auto stream = p::make_stream(source);
        CHECK(!stream->start(1));
        CHECK(source->stops == 1);
    }
    for (auto code : {PvResult::Code::NO_LICENSE, PvResult::Code::NOT_CONNECTED}) {
        auto source = std::make_shared<Source>();
        source->operation_override = code;
        auto stream = p::make_stream(source);
        CHECK(stream->start(1));
        auto frame = stream->receive(10ms);
        CHECK(!frame);
        CHECK(frame.error().code == (code == PvResult::Code::NO_LICENSE
            ? b::ErrorCode::unsupported : b::ErrorCode::disconnected));
        CHECK(source->releases == 1);
        stream->stop();
        CHECK(!stream->start(2));
    }
}
void licensing() {
    const p::LicenseStatus unlicensed{false, "Unlicensed. Watermark, limited use"};
    p::Options options;
    CHECK(!options.allow_watermarked_evaluation);
    std::string diagnostics;
    options.diagnostic = [&](std::string_view text) { diagnostics += std::string(text) + '\n'; };
    unsigned connects = 0;
    const auto permitted = [&]() -> b::Status { ++connects; return {}; };
    auto rejected = p::connect_with_license(unlicensed, options, permitted);
    CHECK(!rejected && rejected.error().code == b::ErrorCode::unsupported);
    CHECK(connects == 0);
    CHECK(rejected.error().message == "eBUS SDK license rejected: " + unlicensed.message);
    options.allow_watermarked_evaluation = true;
    CHECK(p::connect_with_license(unlicensed, options, permitted));
    CHECK(connects == 1);
    CHECK(!unlicensed.valid && unlicensed.message == "Unlicensed. Watermark, limited use");
    CHECK(diagnostics.find("valid=false") != std::string::npos);
    CHECK(diagnostics.find("UNLICENSED / WATERMARKED") != std::string::npos);
    CHECK(diagnostics.find("NOT PRODUCTION QUALIFIED") != std::string::npos);
    const auto denial = p::native_error(PvResult(PvResult::Code::NO_LICENSE), "exclusive Connect");
    auto denied = p::connect_with_license(unlicensed, options, [&]() -> b::Status {
        ++connects;
        return denial;
    });
    CHECK(connects == 2); // exactly one attempt, no retry/fallback on denial
    CHECK(!denied && denied.error().code == denial.code && denied.error().message == denial.message);
    CHECK(diagnostics.find(denial.message) != std::string::npos);
    options.allow_watermarked_evaluation = false;
    CHECK(p::connect_with_license({true, "valid"}, options, permitted));
    CHECK(connects == 3);
}
int main() {
    try {
        auto backend = p::make_backend();
        CHECK(backend->id() == "pleora");
        CHECK(!backend->open("192.168.24.101")); // no discovery for invalid identity
        PvGenParameterArray parameters;
        p::Features features(parameters);
        CHECK(features.implemented("missing") && !features.implemented("missing").value());
        CHECK(!features.read_integer("missing"));
        CHECK(!features.write_float("missing", 1));
        CHECK(p::native_error(PvResult(PvResult::Code::NO_LICENSE), "test").code == b::ErrorCode::unsupported);
        CHECK(p::native_error(PvResult(PvResult::Code::NOT_CONNECTED), "test").code == b::ErrorCode::disconnected);
        licensing(); leases(); failures(); cancellation(); budgets_and_native_errors();
        std::cout << "Pleora controlled-source contracts passed (not camera qualification)\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
