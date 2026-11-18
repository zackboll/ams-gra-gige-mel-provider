#pragma once
#include <gige_mel/backend.hpp>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace recording_stack {
namespace b = gige_mel::backend;
using namespace std::chrono_literals;
inline b::StackSupport support() {
    b::StackSupport s;
    s.supported = true;
    s.width = 4; s.height = 3; s.depth = 2;
    s.storage_bits = 8; s.logical_bits = 8; s.packed_bytes = 24;
    s.source_generation = 17;
    s.layout = b::StackLayout::contiguous_frame_major_row_major_mono8;
    s.source_id = "recording-stack-v1";
    s.formation_meaning = "Two pre-rendered layers of one test volume: low/high test intensity planes, not camera frames or qualified TSI";
    s.identity_scheme = "Test recording capability/activity UUID pair and record key; IDs have no mission meaning";
    s.capability.uuid.back() = 41; s.capability.label = "test-volume";
    s.activity.uuid.back() = 42; s.activity.label = "test-recording";
    s.platform_id = 1234; s.txa_fom = 3;
    s.fom_meaning = "Fixture-declared TXA test quality rank 3 on upstream 1(best)-10 scale; no measured sensor performance";
    s.mask_information_absent = true;
    return s;
}
inline std::shared_ptr<const std::vector<std::byte>> pixels() {
    return std::make_shared<const std::vector<std::byte>>(std::initializer_list<std::byte>{
        std::byte{1},std::byte{2},std::byte{3},std::byte{4},std::byte{5},std::byte{6},
        std::byte{7},std::byte{8},std::byte{9},std::byte{10},std::byte{11},std::byte{12},
        std::byte{101},std::byte{102},std::byte{103},std::byte{104},std::byte{105},std::byte{106},
        std::byte{107},std::byte{108},std::byte{109},std::byte{110},std::byte{111},std::byte{112}});
}
struct Subscription {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<b::StackLease> pending;
    bool stopped{false}, fail{false};
    unsigned receives{0}, destroyed{0};
    std::uint64_t configuration{};
    void wait(unsigned count) {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 3s, [&] { return receives >= count || stopped; }))
            throw std::runtime_error("stack receive watchdog");
        if (receives < count) throw std::runtime_error("stack stopped before receive barrier");
    }
    void wait_stop() {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 3s, [&] { return stopped; })) throw std::runtime_error("stack stop watchdog");
    }
    unsigned deliver(b::StackSupport s = support(), std::uint32_t id = 701,
                 std::optional<std::uint64_t> generation = {}, std::size_t size = 24, bool owner = true) {
        auto bytes = pixels();
        // A later record deliberately differs: held-buffer tests must detect
        // an overwrite, not merely an extra callback or identical-byte copy.
        if (id == 702) bytes = std::make_shared<const std::vector<std::byte>>(24, std::byte{238});
        if (size > bytes->size()) throw std::runtime_error("invalid fixture size");
        std::lock_guard lock(mutex);
        if (pending) throw std::runtime_error("fixture queue full");
        pending.emplace(b::StackDescriptor{std::move(s), id, generation.value_or(configuration)},
            std::span<const std::byte>{bytes->data(), size}, owner ? bytes : nullptr);
        changed.notify_all();
        // The next receive entry proves processing of this pending item ended.
        // Captured under the receive mutex: timeout polling cannot fake it.
        return receives + 1;
    }
};
class Stream final : public b::IStackedImageStream {
    std::shared_ptr<Subscription> state_;
public:
    explicit Stream(std::shared_ptr<Subscription> s) : state_(std::move(s)) {}
    ~Stream() override { request_stop(); std::lock_guard lock(state_->mutex); ++state_->destroyed; }
    b::Result<b::StackLease> receive(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(state_->mutex);
        ++state_->receives; state_->changed.notify_all();
        if (!state_->changed.wait_for(lock, timeout, [&] { return state_->stopped || state_->fail || state_->pending; }))
            return b::Error{b::ErrorCode::timeout, "recording timeout"};
        if (state_->stopped) return b::Error{b::ErrorCode::stopped, "recording cancelled"};
        if (state_->fail) return b::Error{b::ErrorCode::io_error, "recording failure"};
        auto lease = std::move(*state_->pending); state_->pending.reset();
        return lease;
    }
    void request_stop() noexcept override {
        std::lock_guard lock(state_->mutex); state_->stopped = true; state_->changed.notify_all();
    }
};
struct Recording {
    b::StackSupport declarations{support()};
    std::shared_ptr<Subscription> latest;
    unsigned opens{0}, image_streams{0};
    bool fail_open{false};
};
class Device final : public b::ICameraDevice, public b::IStackedImageSource {
    std::shared_ptr<b::ICameraDevice> camera_;
    std::shared_ptr<Recording> recording_;
public:
    Device(std::shared_ptr<b::ICameraDevice> c, std::shared_ptr<Recording> r)
        : camera_(std::move(c)), recording_(std::move(r)) {}
    b::CameraCapabilities capabilities() const override { return camera_->capabilities(); }
    b::Result<b::CameraConfiguration> configuration() const override { return camera_->configuration(); }
    b::Result<b::CameraConfiguration> apply(const b::CameraConfiguration& c) override { return camera_->apply(c); }
    b::Result<std::unique_ptr<b::IFrameStream>> create_stream() override {
        ++recording_->image_streams; return camera_->create_stream();
    }
    b::Status close() override { return camera_->close(); }
    b::StackSupport stacked_image_support() const override { return recording_->declarations; }
    b::Result<std::unique_ptr<b::IStackedImageStream>> open_stacked_images(std::uint64_t generation) override {
        ++recording_->opens;
        if (recording_->fail_open) return b::Error{b::ErrorCode::io_error, "fixture open failed"};
        auto s = std::make_shared<Subscription>(); s->configuration = generation; recording_->latest = s;
        return std::unique_ptr<b::IStackedImageStream>{std::make_unique<Stream>(s)};
    }
};
class Backend final : public b::ICameraBackend {
    std::shared_ptr<b::ICameraBackend> camera_{b::make_synthetic_backend()};
    std::shared_ptr<Recording> recording_;
public:
    explicit Backend(std::shared_ptr<Recording> r) : recording_(std::move(r)) {}
    std::string_view id() const noexcept override { return "recording-stack"; }
    b::Result<std::vector<b::DeviceInfo>> enumerate() override { return camera_->enumerate(); }
    b::Result<std::shared_ptr<b::ICameraDevice>> open(std::string_view id) override {
        auto c = camera_->open(id); if (!c) return c.error();
        return std::shared_ptr<b::ICameraDevice>{std::make_shared<Device>(std::move(c).value(), recording_)};
    }
};
} // namespace recording_stack