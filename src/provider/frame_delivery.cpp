#include "channel_core.hpp"
#include "stream_state.hpp"
#include "test_hooks.hpp"
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/stacked-image/StackedImageListener.h>
#include <cstring>
#include <limits>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

void ChannelCore::run_stacks(const std::shared_ptr<StreamState>& state,
                       const std::weak_ptr<ChannelCore>& weak) noexcept {
    const auto fail = [&](const char* message, bool callback) {
        state->failed = true;
        if (callback) ++state->runtime->callback_failures;
        else ++state->runtime->stream_failures;
        state->runtime->report(message);
    };
    try {
        const auto listener = std::dynamic_pointer_cast<ir::StackedImageListener>(state->listener);
        while (!state->stopping) {
            auto received = state->stack_stream->receive(50ms);
            if (state->stopping) break;
            if (!received) {
                if (received.error().code == backend::ErrorCode::timeout) continue;
                fail(received.error().message.c_str(), false);
                break;
            }
            auto lease = std::move(received).value();
            const auto& d = lease.descriptor();
            const auto& s = d.product;
            if (!backend::valid_stack_support(s) || s != state->runtime->stack_support ||
                d.configuration_generation != state->generation || !lease.owns_pixels() ||
                lease.pixels().size() != s.packed_bytes || !lease.pixels().data()) {
                ++state->runtime->rejected;
                fail("inconsistent stack descriptor, generation or payload; acquisition stopped", false);
                break;
            }
            const auto bytes = static_cast<std::size_t>(s.packed_bytes);
            std::shared_ptr<HostBuffer> target;
            for (const auto& buffer : state->buffers) if (buffer->acquire(bytes)) { target = buffer; break; }
            if (!target) { ++state->runtime->dropped; continue; }
            struct FillGuard {
                std::shared_ptr<HostBuffer> buffer;
                ~FillGuard() { buffer->cancel_fill(); }
            } fill{target};
            ir::StackFrameHeader header(static_cast<std::uint16_t>(s.platform_id),
                static_cast<std::uint8_t>(s.txa_fom), static_cast<std::uint16_t>(s.depth),
                mel::UCI_ID(s.capability.uuid, s.capability.label),
                mel::UCI_ID(s.activity.uuid, s.activity.label), d.stack_id,
                static_cast<std::uint32_t>(bytes), static_cast<std::uint16_t>(s.height),
                static_cast<std::uint16_t>(s.width), static_cast<std::uint8_t>(s.storage_bits),
                static_cast<std::uint8_t>(s.logical_bits), 0);
            std::memcpy(target->getImageAddress(), lease.pixels().data(), bytes);
            const auto control_pin = state->runtime->control.load().lock();
            const auto channel = weak.lock();
            if (!control_pin || !channel) break;
            GIGE_MEL_STACK_SEAM(before_admission);
            {
                std::lock_guard lock(channel->callback_mutex_);
                if (channel->is_detached() || state->stopping) break;
                ++channel->active_metadata_callbacks_;
            }
            struct Admission {
                ChannelCore& channel;
                ~Admission() {
                    { std::lock_guard lock(channel.callback_mutex_); --channel.active_metadata_callbacks_; }
                    channel.callback_cv_.notify_all();
                }
            } admission{*channel};
            GIGE_MEL_STACK_SEAM(after_reservation);
            // Admission is serialized against detach closure. Join drains
            // this delivery even when detach begins before user entry.
            target->publish();
            ++state->runtime->delivered;
            CallbackGuard guard(state->runtime.get());
            try {
                const auto result = listener->onImageStack(*channel, header, target);
                if (result != ir::Return::Success) {
                    fail(result == ir::Return::NotSupported ?
                        "stack callback returned NotSupported; delivered buffer still requires release" :
                        "stack callback returned failure; delivered buffer still requires release", true);
                    break;
                }
            } catch (...) {
                fail("stack callback threw; delivered buffer still requires release", true);
                break;
            }
        }
    } catch (const std::exception& error) {
        state->failed = true;
        ++state->runtime->stream_failures;
        try { state->runtime->report(error.what()); } catch (...) {}
    } catch (...) {
        state->failed = true;
        ++state->runtime->stream_failures;
    }
    state->stack_stream->request_stop();
}

void ChannelCore::run(const std::shared_ptr<StreamState>& state, const std::weak_ptr<ChannelCore>& weak) noexcept {
    try {
        std::optional<std::uint64_t> stream_generation;
        std::optional<std::uint64_t> clock_generation;
        std::optional<std::chrono::nanoseconds> last_timestamp;
        while (!state->stopping) {
            auto result = state->stream->receive(50ms);
            if (!result) {
                // Classify at the cancellation observation: a backend stop
                // alone does not establish provider-requested shutdown.
                if (state->stopping) break;
                if (result.error().code == backend::ErrorCode::timeout) continue;
                ++state->runtime->stream_failures;
                state->failed = true;
                state->runtime->report(result.error().code == backend::ErrorCode::stopped && result.error().message.empty()
                    ? "backend stream stopped without provider cancellation" : result.error().message);
                break;
            }
            auto frame = std::move(result).value();
            if (state->stopping) break;
            const auto& d = frame.descriptor();
            const auto& c = state->configuration;
            const auto format = state->runtime->camera_capabilities.pixel_format;
            const auto layout = backend::image_layout(format, c.width, c.height);
            const bool layout_matches = d.width == c.width && d.height == c.height && d.configuration == c &&
                d.configuration_generation == state->generation && (!stream_generation || d.stream_generation == *stream_generation) &&
                layout && d.pixel_format == format && d.storage_bits_per_pixel == layout->storage_bits &&
                d.significant_bits_per_pixel == layout->significant_bits && d.byte_order == layout->byte_order &&
                d.bands == 1 && d.row_stride_bytes >= layout->row_bytes &&
                (d.height == 1 || d.row_stride_bytes <=
                    (std::numeric_limits<std::size_t>::max() - layout->row_bytes) / (d.height - 1));
            const bool timestamp_matches = d.timestamp.valid && d.timestamp.domain == state->runtime->settings.timestamp_domain &&
                d.timestamp.event == backend::TimestampEvent::integration_start;
            if (!layout_matches || !timestamp_matches || d.status != backend::FrameStatus::complete ||
                frame.bytes().size() < (d.height - 1) * d.row_stride_bytes + layout->row_bytes) {
                ++state->runtime->rejected;
                state->runtime->report("rejected incomplete frame or mismatched layout, generation, or timestamp contract");
                continue;
            }
            stream_generation = d.stream_generation;
            if ((clock_generation && *clock_generation != d.timestamp.clock_generation) ||
                (last_timestamp && d.timestamp.value < *last_timestamp)) {
                ++state->runtime->stream_failures;
                state->failed = true;
                state->runtime->report("timestamp epoch changed or clock regressed; disable and resolve the time mapping before restart");
                break;
            }
            clock_generation = d.timestamp.clock_generation;
            last_timestamp = d.timestamp.value;
            const auto bytes = layout->image_bytes;
            std::shared_ptr<HostBuffer> target;
            for (const auto& buffer : state->buffers) if (buffer->acquire(bytes)) { target = buffer; break; }
            if (!target) { ++state->runtime->dropped; continue; }
            struct FillGuard {
                std::shared_ptr<HostBuffer> buffer;
                ~FillGuard() { buffer->cancel_fill(); }
            } fill_guard{target};
            auto* destination = static_cast<std::byte*>(target->getBufferAddress());
            for (std::size_t row = 0; row < d.height; ++row)
                std::memcpy(destination + row * layout->row_bytes,
                            frame.bytes().data() + row * d.row_stride_bytes, layout->row_bytes);

            ir::FrameHeader header;
            header.setSystemTime(d.timestamp.value);
            header.setIntegrationTime(c.exposure);
            header.setWidth(d.width);
            header.setHeight(d.height);
            header.setBitsPerPixel(layout->storage_bits);
            header.setNumBands(1);
            header.setFormat(ir::PixelFormat::Mono);
            header.setFrameID(static_cast<std::uint32_t>(d.native_frame_id & 0xffffffffULL));
            header.setSubframeID(0);
            header.setSubframeTotal(1);
            header.setImageType(ir::ImageType::Staring);
            header.setImageFlip(ir::ImageFlip::None);
            header.setFlags({ir::ImageFlag::StareSnapshot});
            header.setColumnOffset(c.offset_x);
            header.setRowOffset(c.offset_y);
            header.setHorizontalFieldOfView(state->image_field_of_view.horizontal_radians);
            header.setVerticalFieldOfView(state->image_field_of_view.vertical_radians);
            header.setFace({state->location, 0});
            const auto control_pin = state->runtime->control.load().lock();
            const auto channel = weak.lock();
            if (!control_pin || !channel || channel->is_detached() || state->stopping) break;
            target->publish();
            ++state->runtime->delivered;
            CallbackGuard guard(state->runtime.get());
            try { state->listener->onImage(*channel, header, target); }
            catch (...) {
                // The callback may have retained the buffer before throwing.
                // Never auto-release it and overwrite a live consumer lease.
                ++state->runtime->callback_failures;
                state->failed = true;
                state->runtime->report("image callback threw; acquisition stopped, delivered buffer still requires release");
                state->stopping = true;
                state->stream->request_stop();
            }
        }
    } catch (const std::exception& error) {
        ++state->runtime->stream_failures;
        state->failed = true;
        try { state->runtime->report(error.what()); } catch (...) {}
    } catch (...) {
        ++state->runtime->stream_failures;
        state->failed = true;
    }
    state->stream->stop();
}

} // namespace provider_detail
} // namespace gige_mel
