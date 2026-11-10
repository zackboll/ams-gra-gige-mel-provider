#pragma once

#include <gige_mel/navigation.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace gige_mel::backend {

// Internal C++ contract, deliberately independent of MEL and camera SDK types.
// This is a source interface, not a stable binary plugin ABI.
enum class ErrorCode {
    invalid_argument,
    invalid_state,
    not_found,
    busy,
    unsupported,
    resource_exhausted,
    timeout,
    stopped,
    disconnected,
    io_error
};

struct Error {
    ErrorCode code;
    std::string message;
};

template <class T> class Result {
public:
    Result(T value) : value_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : value_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return value_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }
    T& value() & { return std::get<0>(value_); }
    const T& value() const& { return std::get<0>(value_); }
    T&& value() && { return std::get<0>(std::move(value_)); }
    const Error& error() const { return std::get<1>(value_); }

private:
    std::variant<T, Error> value_;
};

template <> class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}
    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }
    const Error& error() const { return error_.value(); }

private:
    std::optional<Error> error_;
};

using Status = Result<void>;

struct CameraConfiguration {
    std::uint32_t width{64};
    std::uint32_t height{48};
    std::uint32_t offset_x{0};
    std::uint32_t offset_y{0};
    std::uint32_t frame_rate_hz{30};
    std::chrono::nanoseconds exposure{std::chrono::milliseconds{1}};

    bool operator==(const CameraConfiguration&) const = default;
};

// Mono12 profile: PFNC 0x01100005, unsigned LSB-aligned words, high nibble
// zero. This project's selected layout is low byte first; PFNC alone does
// not define word byte order. Adapters must establish it independently.
enum class PixelFormat { mono8, mono12 };
enum class ByteOrder { not_applicable, little_endian, big_endian };

struct ImageLayout {
    std::size_t row_bytes;
    std::size_t image_bytes;
    std::uint32_t significant_bits;
    std::uint32_t storage_bits;
    ByteOrder byte_order;
};

// Checked before allocation, narrowing or offset arithmetic. A successful
// result proves every packed-row offset fits size_t.
inline std::optional<ImageLayout> image_layout(PixelFormat format, std::uint32_t width,
                                               std::uint32_t height) noexcept {
    if (width == 0 || height == 0 || (format != PixelFormat::mono8 && format != PixelFormat::mono12))
        return std::nullopt;
    const std::size_t sample_bytes = format == PixelFormat::mono12 ? 2 : 1;
    if (width > std::numeric_limits<std::size_t>::max() / sample_bytes) return std::nullopt;
    const auto row = static_cast<std::size_t>(width) * sample_bytes;
    if (height > std::numeric_limits<std::size_t>::max() / row) return std::nullopt;
    return ImageLayout{row, row * height, format == PixelFormat::mono12 ? 12u : 8u,
                       format == PixelFormat::mono12 ? 16u : 8u,
                       format == PixelFormat::mono12 ? ByteOrder::little_endian : ByteOrder::not_applicable};
}

struct CameraCapabilities {
    std::uint32_t sensor_width{640};
    std::uint32_t sensor_height{480};
    std::uint32_t width_increment{1};
    std::uint32_t height_increment{1};
    std::uint32_t offset_x_increment{1};
    std::uint32_t offset_y_increment{1};
    std::uint32_t minimum_frame_rate_hz{1};
    std::uint32_t maximum_frame_rate_hz{120};
    std::chrono::nanoseconds minimum_exposure{1};
    bool mono8{true};
    bool external_trigger{false};
    bool external_buffer_acquisition{false};
    // Explicit source evidence, valid across all supported configurations.
    // Unknown/unsupported by default; not inferred from identity or frames.
    bool known_empty_defect_map{false};
    // Selected backend/device profile, not a CameraCommand setting. `mono8`
    // above is retained for existing source consumers; this is authoritative.
    PixelFormat pixel_format{PixelFormat::mono8};
};

struct DeviceInfo {
    std::string backend_id;
    std::string device_id;
    std::string display_name;
};

enum class ClockDomain { unspecified, host_monotonic, device, utc };
enum class TimestampEvent { unspecified, integration_start, reception };
enum class FrameStatus { complete, incomplete };

struct FrameTimestamp {
    std::chrono::nanoseconds value{};
    ClockDomain domain{ClockDomain::unspecified};
    TimestampEvent event{TimestampEvent::unspecified};
    bool valid{false};
    std::uint64_t clock_generation{0};
};

struct FrameDescriptor {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::size_t row_stride_bytes{0};
    std::uint32_t storage_bits_per_pixel{8};
    std::uint32_t significant_bits_per_pixel{8};
    std::uint32_t bands{1};
    PixelFormat pixel_format{PixelFormat::mono8};
    ByteOrder byte_order{ByteOrder::not_applicable};
    FrameStatus status{FrameStatus::complete};
    std::uint64_t native_frame_id{0};
    std::uint64_t stream_generation{0};
    std::uint64_t configuration_generation{0};
    FrameTimestamp timestamp{};
    CameraConfiguration configuration{};
};

// Owning, move-only lease. An adapter must ensure owner keeps the complete SDK
// resource graph alive and releases/requeues storage without throwing. The byte
// view stays immutable and valid until this lease is destroyed or moved from,
// including after stream stop and device close. Only host-readable data is
// represented by this initial contract; GPU/RDMA need an explicit extension.
class FrameLease {
public:
    FrameLease(FrameDescriptor descriptor, std::span<const std::byte> bytes,
               std::shared_ptr<const void> owner) noexcept
        : descriptor_(descriptor), bytes_(bytes), owner_(std::move(owner)) {}
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;
    FrameLease(FrameLease&& other) noexcept
        : descriptor_(other.descriptor_), bytes_(std::exchange(other.bytes_, {})),
          owner_(std::move(other.owner_)) {}
    FrameLease& operator=(FrameLease&& other) noexcept {
        if (this != &other) {
            owner_ = std::move(other.owner_);
            descriptor_ = other.descriptor_;
            bytes_ = std::exchange(other.bytes_, {});
        }
        return *this;
    }

    [[nodiscard]] const FrameDescriptor& descriptor() const noexcept {
        return descriptor_;
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

private:
    FrameDescriptor descriptor_;
    std::span<const std::byte> bytes_;
    std::shared_ptr<const void> owner_;
};

struct StreamStatistics {
    std::uint64_t delivered_frames{0};
    std::uint64_t incomplete_frames{0};
    std::uint64_t timeout_count{0};
    std::uint64_t pool_starvation_events{0};
};

class IFrameStream {
public:
    virtual ~IFrameStream() = default;
    virtual Status start(std::uint64_t configuration_generation) = 0;
    // One receiving thread per stream. request_stop(), stop(), and statistics()
    // may be called concurrently. A receive already committed before stop may
    // return its lease; the provider must gate publication by its generation.
    // The caller must join its receiving thread before destroying the stream.
    // Receive is bounded by timeout and promptly interrupted by cancellation.
    // ErrorCode::timeout is transient; ErrorCode::stopped is terminal for this
    // acquisition. The provider's cancellation state when handling the result
    // determines normal shutdown versus an unexpected acquisition fault: a
    // backend stopped result alone does not prove the MEL consumer requested
    // shutdown. Outstanding leases remain valid across stop and logical close.
    virtual Result<FrameLease> receive(std::chrono::milliseconds timeout) = 0;
    virtual void request_stop() noexcept = 0;
    virtual void stop() noexcept = 0;
    virtual StreamStatistics statistics() const = 0;
};

// Optional interface implemented alongside ICameraDevice. Absence preserves
// receipt-only behavior; an implemented sink's unsupported result is a failure,
// not permission to silently fall back to receipt. Calls are synchronous and
// serialized with device lifecycle/configuration, but may overlap frame receive.
// A sink must not call provider/consumer code, must bound its work, and must own
// any data it retains after return. Success means backend acceptance only, not
// processing completion. Ordinary failures return Error; no retry is automatic.
// A timeout means acceptance is uncertain, not definitive rejection. Other
// uncertain outcomes must use timeout with diagnostic text, not claim rejection.
// Implementations must return within a documented finite bound on every call,
// including error paths; never wait for work requiring provider operations.
class INavigationSink {
public:
    virtual ~INavigationSink() = default;
    virtual Status submit_navigation(NavigationReport report) = 0;
};

class ICameraDevice {
public:
    virtual ~ICameraDevice() = default;
    virtual CameraCapabilities capabilities() const = 0;
    virtual Result<CameraConfiguration> configuration() const = 0;
    // Applies a complete configuration while stopped and returns actual
    // readback. Hardware adapters must report partial changes truthfully; the
    // provider decides recovery. Synthetic validation is atomic.
    virtual Result<CameraConfiguration> apply(const CameraConfiguration&) = 0;
    virtual Result<std::unique_ptr<IFrameStream>> create_stream() = 0;
    // Logical close wakes receive, blocks future I/O, and preserves old leases.
    virtual Status close() = 0;
};

class ICameraBackend {
public:
    virtual ~ICameraBackend() = default;
    virtual std::string_view id() const noexcept = 0;
    virtual Result<std::vector<DeviceInfo>> enumerate() = 0;
    virtual Result<std::shared_ptr<ICameraDevice>> open(std::string_view device_id) = 0;
};

struct SyntheticOptions {
    std::size_t frame_pool_size{4};
    CameraConfiguration initial_configuration{};
    PixelFormat pixel_format{PixelFormat::mono8};
};

// Invalid options are reported by open(), avoiding constructor-side I/O.
// Allocation failure may throw while constructing result/error strings or
// creating the backend; adapters translate all ordinary SDK failures to Error.
std::shared_ptr<ICameraBackend> make_synthetic_backend(SyntheticOptions options = {});

} // namespace gige_mel::backend
