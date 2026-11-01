// TEST-ONLY virtual integration-start clock decorator (GIGE-MEL-010). See the
// header: these timestamps are TEST MODEL values, never camera time.
#include "aravis_virtual_clock.hpp"

#include <limits>
#include <string>
#include <utility>

namespace aravis_virtual_clock {
namespace {

// Shared owner of the ORIGINAL lease: the decorated lease's bytes are the
// original span, kept valid by this object. Destroying it ends the original
// lease, whose own owner (the adapter's LeasedBuffer) requeues or frees the
// SDK buffer exactly once.
struct SourceHold {
    SourceHold(backend::FrameLease original, std::shared_ptr<Ledger> observed) noexcept
        : lease(std::move(original)), ledger(std::move(observed)) {}
    ~SourceHold() {
        { backend::FrameLease ended = std::move(lease); } // SDK lease ends here
        ledger->note_release();
    }
    SourceHold(const SourceHold&) = delete;
    SourceHold& operator=(const SourceHold&) = delete;
    backend::FrameLease lease;
    std::shared_ptr<Ledger> ledger;
};

class Stream final : public backend::IFrameStream {
public:
    Stream(std::unique_ptr<backend::IFrameStream> inner, std::shared_ptr<const Clock> clock, std::shared_ptr<Ledger> ledger)
        : inner_(std::move(inner)), clock_(std::move(clock)), ledger_(std::move(ledger)) {}
    backend::Status start(std::uint64_t configuration_generation) override {
        auto started = inner_->start(configuration_generation);
        if (started) {
            sequence_ = 0;
            run_ = ledger_->begin_run();
        }
        return started;
    }
    backend::Result<backend::FrameLease> receive(std::chrono::milliseconds timeout) override {
        auto received = inner_->receive(timeout); // genuine SDK frame or the adapter's error
        if (!received) return received.error();
        backend::FrameLease original = std::move(received).value();
        SourceFrame f;
        f.run = run_;
        f.sequence = sequence_++;
        f.original = original.descriptor();
        f.delivered = f.original; // every field preserved ...
        const auto bytes = original.bytes();
        if (clock_) {
            auto assigned = clock_->assign(f.sequence, f.original.timestamp);
            if (!assigned)
                return backend::Error{backend::ErrorCode::io_error,
                                      "virtual test clock: checked arithmetic overflow at sequence " +
                                          std::to_string(f.sequence)};
            f.delivered.timestamp = *assigned; // ... except the timestamp (TEST MODEL value)
            f.substituted = true;
        }
        f.original_data = f.delivered_data = bytes.data();
        f.original_size = f.delivered_size = bytes.size();
        ledger_->record(f);
        std::shared_ptr<const SourceHold> hold;
        try {
            hold = std::make_shared<const SourceHold>(std::move(original), ledger_);
        } catch (...) {
            ledger_->note_release(); // `original` still ends normally here
            throw;
        }
        return backend::FrameLease{f.delivered, bytes, std::move(hold)};
    }
    void request_stop() noexcept override { inner_->request_stop(); }
    void stop() noexcept override { inner_->stop(); }
    backend::StreamStatistics statistics() const override { return inner_->statistics(); }

private:
    std::unique_ptr<backend::IFrameStream> inner_;
    std::shared_ptr<const Clock> clock_;
    std::shared_ptr<Ledger> ledger_;
    std::uint64_t run_{0};      // set by start() before any receive()
    std::uint64_t sequence_{0}; // receive() runs on one thread at a time
};

class Device final : public backend::ICameraDevice {
public:
    Device(std::shared_ptr<backend::ICameraDevice> inner, std::shared_ptr<const Clock> clock, std::shared_ptr<Ledger> ledger)
        : inner_(std::move(inner)), clock_(std::move(clock)), ledger_(std::move(ledger)) {}
    backend::CameraCapabilities capabilities() const override { return inner_->capabilities(); }
    backend::Result<backend::CameraConfiguration> configuration() const override { return inner_->configuration(); }
    backend::Result<backend::CameraConfiguration> apply(const backend::CameraConfiguration& c) override {
        return inner_->apply(c);
    }
    backend::Result<std::unique_ptr<backend::IFrameStream>> create_stream() override {
        auto created = inner_->create_stream();
        if (!created) return created.error();
        return std::unique_ptr<backend::IFrameStream>(
            std::make_unique<Stream>(std::move(created).value(), clock_, ledger_));
    }
    backend::Status close() override { return inner_->close(); }

private:
    std::shared_ptr<backend::ICameraDevice> inner_;
    std::shared_ptr<const Clock> clock_;
    std::shared_ptr<Ledger> ledger_;
};

} // namespace

std::optional<std::chrono::nanoseconds> value_at(const Model& model, std::uint64_t sequence) {
    using Rep = std::chrono::nanoseconds::rep;
    constexpr Rep max = std::numeric_limits<Rep>::max();
    const Rep epoch = model.epoch.count();
    const Rep period = model.period.count();
    if (epoch < 0 || period <= 0) return std::nullopt;
    if (sequence > static_cast<std::uint64_t>(max)) return std::nullopt;
    const auto n = static_cast<Rep>(sequence);
    if (n != 0 && period > max / n) return std::nullopt; // n * period overflows
    const Rep offset = n * period;
    if (offset > max - epoch) return std::nullopt; // epoch + offset overflows
    return std::chrono::nanoseconds{epoch + offset};
}

std::optional<backend::FrameTimestamp> nominal(const Model& model, std::uint64_t sequence) {
    const auto value = value_at(model, sequence);
    if (!value) return std::nullopt;
    backend::FrameTimestamp t;
    t.value = *value;
    t.domain = backend::ClockDomain::device;
    t.event = backend::TimestampEvent::integration_start;
    t.valid = true;
    t.clock_generation = model.clock_generation;
    return t;
}

void Clock::set_model(const Model& model) { std::lock_guard lock(mutex_); model_ = model; }
Model Clock::model() const { std::lock_guard lock(mutex_); return model_; }
void Clock::set_plan(Plan plan) {
    auto shared = plan ? std::make_shared<const Plan>(std::move(plan)) : nullptr;
    std::lock_guard lock(mutex_);
    plan_ = std::move(shared);
}
std::optional<backend::FrameTimestamp> Clock::assign(std::uint64_t sequence, const backend::FrameTimestamp& original) const {
    Model model;
    std::shared_ptr<const Plan> plan;
    {
        std::lock_guard lock(mutex_);
        model = model_;
        plan = plan_;
    }
    const auto declared = nominal(model, sequence);
    if (!declared) return std::nullopt;
    return plan ? (*plan)(sequence, *declared, original) : *declared; // plan runs without the lock
}

std::size_t Ledger::frames() const { std::lock_guard lock(mutex_); return frames_.size(); }
std::uint64_t Ledger::runs() const { std::lock_guard lock(mutex_); return runs_; }
std::size_t Ledger::released() const { std::lock_guard lock(mutex_); return released_; }
std::size_t Ledger::outstanding() const { std::lock_guard lock(mutex_); return created_ - released_; }
std::size_t Ledger::peak_outstanding() const { std::lock_guard lock(mutex_); return peak_; }
std::optional<SourceFrame> Ledger::by_frame_id(std::uint64_t native_frame_id) const {
    std::lock_guard lock(mutex_);
    const auto found = by_id_.find(native_frame_id);
    if (found == by_id_.end()) return std::nullopt;
    return frames_[found->second];
}
std::vector<SourceFrame> Ledger::all() const { std::lock_guard lock(mutex_); return frames_; }
bool Ledger::wait_frames(std::size_t at_least, std::chrono::milliseconds bound) const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, bound, [&] { return frames_.size() >= at_least; });
}
bool Ledger::wait_outstanding(std::size_t at_most, std::chrono::milliseconds bound) const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, bound, [&] { return created_ - released_ <= at_most; });
}
std::uint64_t Ledger::begin_run() {
    std::lock_guard lock(mutex_);
    return ++runs_;
}
void Ledger::record(SourceFrame frame) {
    {
        std::lock_guard lock(mutex_);
        // The Fake's 16-bit frame ID can repeat only after 65535 frames;
        // the latest frame with an ID wins (tests use far fewer).
        by_id_[frame.original.native_frame_id] = frames_.size();
        frames_.push_back(std::move(frame));
        ++created_;
        if (created_ - released_ > peak_) peak_ = created_ - released_;
    }
    changed_.notify_all();
}
void Ledger::note_release() {
    { std::lock_guard lock(mutex_); ++released_; }
    changed_.notify_all();
}

Backend::Backend(std::shared_ptr<backend::ICameraBackend> inner, std::shared_ptr<const Clock> clock,
                 std::shared_ptr<Ledger> ledger)
    : inner_(std::move(inner)), clock_(std::move(clock)), ledger_(std::move(ledger)) {}
std::string_view Backend::id() const noexcept { return inner_->id(); }
backend::Result<std::vector<backend::DeviceInfo>> Backend::enumerate() { return inner_->enumerate(); }
backend::Result<std::shared_ptr<backend::ICameraDevice>> Backend::open(std::string_view device_id) {
    auto opened = inner_->open(device_id);
    if (!opened) return opened.error();
    auto inner = std::move(opened).value();
    { std::lock_guard lock(mutex_); last_ = inner; }
    return std::shared_ptr<backend::ICameraDevice>(std::make_shared<Device>(std::move(inner), clock_, ledger_));
}
std::shared_ptr<backend::ICameraDevice> Backend::last_opened_inner() const {
    std::lock_guard lock(mutex_);
    return last_.lock();
}

} // namespace aravis_virtual_clock
