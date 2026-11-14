#pragma once

// TEST ONLY. Exercises the canonical copy stream with real SDK-allocated memory,
// not a camera, Ethernet transport, licensing substitute or production factory.
#include "backends/pleora/pleora_stream.hpp"
#include <cstring>
#include <thread>

namespace pleora_controlled {
namespace b = gige_mel::backend;
namespace p = b::pleora;
using namespace std::chrono_literals;
inline constexpr b::CameraConfiguration configuration{16, 8, 0, 0, 10, 1ms};
class Source final : public p::NativeSource {
public:
    explicit Source(b::PixelFormat format) : format_(format) {}
    b::Result<p::RunConfiguration> start() override {
        const auto result = buffer_.GetImage()->Alloc(16, 8,
            format_ == b::PixelFormat::mono8 ? PvPixelMono8 : PvPixelMono12);
        if (!result.IsOK()) return p::native_error(result, "test allocation");
        std::memset(buffer_.GetImage()->GetDataPointer(), 0, buffer_.GetImage()->GetImageSize());
        return p::RunConfiguration{configuration, format_, b::image_layout(format_, 16, 8)->image_bytes};
    }
    b::Result<p::NativeBuffer> next(std::uint32_t) override {
        std::this_thread::sleep_for(2ms); // bounded deterministic source, no networking
        ++sequence_;
        (void)buffer_.SetBlockID(sequence_);
        (void)buffer_.SetTimestamp(987654321 + sequence_); // observed raw clock, NOT MEL time
        auto* data = buffer_.GetImage()->GetDataPointer();
        for (unsigned i = 0; i < 128; ++i) {
            const auto value = (i + sequence_) & (format_ == b::PixelFormat::mono8 ? 255u : 4095u);
            if (format_ == b::PixelFormat::mono8) data[i] = static_cast<std::uint8_t>(value);
            else { data[2*i] = static_cast<std::uint8_t>(value & 255u); data[2*i+1] = static_cast<std::uint8_t>(value >> 8); }
        }
        return p::NativeBuffer{&buffer_, PvResult(PvResult::Code::OK)};
    }
    b::Status release(PvBuffer*) override { return {}; }
    b::Status stop() override { return {}; }
private:
    b::PixelFormat format_;
    PvBuffer buffer_;
    std::uint64_t sequence_{0};
};
class Device final : public b::ICameraDevice {
public:
    explicit Device(b::PixelFormat format) : format_(format) {}
    b::CameraCapabilities capabilities() const override {
        b::CameraCapabilities c;
        c.sensor_width = 16; c.sensor_height = 8; c.pixel_format = format_;
        c.minimum_frame_rate_hz = 10; c.maximum_frame_rate_hz = 10;
        c.minimum_exposure = 1ms;
        return c;
    }
    b::Result<b::CameraConfiguration> configuration() const override { return pleora_controlled::configuration; }
    b::Result<b::CameraConfiguration> apply(const b::CameraConfiguration& c) override {
        if (c != pleora_controlled::configuration) return b::Error{b::ErrorCode::invalid_argument, "fixed test configuration"};
        return c;
    }
    b::Result<std::unique_ptr<b::IFrameStream>> create_stream() override {
        return p::make_stream(std::make_shared<Source>(format_));
    }
    b::Status close() override { return {}; }
private:
    b::PixelFormat format_;
};
class Backend final : public b::ICameraBackend {
public:
    explicit Backend(b::PixelFormat format) : format_(format) {}
    std::string_view id() const noexcept override { return "pleora-controlled-test"; }
    b::Result<std::vector<b::DeviceInfo>> enumerate() override { return std::vector<b::DeviceInfo>{}; }
    b::Result<std::shared_ptr<b::ICameraDevice>> open(std::string_view) override {
        return std::shared_ptr<b::ICameraDevice>(std::make_shared<Device>(format_));
    }
private:
    b::PixelFormat format_;
};
}