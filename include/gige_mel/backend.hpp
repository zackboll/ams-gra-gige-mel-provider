#pragma once

#include <gige_mel/navigation.hpp>
#include <gige_mel/line_of_sight.hpp>

#include <chrono>
#include <array>
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

/// @brief Backend outcomes, distinct from Common MEL ErrorCode and IR MEL Return.
/// Ordinary SDK/transport failures should be translated at the adapter boundary.
enum class ErrorCode {
    invalid_argument,   ///< Invalid request value or combination.
    invalid_state,      ///< Operation not valid in the current lifecycle state.
    not_found,          ///< Requested identity not found.
    busy,               ///< Resource already in use (for example, an exclusive open).
    unsupported,        ///< Operation or mode not implemented by this source/profile.
    resource_exhausted, ///< A required resource could not be obtained.
    timeout,            ///< Wait expired; by itself neither a stop nor connection loss.
    stopped,            ///< Acquisition ended; does not say who requested the stop.
    disconnected,       ///< Device connection unavailable, lost, or logically closed.
    io_error            ///< Native operation failed, not ordinary polling expiry.
};

/// @brief Owned backend failure and its human-readable diagnostic.
struct Error {
    ErrorCode code;
    std::string message;
};

/// @brief Holds a success value or a backend error, by value.
/// Use has_value() (or the bool conversion) before accessing an alternative.
/// Returning Result does not make an operation non-throwing: unexpected C++
/// exceptions, such as allocation or value construction failures, can escape.
template <class T> class Result {
public:
    Result(T value) : value_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : value_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return value_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }
    /// @pre has_value(). The returned reference borrows from this result.
    T& value() & { return std::get<0>(value_); }
    /// @pre has_value(). The returned reference borrows from this result.
    const T& value() const& { return std::get<0>(value_); }
    /// @pre has_value(). Move the value out before the result is destroyed.
    T&& value() && { return std::get<0>(std::move(value_)); }
    /// @pre The result holds an error. A wrong alternative access throws
    /// std::bad_variant_access (also applies to value()).
    const Error& error() const { return std::get<1>(value_); }

private:
    std::variant<T, Error> value_;
};

/// @brief Success without a payload (default constructed), or an owned error.
template <> class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}
    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }
    /// @pre !has_value(). Access on success throws std::bad_optional_access.
    /// @return A reference valid while this result retains the error.
    const Error& error() const { return error_.value(); }

private:
    std::optional<Error> error_;
};

/// Backend operation outcome without a success payload.
using Status = Result<void>;

/// @brief Complete ROI and timing request or effective device snapshot.
/// Defaults are synthetic fixture values, not a claim about a physical camera.
/// @see ICameraDevice::apply, ICameraDevice::configuration
struct CameraConfiguration {
    std::uint32_t width{64};      ///< ROI width in pixels.
    std::uint32_t height{48};     ///< ROI height in pixels.
    std::uint32_t offset_x{0};    ///< Column offset in pixels from sensor origin.
    std::uint32_t offset_y{0};    ///< Row offset in pixels from sensor origin.
    std::uint32_t frame_rate_hz{30}; ///< Integer frames per second.
    std::chrono::nanoseconds exposure{std::chrono::milliseconds{1}}; ///< Integration duration.

    bool operator==(const CameraConfiguration&) const = default;
};

/// @brief Selected single-band formats: Mono8 or unpacked Mono12 (not Mono12p).
/// Mono12 is PFNC 0x01100005: unsigned LSB-aligned 12-bit values in 16-bit
/// words with the high nibble zero. PFNC does not establish native word byte
/// order; the adapter must qualify it independently of the format ID.
/// @see @ref md_docs_2mono12-image-profile "Unpacked Mono12 profile"
enum class PixelFormat { mono8, mono12 };
/// Byte order of multibyte samples (not_applicable for Mono8).
enum class ByteOrder { not_applicable, little_endian, big_endian };

/// @brief Tightly packed destination geometry; excludes native row padding.
struct ImageLayout {
    std::size_t row_bytes;             ///< Bytes per destination row.
    std::size_t image_bytes;           ///< Bytes for the full destination image.
    std::uint32_t significant_bits;    ///< Signal bits per sample (8 or 12).
    std::uint32_t storage_bits;        ///< Stored bits per sample (8 or 16).
    ByteOrder byte_order;              ///< Selected destination order; Mono12 is low byte first.
};

/// @brief Compute the selected destination representation for dimensions in pixels.
/// @return Layout whose packed rows and image byte count fit size_t, or
/// std::nullopt for a zero dimension, unknown format, or arithmetic overflow.
/// @note Does not check MEL's narrower fields, native ROI/stride constraints,
/// sensor byte order, or timestamp provenance.
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

/// @brief Device/profile envelope and source facts, not a list of valid requests.
/// Sensor extent and geometry increments are in pixels; rate bounds are in
/// integer Hz and minimum exposure is a duration. The adapter still validates
/// native limits, step sizes, ROI fit and coupled constraints (e.g. exposure
/// versus frame period). A profile may admit only part of the envelope.
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
    bool mono8{true};                      ///< Legacy source indicator; pixel_format is authoritative.
    bool external_trigger{false};          ///< Source capability, not a MEL trigger guarantee.
    bool external_buffer_acquisition{false}; ///< Source capability; provider still copies into MEL buffers.
    /// Explicit source evidence of an empty defect map across supported
    /// configurations; false means no such evidence, not proof of defects.
    bool known_empty_defect_map{false};
    /// Authoritative selected backend/device format, not a CameraCommand setting.
    PixelFormat pixel_format{PixelFormat::mono8};
};

/// @brief Owned enumeration record. Only device_id is the backend's open key.
struct DeviceInfo {
    std::string backend_id;
    std::string device_id;
    std::string display_name; ///< Human-readable label, not necessarily unique.
};

/// Clock basis, not a clock conversion. Host monotonic, device time and UTC
/// are not interchangeable; PTP synchronization or mapping must be established separately.
enum class ClockDomain { unspecified, host_monotonic, device, utc };
/// Event represented by the timestamp. Reception is not integration start.
enum class TimestampEvent { unspecified, integration_start, reception };
/// Native payload completeness. Incomplete imagery is not published to MEL.
enum class FrameStatus { complete, incomplete };

/// @brief Timestamp and the context required to interpret it.
/// @warning A numeric time or a valid flag does not qualify MEL timing.
/// Independently establish integration-start provenance and the clock mapping
/// for the selected sensor. Synthetic timing models only a virtual exposure.
struct FrameTimestamp {
    std::chrono::nanoseconds value{};              ///< Nanoseconds on the declared clock; not raw ticks.
    ClockDomain domain{ClockDomain::unspecified};  ///< Clock basis of value.
    TimestampEvent event{TimestampEvent::unspecified}; ///< Observed or established event.
    bool valid{false};                            ///< Source vouches for the stated time and context.
    std::uint64_t clock_generation{0};            ///< Identifies a continuous clock mapping; change on discontinuity.
};

/// @brief Frozen metadata for the acquired image, not a later device readback.
/// The MEL provider requires completeness, matching geometry, format, byte
/// order, bit depths and generations, one band, and a readable span covering
/// the last row at the declared stride (checked for overflow). It copies packed
/// row contents, omitting padding; it does not byte-swap or qualify native data.
/// The timestamp must be valid integration start in the configured domain.
/// A changed clock generation or time regression after admission faults the
/// running acquisition; a repeated timestamp alone does not.
/// @see FrameLease, IFrameStream::receive, image_layout
struct FrameDescriptor {
    std::uint32_t width{0};                ///< Image columns in pixels.
    std::uint32_t height{0};               ///< Image rows in pixels.
    std::size_t row_stride_bytes{0};       ///< Byte distance between row starts; may include padding.
    std::uint32_t storage_bits_per_pixel{8}; ///< Bits occupied by one pixel.
    std::uint32_t significant_bits_per_pixel{8}; ///< Signal bits per pixel.
    std::uint32_t bands{1};
    PixelFormat pixel_format{PixelFormat::mono8};
    ByteOrder byte_order{ByteOrder::not_applicable};
    FrameStatus status{FrameStatus::complete};
    std::uint64_t native_frame_id{0};      ///< Source identifier; MEL Image uses its low 32 bits.
    std::uint64_t stream_generation{0};   ///< Backend acquisition run identity.
    std::uint64_t configuration_generation{0}; ///< Caller token from IFrameStream::start.
    FrameTimestamp timestamp{};
    CameraConfiguration configuration{}; ///< Effective ROI and timing frozen for this frame.
};

/// @brief Move-only ownership token for immutable host-readable source bytes.
/// The retained owner must back the entire span and keep the SDK resources
/// needed to read and retire it alive across stop, close, and stream/device
/// destruction. Final retirement (possibly on a different thread) must safely
/// release or requeue the storage without throwing.
/// @warning A borrowed view does not pin storage. Keep the owning lease alive;
/// do not use a view after retirement or rely on the moved-from lease. Do not
/// read a lease concurrently with moving or destroying it.
/// @note The provider copies to separate consumer-registered host storage.
/// Retiring this source lease is not MEL Buffer::release(); a delivered MEL
/// buffer remains held until explicit release, even after disable or detach.
/// This is not a zero-copy, GPU, or RDMA contract.
class FrameLease {
public:
    /// @param descriptor Metadata for these bytes, stored by value.
    /// @param bytes Read-only view of the source payload.
    /// @param owner Retention token that actually owns/pins all viewed bytes.
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

    /// Borrowed metadata for the lifetime of this lease instance.
    [[nodiscard]] const FrameDescriptor& descriptor() const noexcept {
        return descriptor_;
    }
    /// Borrowed immutable bytes; copying the span does not extend their lifetime.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

private:
    FrameDescriptor descriptor_;
    std::span<const std::byte> bytes_;
    std::shared_ptr<const void> owner_;
};

/// @brief Backend-owned observations returned by value, not MEL publications.
/// Counter coverage and reset scope are adapter-specific. Synthetic counters
/// are held by the opened device, across acquisition starts/stops.
struct StreamStatistics {
    std::uint64_t delivered_frames{0};       ///< Frames handed out by the source.
    std::uint64_t incomplete_frames{0};      ///< Incomplete frames observed by the source.
    std::uint64_t timeout_count{0};          ///< Source receive waits that timed out.
    std::uint64_t pool_starvation_events{0}; ///< Source pool starvation, not MEL destination drops.
};

/// @brief One source acquisition stream with a single receive owner.
/// Serialize setup/start with configuration and teardown. Cancellation, stop
/// and statistics may run concurrently with receive and each other. No backend
/// method calls back into the provider or consumer. Join the receive owner
/// before destroying the stream; held leases do not require the stream object.
/// Receive bounds assume normal scheduling; they are not hard real-time deadlines.
/// @see @ref md_docs_2backend-authoring "Adding a camera backend"
class IFrameStream {
public:
    virtual ~IFrameStream() = default;
    /// @brief Synchronously start a fresh acquisition on an open, stopped stream.
    /// @param configuration_generation Caller token to echo on each descriptor
    /// with the effective configuration frozen for this run. Do not replay
    /// frames from earlier runs; use a fresh backend stream generation.
    /// @return Success when ready to receive, or a backend error. Restart is
    /// not guaranteed with outstanding leases (the synthetic pool rejects it).
    virtual Status start(std::uint64_t configuration_generation) = 0;
    /// @brief Receive one owned frame lease, waiting at most the finite timeout.
    /// @pre Start succeeded. Supply a nonnegative timeout; only one receiving
    /// thread may call receive.
    /// @retval ErrorCode::timeout No frame within the wait; retryable polling.
    /// @retval ErrorCode::stopped This acquisition ended. Without provider
    /// cancellation it is an acquisition fault, not normal MEL shutdown.
    /// @note Cancellation wakes the wait promptly. An already committed receive
    /// may still return a lease; the provider gates publication by cancellation
    /// and configuration generation, and checks stream generation against the
    /// first admitted frame. Do not rely on that check to identify an initial
    /// stale frame with the same configuration token. Report connection loss separately.
    virtual Result<FrameLease> receive(std::chrono::milliseconds timeout) = 0;
    /// Signal cancellation and wake a pending receive. Safe concurrently with
    /// receive; does not join its caller-owned thread or wait for lease retirement.
    virtual void request_stop() noexcept = 0;
    /// Stop acquisition and wake receive, without invalidating outstanding
    /// leases. Safe concurrently with receive; support repeated stop (the MEL
    /// worker and teardown path may both stop). Caller must still join receive.
    /// The void return does not confirm native stop success; adapters may latch
    /// an uncertain native outcome and refuse restart until it is resolved.
    virtual void stop() noexcept = 0;
    /// Return an owned source snapshot; callable concurrently with receive.
    virtual StreamStatistics statistics() const = 0;
};

/// @brief Optional input interface implemented alongside ICameraDevice.
/// Without a sink, the provider validates and acknowledges MEL receipt only.
/// A sink does not itself establish GVCP serialization, a register protocol,
/// transport delivery, sensor processing, clock or transfer alignment, or
/// navigation-to-image association.
/// @see @ref md_docs_2navigation-backend-forwarding "Navigation forwarding and MEL error mapping"
class INavigationSink {
public:
    virtual ~INavigationSink() = default;
    /// @brief Synchronously accept a validated report, independently of image acquisition.
    /// @param report Owned values. Move or copy any data retained after return.
    /// @return Success means backend acceptance only. On failure the provider
    /// neither commits the timestamp nor emits an acknowledgement callback.
    /// `unsupported` maps to MEL Unsupported, not receipt-only success. Use
    /// `timeout` with a diagnostic for uncertain acceptance (MEL InvalidState).
    /// Disconnection/I/O failures map to InsufficientRemoteResources; invalid
    /// requests map to InvalidParameters. See the forwarding guide for the full
    /// mapping. Ordinary failures should return Error; exceptions may leave
    /// acceptance uncertain. There is no automatic retry or rollback.
    /// @warning Runs on the submission thread under the provider operation lock,
    /// serialized with lifecycle/configuration but may overlap frame receive.
    /// Return within a documented finite bound, including failure paths. Do not
    /// reenter the provider, invoke consumers, or wait for provider-dependent work.
    virtual Status submit_navigation(NavigationReport report) = 0;
};

/// @brief Owned LOS subscription with one receiver and bounded source work.
/// No provider/consumer calls or waits for provider operations. The provider
/// cancels on Image disable/detach and retires its worker before destruction.
class ILineOfSightStream {
public:
    virtual ~ILineOfSightStream() = default;
    /// Fixed, nonzero clock generation for this subscription, as echoed in observations.
    virtual std::uint64_t clock_generation() const noexcept = 0;
    /// @brief Wait for an owned observation or error, bounded by timeout.
    /// Keep at most one pending latest observation (coalesce older pending
    /// values); do not replay observations predating open. Timeout is polling;
    /// terminal errors retire LOS delivery without faulting Image acquisition.
    virtual Result<LosObservation> receive(std::chrono::milliseconds timeout) = 0;
    /// Concurrent, idempotent, nonblocking cancellation that wakes receive
    /// promptly. Join the receive owner before destroying the stream.
    virtual void request_stop() noexcept = 0;
};

/// @brief Optional sensor LOS source implemented alongside ICameraDevice.
/// Bounded methods; no provider/consumer calls or waits for provider operations.
/// @see @ref md_docs_2image-los-metadata "Source-backed Image LOS metadata"
class ILineOfSightSource {
public:
    virtual ~ILineOfSightSource() = default;
    /// Return immutable device-lifetime support facts by value. Only a valid
    /// declaration at init permits advertising the corresponding Image metadata.
    virtual LosSupport line_of_sight_support() const = 0;
    /// @brief Open a fresh owned subscription, with no replay and a fixed clock generation.
    /// @param subscription_id Provider token to echo on every observation.
    /// @note Synchronous under the operation lock at Image enable, not discovery.
    virtual Result<std::unique_ptr<ILineOfSightStream>> open_line_of_sight(
        std::uint64_t subscription_id) = 0;
};

/// Volume order: contiguous slices, then consecutive rows within each slice,
/// with no row/slice padding and one byte per Mono8 sample. Unknown is rejected.
enum class StackLayout { unknown, contiguous_frame_major_row_major_mono8 };
/// Declared product capability/activity identity, not invented by the provider.
struct StackIdentity {
    std::array<std::uint8_t, 16> uuid{};
    std::string label;
    bool operator==(const StackIdentity&) const = default;
};
/// @brief Device-lifetime declaration for a pre-formed stack product.
/// Width/height are pixels per slice, depth is slice count, and storage/logical
/// bits are per sample (both 8). Packed bytes are width * height * depth.
/// Source ID/generation identify the producer and its product generation;
/// formation_meaning, identity_scheme and fom_meaning describe the source's
/// product formation, stack keys and transfer-alignment FOM interpretation.
/// mask_information_absent is absence of information, not a defect-free claim.
/// @see valid_stack_support, StackDescriptor
struct StackSupport {
    bool supported{false};
    std::uint32_t width{}, height{}, depth{}, storage_bits{}, logical_bits{};
    std::uint64_t packed_bytes{}, source_generation{};
    StackLayout layout{StackLayout::unknown};
    std::string source_id, formation_meaning, identity_scheme, fom_meaning;
    StackIdentity capability, activity;
    std::uint32_t platform_id{}, txa_fom{};
    bool mask_information_absent{false};
    bool operator==(const StackSupport&) const = default;
};
/// @brief Test whether a declaration can be advertised by this stack profile.
/// Requires support, the selected Mono8 layout, representable dimensions
/// (depth at least two), checked packed size fitting size_t and uint32_t,
/// nonzero source generation, identity/provenance fields, a uint16_t platform
/// ID, FOM 1–10 and absence of mask information. Not physical qualification.
inline bool valid_stack_support(const StackSupport& s) noexcept {
    if (!s.supported || s.layout != StackLayout::contiguous_frame_major_row_major_mono8 ||
        s.width == 0 || s.width > UINT16_MAX || s.height == 0 || s.height > UINT16_MAX ||
        s.depth < 2 || s.depth > UINT16_MAX || s.storage_bits != 8 || s.logical_bits != 8 ||
        s.platform_id > UINT16_MAX || s.txa_fom < 1 || s.txa_fom > 10 ||
        !s.mask_information_absent || s.source_generation == 0 || s.source_id.empty() ||
        s.formation_meaning.empty() || s.identity_scheme.empty() || s.fom_meaning.empty() ||
        s.capability.label.empty() || s.activity.label.empty() ||
        s.capability.uuid == std::array<std::uint8_t, 16>{} ||
        s.activity.uuid == std::array<std::uint8_t, 16>{}) return false;
    std::size_t bytes = s.width;
    if (s.height > std::numeric_limits<std::size_t>::max() / bytes) return false;
    bytes *= s.height;
    if (s.depth > std::numeric_limits<std::size_t>::max() / bytes) return false;
    bytes *= s.depth;
    return bytes <= UINT32_MAX && bytes == s.packed_bytes;
}
/// @brief Identity, layout and configuration context for one source product.
/// The declaration must match the cached device support exactly.
struct StackDescriptor {
    StackSupport product;
    std::uint32_t stack_id{}; ///< Source record key under identity_scheme, not a provider frame counter.
    std::uint64_t configuration_generation{}; ///< Caller token supplied at source open.
};
/// @brief Move-only lease over immutable, host-readable packed stack pixels.
/// The owner must pin the entire span through copy and subsequent retirement,
/// including cancellation. Do not use views after retirement or use a moved-from
/// lease. Source lease retirement does not release a consumer's MEL destination.
class StackLease {
public:
    /// Retain the owner of the entire pixel span and store the descriptor by value.
    StackLease(StackDescriptor descriptor, std::span<const std::byte> pixels,
               std::shared_ptr<const void> owner)
        : descriptor_(std::move(descriptor)), pixels_(pixels), owner_(std::move(owner)) {}
    StackLease(StackLease&&) = default;
    StackLease& operator=(StackLease&&) = default;
    StackLease(const StackLease&) = delete;
    StackLease& operator=(const StackLease&) = delete;
    /// Borrowed descriptor, valid for the lifetime of this lease instance.
    const StackDescriptor& descriptor() const noexcept { return descriptor_; }
    /// Borrowed immutable pixels; copying the span does not extend storage lifetime.
    std::span<const std::byte> pixels() const noexcept { return pixels_; }
    /// Tests token presence only, not actual pointer ownership or span bounds.
    bool owns_pixels() const noexcept { return static_cast<bool>(owner_); }
private:
    StackDescriptor descriptor_;
    std::span<const std::byte> pixels_;
    std::shared_ptr<const void> owner_; // immutable bytes survive through copy
};
/// @brief An open stack product stream with a single receive owner.
/// Keep source work bounded; do not invoke provider/consumer code. Join the
/// receiving thread before destroying the stream.
class IStackedImageStream {
public:
    virtual ~IStackedImageStream() = default;
    /// @brief Receive a complete product lease or a backend error within timeout.
    /// Timeout is polling. Other errors without cancellation fault acquisition.
    /// The provider checks the declaration, generation, owner and exact span size
    /// before copying; malformed products fail the stream rather than publish.
    virtual Result<StackLease> receive(std::chrono::milliseconds timeout) = 0;
    /// Concurrent, idempotent, nonblocking cancellation; wake receive promptly.
    virtual void request_stop() noexcept = 0;
};
/// @brief Optional source of pre-formed products, not generic frame concatenation.
/// The SDK-free fixture exercises delivery; it does not establish physical
/// detector TSI/stack capability. Image and Stack share exclusive acquisition.
/// @see @ref md_docs_2stacked-image-provider "Stacked-image provider"
class IStackedImageSource {
public:
    virtual ~IStackedImageSource() = default;
    /// Return immutable device-lifetime facts by value. Only a valid declaration
    /// from this interface at init permits Stack advertisement; enable rechecks it.
    virtual StackSupport stacked_image_support() const = 0;
    /// @brief Synchronously open an owned stream ready to receive, without pre-open replay.
    /// @param configuration_generation Token to echo in each StackDescriptor.
    /// @note Called at enable after listener, buffer and acquisition checks;
    /// bound the work and retire partial resources on failure. Do not call back
    /// into the provider/consumer or wait for provider-dependent work.
    virtual Result<std::unique_ptr<IStackedImageStream>> open_stacked_images(
        std::uint64_t configuration_generation) = 0;
};

/// @brief Open camera connection and the factory for its source streams.
/// Lifecycle methods are synchronous. Serialize control/configuration and
/// stream setup/close; do not configure during acquisition or assume arbitrary
/// concurrent calls are supported. The common provider serializes these calls.
/// @see IFrameStream, ICameraBackend::open
class ICameraDevice {
public:
    virtual ~ICameraDevice() = default;
    /// Return a value snapshot of selected profile bounds and source support.
    virtual CameraCapabilities capabilities() const = 0;
    /// Return actual effective ROI and timing by value, not the last request.
    /// Fail if readback is unavailable (e.g. disconnected); never invent a snapshot.
    virtual Result<CameraConfiguration> configuration() const = 0;
    /// @brief Apply a complete configuration, not a patch or best-effort success.
    /// @pre Device open and acquisition stopped. Validate native constraints
    /// and discrete settings before native writes wherever possible.
    /// @return On success, actual readback. On failure, report the reason and
    /// expose any partial writes through configuration(); failure is not rollback.
    /// The provider rereads even after a returned error and requires matching
    /// readback for MEL success. Unreadable or inadmissible readback blocks
    /// enable until valid configuration is established. Synthetic validation
    /// failures leave state unchanged.
    virtual Result<CameraConfiguration> apply(const CameraConfiguration&) = 0;
    /// Create a uniquely owned, stopped stream on an open device. Call start
    /// before receive. A backend may enforce one live stream (busy on a duplicate).
    virtual Result<std::unique_ptr<IFrameStream>> create_stream() = 0;
    /// @brief Logically close the device, waking receive and blocking future I/O.
    /// This does not join the caller's receive owner or retire held frame leases.
    /// Physical SDK resources may outlive close under lease ownership. Explicit
    /// close is not a reconnect; report connection loss as disconnected rather
    /// than repeated timeouts. Synthetic close can be repeated successfully.
    virtual Status close() = 0;
};

/// @brief Enumerates and explicitly opens cameras through an installed C++ source contract.
/// The common provider owns MEL lifecycle and publication; a camera profile
/// selects policy and the backend performs native SDK/transport operations.
/// This is not a stable cross-toolchain binary plugin ABI. Serialize control
/// calls unless the adapter specifies stronger concurrency support.
class ICameraBackend {
public:
    virtual ~ICameraBackend() = default;
    /// Borrowed stable adapter ID, valid while the backend lives; no device I/O.
    virtual std::string_view id() const noexcept = 0;
    /// Synchronously discover owned identity records without opening a capture
    /// session. A discovery result is not a reservation or guarantee of availability.
    virtual Result<std::vector<DeviceInfo>> enumerate() = 0;
    /// @brief Synchronously open the exact backend-specific device ID.
    /// @param device_id Stable key, not a display label or MEL instance name.
    /// Explicit open need not be preceded by enumeration. Do not fall back to
    /// another device or retain the caller's borrowed view; use not_found or
    /// busy where applicable.
    /// @return Shared ownership of an open camera on success, or a backend error.
    virtual Result<std::shared_ptr<ICameraDevice>> open(std::string_view device_id) = 0;
};

/// SDK-free fixture options; invalid settings are reported on device open.
struct SyntheticOptions {
    std::size_t frame_pool_size{4}; ///< Native source pool size, [1, 64].
    CameraConfiguration initial_configuration{};
    PixelFormat pixel_format{PixelFormat::mono8};
};

/// Construct a synthetic backend without device I/O. Allocation may throw;
/// open validates the supplied options. Not a physical capability claim.
std::shared_ptr<ICameraBackend> make_synthetic_backend(SyntheticOptions options = {});

} // namespace gige_mel::backend
