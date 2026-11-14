#include "pleora_stream.hpp"
#include "pleora_features.hpp"
#include "profiles/mono_free_running.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>

namespace gige_mel::backend::pleora {
namespace {
namespace profile = profiles::mono_free_running;
using namespace std::chrono_literals;
constexpr std::size_t pool_size = 4;
constexpr std::size_t frame_limit = 64 * 1024 * 1024;
constexpr std::size_t pool_limit = 256 * 1024 * 1024;

class Stream final : public IFrameStream {
public:
    explicit Stream(std::shared_ptr<NativeSource> source) : source_(std::move(source)) {}
    ~Stream() override { stop(); }
    Status start(std::uint64_t configuration_generation) override {
        std::lock_guard lock(mutex_);
        if (running_) return Error{ErrorCode::invalid_state, "eBUS stream already started"};
        if (terminal_) return *terminal_;
        // No unbounded per-restart pools: old leases must be released first.
        for (const auto& slot : pool_)
            if (slot && slot.use_count() > 1)
                return Error{ErrorCode::resource_exhausted, "eBUS retained leases prevent restart"};
        for (auto& slot : pool_) slot.reset();
        auto run = source_->start();
        if (!run) return run.error();
        if (run.value().payload_bytes > frame_limit) {
            cleanup();
            return Error{ErrorCode::resource_exhausted, "eBUS payload exceeds copy budget"};
        }
        const auto plan = profile::plan_pool(static_cast<std::int64_t>(run.value().payload_bytes),
            run.value().configuration, pool_size, frame_limit, pool_limit, run.value().format);
        if (!plan) {
            cleanup();
            return plan.error();
        }
        try {
            for (auto& slot : pool_) slot = std::make_shared<std::vector<std::byte>>(plan.value().buffer_bytes);
        } catch (const std::bad_alloc&) {
            cleanup();
            return Error{ErrorCode::resource_exhausted, "eBUS host copy pool allocation failed"};
        }
        run_ = run.value();
        configuration_generation_ = configuration_generation;
        ++stream_generation_;
        running_ = true;
        cancelled_.store(false);
        return {};
    }
    Result<FrameLease> receive(std::chrono::milliseconds timeout) override {
        if (timeout < 0ms || timeout > 24h)
            return Error{ErrorCode::invalid_argument, "eBUS timeout outside 0..24 hours"};
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            if (cancelled_.load()) return Error{ErrorCode::stopped, "eBUS receive cancelled"};
            // Stop waits at most one 10ms native poll plus bounded validation/copy.
            // The SDK graph cannot be destroyed while a buffer is in use.
            std::unique_lock lock(mutex_);
            if (terminal_) return *terminal_;
            if (!running_ || cancelled_.load()) return Error{ErrorCode::stopped, "eBUS stream stopped"};
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            const auto slice = std::clamp(remaining, 0ms, 10ms);
            auto native = source_->next(static_cast<std::uint32_t>(slice.count()));
            if (!native) {
                if (native.error().code != ErrorCode::timeout) return native.error();
            } else {
                if (!native.value().buffer) return Error{ErrorCode::io_error, "eBUS returned a null buffer"};
                auto* buffer = native.value().buffer;
                // Always release, including allocation/diagnostic exceptions.
                struct ReturnBuffer {
                    NativeSource& source;
                    PvBuffer* buffer;
                    ~ReturnBuffer() {
                        if (buffer) { try { (void)source.release(buffer); } catch (...) {} }
                    }
                } returned{*source_, buffer};
                auto result = copy(*buffer, native.value().operation);
                returned.buffer = nullptr;
                auto released = source_->release(buffer);
                if (!released) {
                    terminal_ = released.error();
                    return *terminal_;
                }
                if (result || result.error().code != ErrorCode::timeout) return result;
            }
            lock.unlock(); // allow stop to acquire the operation boundary between polls
        } while (!cancelled_.load() && std::chrono::steady_clock::now() < deadline);
        if (cancelled_.load()) return Error{ErrorCode::stopped, "eBUS receive cancelled"};
        std::lock_guard lock(mutex_);
        ++statistics_.timeout_count;
        return Error{ErrorCode::timeout, "eBUS receive deadline expired"};
    }
    void request_stop() noexcept override { cancelled_.store(true); }
    void stop() noexcept override {
        request_stop();
        std::lock_guard lock(mutex_);
        if (running_) {
            try { cleanup(); }
            catch (...) {
                // Allocation while constructing a diagnostic must not escape
                // noexcept stop. Latch failure without another allocation.
                terminal_.emplace(ErrorCode::io_error, std::string{});
            }
        }
        running_ = false;
    }
    StreamStatistics statistics() const override {
        std::lock_guard lock(mutex_);
        return statistics_;
    }
private:
    void cleanup() {
        auto result = source_->stop();
        if (!result) terminal_ = result.error(); // uncertain cleanup cannot silently restart
    }
    Result<FrameLease> copy(PvBuffer& buffer, const PvResult& operation) {
        if (!operation.IsOK()) {
            switch (operation.GetCode()) {
            case PvResult::Code::MISSING_PACKETS:
            case PvResult::Code::IMAGE_ERROR:
            case PvResult::Code::BUFFER_TOO_SMALL:
            case PvResult::Code::TOO_MANY_RESENDS:
            case PvResult::Code::RESENDS_FAILURE:
            case PvResult::Code::TOO_MANY_CONSECUTIVE_RESENDS:
            case PvResult::Code::CORRUPTED_DATA:
                break; // incomplete data is discarded, not delivered
            default:
                terminal_ = native_error(operation, "buffer operation");
                return *terminal_;
            }
        }
        profile::NativeFrameFacts facts;
        facts.status = operation.IsOK() && buffer.IsHeaderValid() && buffer.IsTrailerValid()
            ? profile::NativeStatus::success : profile::NativeStatus::incomplete;
        facts.raw_status = static_cast<int>(operation.GetCode());
        if (facts.status == profile::NativeStatus::success) {
            facts.raw_payload = static_cast<int>(buffer.GetPayloadType());
            facts.payload = buffer.GetPayloadType() == PvPayloadTypeImage
                ? profile::NativePayload::image : profile::NativePayload::other;
            facts.has_chunks = buffer.HasChunks();
            facts.parts = 1;
            facts.part_is_2d_image = true;
            facts.capacity_bytes = buffer.GetSize();
            facts.received_bytes = buffer.GetAcquiredSize();
            facts.frame_id = buffer.GetBlockID();
            if (facts.payload == profile::NativePayload::image) {
                auto* image = buffer.GetImage();
                facts.pixel_format = static_cast<std::uint32_t>(image->GetPixelType());
                facts.width = image->GetWidth(); facts.height = image->GetHeight();
                facts.offset_x = image->GetOffsetX(); facts.offset_y = image->GetOffsetY();
                facts.padding_x = image->GetPaddingX(); facts.padding_y = image->GetPaddingY();
                facts.image_bytes = image->GetImageSize();
                const auto base = reinterpret_cast<std::uintptr_t>(buffer.GetDataPointer());
                const auto data = reinterpret_cast<std::uintptr_t>(image->GetDataPointer());
                facts.data_present = base != 0 && data >= base;
                if (facts.data_present) facts.part_offset = data - base;
                facts.storage_bits = image->GetBitsPerPixel();
                facts.significant_bits = run_.format == PixelFormat::mono12 ? 12u : 8u;
                facts.byte_order = run_.format == PixelFormat::mono12
                    ? ByteOrder::little_endian : ByteOrder::not_applicable;
            }
        }
        const auto decision = profile::validate_frame(facts, run_.configuration, run_.format);
        if (decision.verdict == profile::FrameVerdict::incomplete) {
            ++statistics_.incomplete_frames;
            return Error{ErrorCode::timeout, "eBUS incomplete frame discarded: " + decision.reason};
        }
        if (decision.verdict != profile::FrameVerdict::accept)
            return Error{decision.code, "eBUS frame rejected: " + decision.reason};
        for (auto& slot : pool_) {
            if (slot.use_count() != 1) continue;
            if (decision.span_bytes > slot->size())
                return Error{ErrorCode::io_error, "eBUS frame exceeds frozen copy capacity"};
            std::memcpy(slot->data(), buffer.GetImage()->GetDataPointer(), decision.span_bytes);
            if (run_.format == PixelFormat::mono12) {
                const auto row = image_layout(run_.format, run_.configuration.width, run_.configuration.height)->row_bytes;
                for (std::size_t y = 0; y < run_.configuration.height; ++y)
                    for (std::size_t x = 1; x < row; x += 2)
                        if ((std::to_integer<unsigned>((*slot)[y * decision.row_stride + x]) & 0xf0u) != 0)
                            return Error{ErrorCode::io_error, "eBUS Mono12 sample has nonzero unused bits"};
            }
            FrameDescriptor descriptor;
            descriptor.width = run_.configuration.width; descriptor.height = run_.configuration.height;
            descriptor.row_stride_bytes = decision.row_stride;
            descriptor.storage_bits_per_pixel = facts.storage_bits;
            descriptor.significant_bits_per_pixel = facts.significant_bits;
            descriptor.byte_order = facts.byte_order;
            descriptor.pixel_format = run_.format;
            descriptor.native_frame_id = facts.frame_id;
            descriptor.configuration = run_.configuration;
            descriptor.configuration_generation = configuration_generation_;
            descriptor.stream_generation = stream_generation_;
            // Raw device/reception clocks are deliberately NOT qualified timestamps.
            ++statistics_.delivered_frames;
            return FrameLease(descriptor, {slot->data(), decision.span_bytes}, slot);
        }
        ++statistics_.pool_starvation_events;
        return Error{ErrorCode::resource_exhausted, "eBUS host lease pool full"};
    }
    std::shared_ptr<NativeSource> source_;
    mutable std::mutex mutex_;
    std::atomic<bool> cancelled_{true};
    bool running_{false};
    std::optional<Error> terminal_;
    RunConfiguration run_;
    std::uint64_t stream_generation_{0}, configuration_generation_{0};
    std::array<std::shared_ptr<std::vector<std::byte>>, pool_size> pool_;
    StreamStatistics statistics_;
};
}

std::unique_ptr<IFrameStream> make_stream(std::shared_ptr<NativeSource> source) {
    return std::make_unique<Stream>(std::move(source));
}
} // namespace gige_mel::backend::pleora
