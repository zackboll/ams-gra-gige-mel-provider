#include <gige_mel/provider.hpp>
#include "provider_internal.hpp"
#include "config_parser.hpp"

#include <irmel/library/c2/C2Channel.h>
#include <irmel/library/image/ImageChannel.h>
#include <irmel/library/health-status/HealthStatusChannel.h>
#include <irmel/library/irmel-types/FrameHeader.h>
#include <irmel/library/irmel-types/ImageListener.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <numbers>
#include <thread>
#include <time.h>
#include <type_traits>
#include <utility>

// The provider software version is the CMake project version, supplied as
// private definitions. It is distinct from the MEL API version and from the
// pinned upstream dependency revisions.
#if !defined(GIGE_MEL_VERSION_MAJOR) || !defined(GIGE_MEL_VERSION_MINOR) || !defined(GIGE_MEL_VERSION_PATCH)
#error "GIGE_MEL_VERSION_MAJOR/MINOR/PATCH must be defined from the CMake project version"
#endif
#define GIGE_MEL_STRINGIFY_IMPL(value) #value
#define GIGE_MEL_STRINGIFY(value) GIGE_MEL_STRINGIFY_IMPL(value)
#define GIGE_MEL_VERSION_TEXT GIGE_MEL_STRINGIFY(GIGE_MEL_VERSION_MAJOR) "." \
    GIGE_MEL_STRINGIFY(GIGE_MEL_VERSION_MINOR) "." GIGE_MEL_STRINGIFY(GIGE_MEL_VERSION_PATCH)

namespace gige_mel {
namespace {
namespace ir = ams::iface::irmel;
namespace mel = ams::iface::mel;
using namespace std::chrono_literals;

// Private software identity shared by MFA_Status About, VersionInfo text and the
// SubsystemStatusResp CSCI version.
struct ProviderSoftware {
    static constexpr const char* name = "ams-gra-gige-mel-provider";
    static constexpr const char* version_text = GIGE_MEL_VERSION_TEXT;
    static constexpr std::uint32_t major = GIGE_MEL_VERSION_MAJOR;
    static constexpr std::uint32_t minor = GIGE_MEL_VERSION_MINOR;
    static constexpr std::uint32_t patch = GIGE_MEL_VERSION_PATCH;
};

// Both Health counters use explicit modulo-2^32 wrap; no signed overflow or
// saturation. Compile-time boundary evidence, not a runtime setter.
constexpr std::uint32_t next_counter(std::uint32_t value) noexcept {
    return static_cast<std::uint32_t>(value + 1U);
}
static_assert(next_counter(0) == 1 && next_counter(std::numeric_limits<std::uint32_t>::max()) == 0);

template <class T> mel::RequestFor<T> ready(T value) {
    std::promise<mel::ErrorOr<std::shared_ptr<T>>> promise;
    auto result = promise.get_future();
    promise.set_value(mel::ErrorOr<std::shared_ptr<T>>(std::make_shared<T>(std::move(value))));
    return result;
}

template <class T> mel::RequestFor<T> failed(mel::ErrorCode code, std::string message) {
    std::promise<mel::ErrorOr<std::shared_ptr<T>>> promise;
    auto result = promise.get_future();
    promise.set_value(mel::ErrorOr<std::shared_ptr<T>>(mel::Error(code, std::move(message))));
    return result;
}

mel::ErrorCode map_error(backend::ErrorCode code) {
    using B = backend::ErrorCode;
    switch (code) {
    case B::invalid_argument: return mel::ErrorCode::InvalidParameters;
    case B::not_found: return mel::ErrorCode::InvalidId;
    case B::unsupported: return mel::ErrorCode::Unsupported;
    case B::resource_exhausted: return mel::ErrorCode::InsufficientLocalResources;
    case B::disconnected: case B::io_error: return mel::ErrorCode::InsufficientRemoteResources;
    case B::timeout: case B::busy: case B::stopped: case B::invalid_state:
        return mel::ErrorCode::InvalidState;
    }
    return mel::ErrorCode::InvalidState;
}

ir::CannotComply command_reason(backend::ErrorCode code) {
    using B = backend::ErrorCode;
    switch (code) {
    case B::invalid_argument: return ir::CannotComply::InvalidInputParameter;
    case B::unsupported: return ir::CannotComply::CapabilityUnavailable;
    case B::not_found: return ir::CannotComply::UnknownID;
    case B::resource_exhausted: return ir::CannotComply::InsufficientResources;
    case B::disconnected: return ir::CannotComply::SubsystemUnavailable;
    case B::io_error: return ir::CannotComply::SubsystemFault;
    case B::timeout: case B::busy: case B::stopped: case B::invalid_state:
        return ir::CannotComply::StateOrSettings;
    }
    return ir::CannotComply::Other;
}

struct ConfigurationOutcome {
    bool success{false};
    // invalid_argument/unsupported from apply are ordinary negative responses;
    // schema errors and transport/readback failures are error-only futures.
    std::optional<backend::Error> error;
    ir::CannotComply reason{ir::CannotComply::Other};
    std::string description;

    static ConfigurationOutcome failure(const backend::Error& failure, bool error_only = true) {
        return {false, error_only ? std::optional{failure} : std::nullopt,
                command_reason(failure.code), failure.message};
    }
    ir::CommandStatus status(std::uint32_t id) const {
        ir::CommandStatus result;
        result.setCommandID(id);
        result.setState(success ? ir::CommandState::Accepted : ir::CommandState::Rejected);
        result.setReasonID(success ? ir::CannotComply::NotSet : reason);
        result.setReasonDescription(success ? "" : description);
        return result;
    }
};

// Exact pinned ScanParam/ScanType defaults, not a scan geometry interpretation.
// Equality to finite zero also rejects NaN and either infinity (including center).
bool unset_scan(const ir::ScanParam& scan) {
    const ir::ScanParam defaults;
    const auto center = scan.getCenter();
    const auto& type = scan.getScanType();
    return scan.getIsDefinedWithRangeAndAltitude() == defaults.getIsDefinedWithRangeAndAltitude() &&
        center.az == defaults.getCenter().az && center.el == defaults.getCenter().el &&
        scan.getCentFrameRefEl() == defaults.getCentFrameRefEl() &&
        scan.getCentFrameRefAz() == defaults.getCentFrameRefAz() &&
        scan.getScanWidth() == defaults.getScanWidth() && scan.getScanHeight() == defaults.getScanHeight() &&
        type.getContinuousScan() == defaults.getScanType().getContinuousScan() &&
        type.getReturning() == defaults.getScanType().getReturning() &&
        type.getAgileScan() == defaults.getScanType().getAgileScan() &&
        scan.getScanId() == defaults.getScanId() && scan.getScanRate() == defaults.getScanRate() &&
        scan.getPreferredRevisitInterval() == defaults.getPreferredRevisitInterval() &&
        scan.getRequiredRevisitInterval() == defaults.getRequiredRevisitInterval() &&
        scan.getMaxRangeOfInterest() == defaults.getMaxRangeOfInterest() &&
        scan.getMinRangeOfInterest() == defaults.getMinRangeOfInterest() &&
        scan.getElevationScanCenterAltitude() == defaults.getElevationScanCenterAltitude() &&
        scan.getElevationScanCenterRange() == defaults.getElevationScanCenterRange() &&
        scan.getDegredationType() == defaults.getDegredationType();
}

class ChannelCore;
class ControlImpl;
class C2ChannelImpl;
struct StreamState;

struct Runtime {
    ProviderSettings settings;
    std::shared_ptr<backend::ICameraBackend> backend;
    std::string backend_id; // immutable, acquired before any publisher exists
    std::shared_ptr<backend::ICameraDevice> device;
    std::shared_ptr<const int> affinity{std::make_shared<const int>(0)};
    // Mutations serialize here. A worker never takes this lock. Callbacks run
    // outside it; their reentrant mutations are rejected before acquiring it.
    std::mutex operations;
    mutable std::mutex state_mutex;
    backend::CameraConfiguration configuration;
    backend::CameraCapabilities camera_capabilities;
    bool initialized{false};
    bool configuration_valid{false};
    bool image_active{false};
    std::weak_ptr<StreamState> acquisition; // guarded by state_mutex
    std::uint64_t configuration_generation{1};
    std::atomic<std::weak_ptr<ControlImpl>> control;
    std::weak_ptr<ChannelCore> image_channel; // guarded by operations
    std::atomic<std::uint64_t> delivered{0}, dropped{0}, rejected{0}, callback_failures{0}, stream_failures{0};
    std::string last_error;
    // Manager lifetime; bounded latest completion and two category observations.
    std::shared_ptr<const mel::CompletedBIT> completed_bit;
    std::array<std::optional<std::chrono::nanoseconds>, 2> fault_observations;
    // Manager lifetime, guarded by state_mutex. bit_revision is committed with
    // completed_bit/fault_observations; subsystem_sequence is consumed only by a
    // successfully prepared SubsystemStatusResp. Zero is the initial state.
    std::uint32_t bit_revision{0};
    std::uint32_t subsystem_sequence{0};

    ~Runtime() {
        if (device) {
            try { (void)device->close(); } catch (...) { /* destructors must not throw */ }
        }
    }

    void report(std::string message) {
        std::lock_guard lock(state_mutex);
        last_error = std::move(message);
    }
};

// Stack frames live on the callback's stack: admission cannot be stranded by
// allocation failure, and nested callbacks retain every runtime in the chain.
struct CallbackGuard;
thread_local const CallbackGuard* callback_context = nullptr;
struct CallbackGuard {
    const Runtime* const runtime;
    const CallbackGuard* const previous;
    explicit CallbackGuard(const Runtime* value) noexcept
        : runtime(value), previous(callback_context) { callback_context = this; }
    ~CallbackGuard() { callback_context = previous; }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

bool in_provider_callback() noexcept { return callback_context != nullptr; }

bool in_runtime_callback(const Runtime* runtime) noexcept {
    for (auto frame = callback_context; frame; frame = frame->previous) {
        if (frame->runtime == runtime) return true;
    }
    return false;
}

bool valid_layout(const backend::CameraConfiguration& c, const backend::CameraCapabilities& caps) {
    const auto layout = backend::image_layout(caps.pixel_format, c.width, c.height);
    return layout && c.width <= caps.sensor_width && c.height <= caps.sensor_height &&
        c.offset_x <= caps.sensor_width - c.width && c.offset_y <= caps.sensor_height - c.height &&
        layout->image_bytes <= std::numeric_limits<std::uint32_t>::max();
}

ir::ChannelCapability capability(const Runtime& runtime, ir::ChannelType type,
                                 const mel::UCI_ID& id, const ir::Config* config = nullptr) {
    std::lock_guard lock(runtime.state_mutex);
    const auto& c = runtime.configuration;
    ir::ChannelCapability cap;
    cap.setChanID(id);
    if (type != ir::ChannelType::HealthAndStatus) {
        cap.setWidth(c.width);
        cap.setHeight(c.height);
        const auto layout = backend::image_layout(runtime.camera_capabilities.pixel_format, c.width, c.height);
        if (!layout || layout->image_bytes > std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("MEL image layout is not representable");
        cap.setBitDepth(layout->significant_bits);
        cap.setRowPitch(static_cast<std::uint32_t>(layout->row_bytes));
        cap.setBufferSize(static_cast<std::uint32_t>(layout->image_bytes));
        cap.setImageSize(static_cast<std::uint32_t>(layout->image_bytes));
        cap.setNumberOfBands(1);
        cap.setFormat(ir::PixelFormat::Mono);
    }
    cap.setChannelTypes({type});
    std::set<ir::ChannelMetadataCapabilityType> metadata{ir::ChannelMetadataCapabilityType::ChannelCommsTestRep};
    if (type == ir::ChannelType::IRSTImage) {
        metadata.insert(ir::ChannelMetadataCapabilityType::CameraCommandResp);
        metadata.insert(ir::ChannelMetadataCapabilityType::NavigationReportResp);
        if (runtime.camera_capabilities.known_empty_defect_map)
            metadata.insert(ir::ChannelMetadataCapabilityType::BadPixelList);
    }
    if (type == ir::ChannelType::CommandAndControl) {
        metadata.insert(ir::ChannelMetadataCapabilityType::CommandStatus);
        metadata.insert(ir::ChannelMetadataCapabilityType::BITConfiguration);
        metadata.insert(ir::ChannelMetadataCapabilityType::BITStatus);
    }
    if (type == ir::ChannelType::HealthAndStatus) {
        metadata.insert(ir::ChannelMetadataCapabilityType::MFAStatus);
        metadata.insert(ir::ChannelMetadataCapabilityType::MFAStatusDetailed);
        metadata.insert(ir::ChannelMetadataCapabilityType::BITStatus);
        metadata.insert(ir::ChannelMetadataCapabilityType::SubsystemStatusResp);
    }
    cap.setChannelMetadataCapabilities(metadata);
    if (config) {
        cap.setPlatform(config->getPlatform());
        cap.setSensorLocation(config->getSensorLocation());
    }
    // Navigation receipt only, not image association, NUC, distortion or tracking.
    // TaskScheduleDepth's default zero is not a conformance claim: see the profile.
    return cap;
}

class HostBuffer final : public ir::Buffer {
public:
    explicit HostBuffer(std::shared_ptr<const int> affinity) : affinity_(std::move(affinity)) {}

    ir::Return init(void* address, std::size_t size, std::int64_t context) override {
        std::lock_guard lock(mutex_);
        if (owner_ || state_ != State::available) return ir::Return::Fail;
        if (!address || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            return ir::Return::BadPointer;
        const auto begin = reinterpret_cast<std::uintptr_t>(address);
        if (size > std::numeric_limits<std::uintptr_t>::max() - begin) return ir::Return::BadPointer;
        address_ = address;
        size_ = size;
        context_ = context;
        return ir::Return::Success;
    }
    void* getBufferAddress() const override { std::lock_guard lock(mutex_); return address_; }
    void* getImageAddress() const override { return getBufferAddress(); }
    std::int64_t getSize() const override { std::lock_guard lock(mutex_); return static_cast<std::int64_t>(size_); }
    std::int64_t getContext() const override { std::lock_guard lock(mutex_); return context_; }

    ir::Return release() override {
        std::lock_guard lock(mutex_);
        if (state_ != State::delivered) return ir::Return::Fail;
        state_ = State::available;
        return ir::Return::Success;
    }
    ir::Return getFlags(std::vector<ir::BufferFlag>& flags) const override {
        std::lock_guard lock(mutex_);
        flags = flags_;
        return ir::Return::Success;
    }
    void addFlag(ir::BufferFlag flag) override {
        std::lock_guard lock(mutex_);
        if (!owner_ && state_ == State::available && std::find(flags_.begin(), flags_.end(), flag) == flags_.end())
            flags_.push_back(flag);
    }
    void setFlags(const std::vector<ir::BufferFlag>& flags) override {
        std::lock_guard lock(mutex_);
        if (!owner_ && state_ == State::available) flags_ = flags;
    }

    ir::Return bind(const void* owner, const std::shared_ptr<const int>& affinity, std::size_t minimum_size) {
        std::lock_guard lock(mutex_);
        if (affinity != affinity_) return ir::Return::BadPointer;
        if (!address_ || size_ < minimum_size) return ir::Return::BadPointer;
        if (owner_ || state_ != State::available) return ir::Return::Fail;
        if (flags_.size() != 1 || flags_[0] != ir::BufferFlag::Host) return ir::Return::NotSupported;
        owner_ = owner;
        return ir::Return::Success;
    }
    void unbind() { std::lock_guard lock(mutex_); owner_ = nullptr; }
    bool outstanding() const { std::lock_guard lock(mutex_); return state_ != State::available; }
    bool acquire(std::size_t minimum_size) {
        std::lock_guard lock(mutex_);
        if (!owner_ || state_ != State::available || size_ < minimum_size) return false;
        state_ = State::filling;
        return true;
    }
    void publish() { std::lock_guard lock(mutex_); state_ = State::delivered; }
    void cancel_fill() { std::lock_guard lock(mutex_); if (state_ == State::filling) state_ = State::available; }

private:
    enum class State { available, filling, delivered };
    std::shared_ptr<const int> affinity_;
    mutable std::mutex mutex_;
    void* address_{nullptr};
    std::size_t size_{0};
    std::int64_t context_{0};
    const void* owner_{nullptr};
    State state_{State::available};
    std::vector<ir::BufferFlag> flags_{ir::BufferFlag::Host};
};

struct StreamState {
    std::shared_ptr<Runtime> runtime;
    std::unique_ptr<backend::IFrameStream> stream;
    std::shared_ptr<ir::ImageListener> listener;
    std::vector<std::shared_ptr<HostBuffer>> buffers;
    backend::CameraConfiguration configuration;
    std::uint64_t generation{};
    mel::ComponentLocation location;
    std::atomic<bool> stopping{false};
    std::atomic<bool> failed{false};
};

// Fixed identities in the manager-local experimental profile, not a vendor
// registry. Numeric BIT_Command ID 1 maps explicitly to software_bit_id().
mel::UCI_ID software_bit_id() {
    return {{0x9c,0x85,0xc2,0x63,0x54,0x6e,0x4e,0x11,0x9b,0x71,0xd2,0x40,0x5a,0x13,0x80,0x01},
            "gige_mel.provider_state.v1"};
}
constexpr const char* bit_items[] = {"gige_mel.configuration_known_valid",
    "gige_mel.owned_acquisition_without_terminal_failure"};
constexpr const char* fault_codes[] = {"gige_mel.configuration_invalid", "gige_mel.acquisition_failed"};
constexpr const char* fault_descriptions[] = {"Initialized provider configuration is not known valid",
    "Logically owned Image acquisition has an observed terminal failure; explicitly disable for cleanup"};

mel::BIT_Configuration bit_catalogue() {
    mel::BIT_Type test;
    test.setBitID(software_bit_id());
    test.setAcceptedInterface(mel::BIT_ControlInterface::SubsystemBITCommand);
    test.setBitItemNames({bit_items[0], bit_items[1]});
    // Empty components and upstream default duration 0 mean unspecified.
    return mel::BIT_Configuration({test});
}

std::chrono::nanoseconds host_utc_now() {
    timespec value{};
    if (::clock_gettime(CLOCK_REALTIME, &value) != 0)
        throw std::runtime_error("BIT host UTC clock_gettime(CLOCK_REALTIME) failed");
    constexpr auto billion = 1000000000LL;
    constexpr auto maximum = std::numeric_limits<std::chrono::nanoseconds::rep>::max();
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= billion ||
        value.tv_sec > maximum / billion ||
        (value.tv_sec == maximum / billion && value.tv_nsec > maximum % billion))
        throw std::runtime_error("BIT host Unix-epoch nanoseconds are not representable");
    const auto count = static_cast<std::chrono::nanoseconds::rep>(value.tv_sec) * billion + value.tv_nsec;
    if (count == 0) throw std::runtime_error("BIT host UTC clock returned an unspecified zero timestamp");
    return std::chrono::nanoseconds(count);
}

struct HealthSnapshot {
    bool initialized, configuration_valid, active, failed;
    std::uint64_t generation;
    ProviderStatistics statistics;
    std::optional<mel::BIT_Status> bit;
    std::exception_ptr bit_error;
    // Committed BIT-state revision paired with `bit` in this same capture.
    std::uint32_t bit_revision;
    std::optional<ir::SubsystemStatusResp> subsystem;
    std::exception_ptr subsystem_error;
};

// Provider-defined software subsystem profile; see docs/subsystem-status.md.
// Identifier 1 is scoped to this manager's provider software subsystem, not a
// globally allocated physical identifier. Zero criticality/source mean
// "unassigned in this profile", not a safety or mission-criticality rating.
constexpr std::uint32_t provider_subsystem_id = 1;
constexpr std::uint32_t unassigned_criticality = 0;
constexpr std::uint32_t unassigned_version_source = 0;

// Pure mapping from one captured core state; no I/O, no runtime reads. The
// caller assigns statusSeqNum only after this payload has been formed.
ir::SubsystemStatusResp subsystem_report(const HealthSnapshot& s) {
    // Major: this profile currently cannot provide its image acquisition
    // function. It is not a hardware diagnosis; Critical is never substituted.
    const bool unable = !s.configuration_valid || (s.active && s.failed);
    const auto failure = !s.initialized ? ir::Failure::NA : unable ? ir::Failure::Major : ir::Failure::Available;
    const auto mode = !s.initialized || unable ? ir::CSCIMode::Unknown :
        s.active ? ir::CSCIMode::Imaging : ir::CSCIMode::Idle;
    ir::SubsystemCSCIInfo csci;
    csci.setCsci(ProviderSoftware::name);
    csci.setMode(mode);
    // Pinned order: Version(source, majorRevision, minorRevision, engineeringRevision).
    csci.setVersion(ir::Version(unassigned_version_source, ProviderSoftware::major,
                                ProviderSoftware::minor, ProviderSoftware::patch));
    csci.setCriticality(unassigned_criticality);
    csci.setFailureLevel(failure);
    csci.setBIT_report(s.bit_revision);
    // In-process software association only: not camera reachability, SDK
    // transport, heartbeat, packet delivery or a future-acquisition promise.
    csci.setConnectionEstablished(s.initialized);
    ir::SubsystemStatusResp report;
    report.setSubsystemId(provider_subsystem_id);
    report.setCriticality(unassigned_criticality);
    report.setFailureLevel(failure);
    report.setSubsystems({});
    report.setSubsystemCount(0);
    report.setCSCI({csci});
    report.setCsciCount(1);
    return report;
}

// No operations mutex or backend I/O. The temporary acquisition owner is
// destroyed while state_mutex is held, under the established unpublication rule.
// One transaction prepares BIT state (and optionally the subsystem report) from
// the same core capture; candidate state is committed only after formation.
HealthSnapshot health_snapshot(Runtime& r, bool execute = false, bool prepare_bit = true,
                               bool prepare_subsystem = false) {
    std::lock_guard lock(r.state_mutex);
    const auto acquisition = r.acquisition.lock();
    HealthSnapshot s{r.initialized, r.configuration_valid, r.image_active,
        acquisition && acquisition->failed.load(), r.configuration_generation,
        {r.delivered.load(), r.dropped.load(), r.rejected.load(),
         r.callback_failures.load(), r.stream_failures.load(), r.last_error}, {}, {},
        r.bit_revision, {}, {}};
    if (!prepare_bit) return s;
    try {
        auto observations = r.fault_observations;
        const bool predicates[] = {s.initialized && !s.configuration_valid,
                                  s.initialized && s.active && s.failed};
        std::vector<mel::Fault> faults;
        for (std::size_t i = 0; i < observations.size(); ++i) {
            if (!predicates[i]) { observations[i].reset(); continue; }
            if (!observations[i]) observations[i] = host_utc_now();
            auto uuid = software_bit_id().getUUID();
            uuid.back() = static_cast<std::uint8_t>(0x11 + i);
            faults.emplace_back(mel::UCI_ID(uuid, fault_codes[i]), mel::FaultSeverity::Warning,
                mel::FaultState::Set, std::vector<mel::FaultData>{}, *observations[i],
                fault_codes[i], fault_descriptions[i], std::vector<mel::UCI_ID>{},
                std::vector<mel::FaultAmbiguityGroup>{});
        }
        auto completion = r.completed_bit;
        if (execute) {
            const bool passes[] = {s.initialized && s.configuration_valid, !(s.active && s.failed)};
            std::vector<mel::CompletedBIT_Item> items;
            for (std::size_t i = 0; i < 2; ++i)
                items.emplace_back(bit_items[i], passes[i] ? mel::BIT_Result::Pass : mel::BIT_Result::Fail,
                    passes[i] ? "" : fault_descriptions[i]);
            const bool pass = passes[0] && passes[1];
            // Completion means item evaluation has finished, not command admission.
            completion = std::make_shared<const mel::CompletedBIT>(software_bit_id(), host_utc_now(),
                pass ? mel::BIT_Result::Pass : mel::BIT_Result::Fail,
                pass ? "" : "Provider observed-state check failed; see item failure reasons", std::move(items));
        }
        s.bit.emplace(std::vector<mel::ActiveBIT>{}, completion ? std::vector<mel::CompletedBIT>{*completion} :
                      std::vector<mel::CompletedBIT>{}, std::move(faults));
        // One revision per committed semantic change: a newly completed
        // execution (even with an identical result/time) and/or an observed
        // fault creation, retirement or recurrence. Unchanged samples keep it.
        const bool changed = execute || observations != r.fault_observations;
        const auto revision = changed ? next_counter(r.bit_revision) : r.bit_revision;
        // Commit only after the complete immutable payload has been formed.
        r.completed_bit = std::move(completion);
        r.fault_observations = observations;
        r.bit_revision = revision;
        s.bit_revision = revision;
    } catch (...) { s.bit_error = std::current_exception(); }
    // A subsystem report requires the paired canonical BIT commit. Its own
    // failure never rolls back that commit and consumes no sequence number.
    if (prepare_subsystem && s.bit) {
        try {
            auto report = subsystem_report(s);
            const auto sequence = next_counter(r.subsystem_sequence);
            report.setStatusSeqNum(sequence);
            s.subsystem.emplace(std::move(report));
            r.subsystem_sequence = sequence;
        } catch (...) { s.subsystem.reset(); s.subsystem_error = std::current_exception(); }
    }
    return s;
}

class ChannelCore : public virtual ir::Channel, public std::enable_shared_from_this<ChannelCore> {
public:
    ChannelCore(std::shared_ptr<Runtime> runtime, ir::Config config)
        : runtime_(std::move(runtime)), config_(std::move(config)) {}
    ~ChannelCore() override {
        // Public detach/disable normally joins first. If a consumer destroys its
        // control graph from a callback, state captured by the thread still owns
        // every object it needs; never join the current thread.
        // Derived auxiliary cleanup belongs to disable/detach and the derived
        // destructor, never a virtual hook reached from this base destructor.
        stop_locked();
        for (auto& buffer : buffers_) buffer->unbind();
    }

    ir::ChannelType type() const { return config_.getChannelType(); }
    bool is_detached() const { return detached_.load(); }

    mel::RequestFor<ir::Return> sendKeepAliveRep() override {
        if (is_detached()) return failed<ir::Return>(mel::ErrorCode::InvalidState, "channel is detached");
        return ready(ir::Return::Success);
    }
    mel::RequestFor<ir::ChannelCommsTestRep> send(ir::ChannelCommsTestReq request) override {
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin || is_detached()) return failed<ir::ChannelCommsTestRep>(mel::ErrorCode::InvalidState, "channel is detached");
        const auto self = shared_from_this();
        ir::ChannelCommsTestRep reply(request.getCommandID(), request.getRequestID());
        auto result = ready(reply);
        std::function<void(ir::Channel&, const ir::ChannelCommsTestRep*)> callback;
        { std::lock_guard lock(callback_mutex_); callback = comms_callback_; }
        invoke(callback, reply);
        return result;
    }

    ir::Return registerBuffer(std::shared_ptr<ir::Buffer> buffer) override {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        if (type() == ir::ChannelType::HealthAndStatus) return ir::Return::NotSupported;
        if (enabled_) return ir::Return::Fail;
        if (type() != ir::ChannelType::IRSTImage) return ir::Return::NotSupported;
        auto host = std::dynamic_pointer_cast<HostBuffer>(buffer);
        if (!host) return ir::Return::BadPointer;
        if (buffers_.size() >= runtime_->settings.maximum_registered_buffers) return ir::Return::Fail;
        const auto minimum_size = static_cast<std::size_t>(getCapabilities().getBufferSize());
        const auto status = host->bind(this, runtime_->affinity, minimum_size);
        if (status != ir::Return::Success) return status;
        const auto start = reinterpret_cast<std::uintptr_t>(host->getBufferAddress());
        const auto end = start + static_cast<std::size_t>(host->getSize());
        for (const auto& registered : buffers_) {
            const auto other = reinterpret_cast<std::uintptr_t>(registered->getBufferAddress());
            const auto other_end = other + static_cast<std::size_t>(registered->getSize());
            if (start < other_end && other < end) { host->unbind(); return ir::Return::Fail; }
        }
        try { buffers_.push_back(host); }
        catch (...) { host->unbind(); throw; }
        return ir::Return::Success;
    }
    ir::Return unregisterBuffer(std::shared_ptr<ir::Buffer> buffer) override {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        if (type() == ir::ChannelType::HealthAndStatus) return ir::Return::NotSupported;
        if (enabled_) return ir::Return::Fail;
        const auto found = std::find(buffers_.begin(), buffers_.end(), buffer);
        if (found == buffers_.end()) return ir::Return::BadPointer;
        if ((*found)->outstanding()) return ir::Return::Fail;
        (*found)->unbind();
        buffers_.erase(found);
        return ir::Return::Success;
    }
    ir::Return enable() override {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        if (enabled_) return stream_state_ && stream_state_->failed ? ir::Return::Fail : ir::Return::Success;
        if (type() != ir::ChannelType::IRSTImage) { enabled_ = true; return ir::Return::Success; }
        if (buffers_.empty() || !config_.getImgLstnr()) return ir::Return::Fail;

        auto state = std::make_shared<StreamState>();
        state->runtime = runtime_;
        state->buffers = buffers_;
        state->listener = config_.getImgLstnr();
        state->location = config_.getSensorLocation();
        {
            std::lock_guard lock(runtime_->state_mutex);
            if (!runtime_->configuration_valid || runtime_->image_active) return ir::Return::Fail;
            state->configuration = runtime_->configuration;
            state->generation = runtime_->configuration_generation;
        }
        const auto layout = backend::image_layout(runtime_->camera_capabilities.pixel_format,
                                                  state->configuration.width, state->configuration.height);
        if (!layout || layout->image_bytes > std::numeric_limits<std::uint32_t>::max()) return ir::Return::Fail;
        const auto bytes = layout->image_bytes;
        for (const auto& buffer : buffers_) {
            if (buffer->outstanding() || static_cast<std::size_t>(buffer->getSize()) < bytes) return ir::Return::Fail;
        }
        try {
            auto created = runtime_->device->create_stream();
            if (!created) { runtime_->report(created.error().message); return ir::Return::Fail; }
            state->stream = std::move(created).value();
            auto started = state->stream->start(state->generation);
            if (!started) { runtime_->report(started.error().message); return ir::Return::Fail; }
            {
                std::lock_guard lock(runtime_->state_mutex);
                runtime_->image_active = true;
                runtime_->acquisition = state;
            }
            stream_state_ = state;
            enabled_ = true;
            worker_ = std::thread([state, weak = weak_from_this()] { run(state, weak); });
            return ir::Return::Success;
        } catch (const std::exception& error) {
            state->stopping = true;
            if (state->stream) state->stream->stop();
            {
                // Unpublish while the local state still owns the acquisition,
                // including thread-construction failure after publication.
                std::lock_guard lock(runtime_->state_mutex);
                runtime_->image_active = false;
                runtime_->acquisition.reset();
            }
            stream_state_.reset();
            enabled_ = false;
            runtime_->report(error.what());
            return ir::Return::Fail;
        }
    }
    ir::Return disable() override {
        if (in_provider_callback()) return ir::Return::Fail;
        // Pin before taking operations: a callback dropping the last external
        // Control must not run its destructor while this thread joins it.
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        stop_auxiliary_locked();
        stop_locked();
        return ir::Return::Success;
    }
    ir::ChannelCapability getCapabilities() const override {
        return capability(*runtime_, type(), config_.getChanID(), &config_);
    }
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::ChannelCommsTestRep* const)> callback) override {
        return register_callback(comms_callback_, std::move(callback));
    }

    void detach_locked() {
        detached_ = true;
        stop_auxiliary_locked();
        stop_locked();
        // Only defensive destruction involving this runtime may skip its own
        // drain. External detach must still wait for every admitted callback.
        if (!in_runtime_callback(runtime_.get())) {
            std::unique_lock lock(callback_mutex_);
            callback_cv_.wait(lock, [this] { return active_metadata_callbacks_ == 0; });
        }
        for (auto& buffer : buffers_) buffer->unbind();
        buffers_.clear();
    }

protected:
    template <class T> using Callback = std::function<void(ir::Channel&, const T*)>;

    template <class T> ir::Return register_callback(Callback<T>& slot, Callback<T> callback) {
        if (in_provider_callback() || is_detached()) return ir::Return::Fail;
        if (!callback) return ir::Return::BadPointer;
        std::lock_guard lock(callback_mutex_);
        if (is_detached()) return ir::Return::Fail;
        if (slot) return ir::Return::Fail;
        slot = std::move(callback);
        return ir::Return::Success;
    }
    // Used only when the caller holds operations and will unconditionally invoke
    // after a no-throw commit. Detach must drain even a not-yet-entered callback.
    template <class T> void admit_callback(const Callback<T>& callback) {
        if (callback) { std::lock_guard lock(callback_mutex_); ++active_metadata_callbacks_; }
    }
    template <class T> void invoke(const Callback<T>& callback, const T& message,
                                  const std::atomic<bool>* admission = nullptr, bool admitted = false) {
        if (!callback) return;
        if (!admitted) {
            std::lock_guard lock(callback_mutex_);
            if (is_detached() || (admission && !admission->load())) return;
            ++active_metadata_callbacks_;
        }
        CallbackGuard guard(runtime_.get());
        try { callback(*this, &message); }
        catch (...) {
            ++runtime_->callback_failures;
            try { runtime_->report(admission ? "Health metadata callback threw; periodic reporting continues" :
                "metadata callback threw; successful operation remains committed"); } catch (...) {}
        }
        { std::lock_guard lock(callback_mutex_); --active_metadata_callbacks_; }
        callback_cv_.notify_all();
    }
    template <class T> mel::RequestFor<T> unsupported(const char* name) {
        return failed<T>(is_detached() ? mel::ErrorCode::InvalidState : mel::ErrorCode::Unsupported,
                         std::string(name) + ": not available in the experimental image-stream profile");
    }

    // Callback-free shared transaction. Caller holds operations, has admitted
    // this submission and pins Control/channel through result and dispatch.
    ConfigurationOutcome configure_locked(const backend::CameraConfiguration& requested, bool profile_valid = true) {
        {
            std::lock_guard lock(runtime_->state_mutex);
            if (runtime_->image_active)
                return ConfigurationOutcome::failure({backend::ErrorCode::invalid_state, "disable image acquisition before configuring"});
        }
        if (!profile_valid || !valid_layout(requested, runtime_->camera_capabilities)) {
            const std::string message = "camera configuration cannot be satisfied: requires a complete, valid Mono8 staring configuration";
            runtime_->report(message);
            return ConfigurationOutcome::failure({backend::ErrorCode::invalid_argument, message}, false);
        }
        auto applied = runtime_->device->apply(requested);
        // Even an apply failure may have partially mutated the device.
        auto actual = runtime_->device->configuration();
        bool valid;
        {
            std::lock_guard lock(runtime_->state_mutex);
            ++runtime_->configuration_generation;
            valid = actual && valid_layout(actual.value(), runtime_->camera_capabilities);
            runtime_->configuration_valid = valid;
            if (valid) runtime_->configuration = actual.value();
        }
        ConfigurationOutcome outcome;
        if (!actual) outcome = ConfigurationOutcome::failure(actual.error());
        else if (!applied) {
            const auto code = applied.error().code;
            outcome = ConfigurationOutcome::failure(applied.error(),
                code != backend::ErrorCode::invalid_argument && code != backend::ErrorCode::unsupported);
        } else if (!valid || actual.value() != requested) {
            outcome.description = "camera readback differs from requested configuration or violates the supported layout";
        } else outcome.success = true;
        if (!outcome.success) runtime_->report(outcome.description);
        return outcome;
    }

    mel::RequestFor<ir::Return> config_set(const ir::ConfigSetCommand& command) {
        // Must precede parsing, operation locks and callback admission, including
        // cross-provider callbacks: a rejection must not recursively send status.
        if (in_provider_callback())
            return failed<ir::Return>(mel::ErrorCode::InvalidState, "schedule configuration outside callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::Return>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        ConfigurationOutcome outcome;
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<ir::Return>(mel::ErrorCode::InvalidState, "channel is detached");
            const auto parsed = parse_camera_config(command.getConfig());
            outcome = parsed ? configure_locked(parsed.value()) : ConfigurationOutcome::failure(parsed.error());
        }
        auto result = outcome.error ? failed<ir::Return>(map_error(outcome.error->code), outcome.description)
                                    : ready(outcome.success ? ir::Return::Success : ir::Return::Fail);
        on_command_status(outcome.status(command.getCommandID()));
        return result;
    }

    mel::RequestFor<ir::CameraCommandResp> camera_command(const ir::CameraCommand& command) {
        if (in_provider_callback())
            return failed<ir::CameraCommandResp>(mel::ErrorCode::InvalidState, "schedule camera changes outside callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::CameraCommandResp>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        std::shared_ptr<ChannelCore> image_recipient;
        ConfigurationOutcome outcome;
        ir::CameraCommandResp reply;
        reply.setCommandID(command.getCommandID());
        // size is left at zero: this C++ object has no specified serialized wire
        // encoding. sizeof(CameraCommandResp) would not be a portable wire size.
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<ir::CameraCommandResp>(mel::ErrorCode::InvalidState, "channel is detached");
            image_recipient = runtime_->image_channel.lock();
            backend::CameraConfiguration requested;
            requested.width = command.getWidth();
            requested.height = command.getHeight();
            requested.offset_x = command.getColumnOffset();
            requested.offset_y = command.getRowOffset();
            requested.frame_rate_hz = command.getFrameRate();
            requested.exposure = command.getIntegrationTime();
            outcome = configure_locked(requested,
                command.getImageType() == ir::ImageType::Staring && command.getNumberOfStages() == 0);
        }
        if (outcome.error) {
            auto result = failed<ir::CameraCommandResp>(map_error(outcome.error->code), outcome.description);
            on_command_status(outcome.status(command.getCommandID()));
            return result;
        }
        reply.setCommandSuccess(outcome.success);
        reply.setTimestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()));
        auto result = ready(reply);
        // Camera responses are an Image callback even when C2 submitted the
        // command. No operation/state locks are held across user code.
        if (image_recipient) image_recipient->on_camera_reply(reply);
        on_command_status(outcome.status(command.getCommandID()));
        return result;
    }

    virtual void on_camera_reply(const ir::CameraCommandResp&) {}
    virtual void on_command_status(const ir::CommandStatus&) {}
    // Called by disable AND internal detach, including Control destruction.
    virtual void stop_auxiliary_locked() {}
    std::shared_ptr<Runtime> runtime_;
    ir::Config config_;
    std::mutex callback_mutex_;

private:
    // C2 Standby is the only command allowed to stop the attached Image.
    // The caller pins Control, C2 and Image, rejects callback reentry, validates
    // the request, and holds Runtime::operations (never state/callback mutexes).
    friend class C2ChannelImpl;
    void stop_locked() {
        const bool owned_stream = static_cast<bool>(stream_state_);
        if (stream_state_) {
            stream_state_->stopping = true;
            stream_state_->stream->request_stop();
        }
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) worker_.detach();
            else worker_.join();
        }
        if (stream_state_) stream_state_->stream->stop();
        if (owned_stream) {
            // Retain the strong stream owner until no snapshot can promote the
            // weak publication. Earlier snapshots release their temporary owner
            // before this lock is acquired. No backend destruction under lock.
            std::lock_guard lock(runtime_->state_mutex);
            runtime_->image_active = false;
            runtime_->acquisition.reset();
        }
        stream_state_.reset();
        enabled_ = false;
    }

    static void run(const std::shared_ptr<StreamState>& state, const std::weak_ptr<ChannelCore>& weak) noexcept {
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
                header.setHorizontalFieldOfView(d.width * state->runtime->settings.horizontal_radians_per_pixel);
                header.setVerticalFieldOfView(d.height * state->runtime->settings.vertical_radians_per_pixel);
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

    std::atomic<bool> detached_{false};
    bool enabled_{false}; // guarded by Runtime::operations
    Callback<ir::ChannelCommsTestRep> comms_callback_;
    std::condition_variable callback_cv_;
    std::size_t active_metadata_callbacks_{0};
    std::vector<std::shared_ptr<HostBuffer>> buffers_;
    std::shared_ptr<StreamState> stream_state_;
    std::thread worker_;
};

// Every upstream pure virtual is implemented. Unsupported surfaces return the
// real MEL errors, never fabricated success or an unfulfilled promise.
#define GIGE_UNSUPPORTED_SEND(Request, Response) \
    mel::RequestFor<Response> send(Request) override { return unsupported<Response>(#Request); }
#define GIGE_UNSUPPORTED_CALLBACK(Message) \
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const Message* const)>) override { \
        return ir::Return::NotSupported; \
    }

// Structural checks only: consumer labels and values are not independently
// qualified navigation. No clock conversion, clamping or normalization.
std::optional<std::string> navigation_error(const mel::NavigationReport& report) {
    if (report.getSystemTime() <= 0ns) return "systemTime must be positive nanoseconds";
    switch (report.getState()) {
    case mel::PositionSolutionState::Aligning:
    case mel::PositionSolutionState::FreeInertial:
    case mel::PositionSolutionState::GPS:
    case mel::PositionSolutionState::Blended: break;
    default: return "state must be Aligning, FreeInertial, GPS or Blended";
    }
    if (!std::isfinite(report.getLatitude()) || report.getLatitude() < -std::numbers::pi / 2 ||
        report.getLatitude() > std::numbers::pi / 2) return "latitude must be finite and within [-pi/2, pi/2] radians";
    if (!std::isfinite(report.getLongitude()) || report.getLongitude() < -std::numbers::pi ||
        report.getLongitude() > std::numbers::pi) return "longitude must be finite and within [-pi, pi] radians";
    const auto rate = report.getAttitudeRate();
    if (rate.getAttitudeRateTime() < 0ns) return "attitudeRate.attitudeRateTime must be nonnegative nanoseconds";
    const auto covariance = report.getPositionVelocityCovarianceUncertainty();
    struct Field { const char* name; double value; bool variance = false; };
    const Field fields[]{
        {"altitude", report.getAltitude()}, {"altitudeMSL", report.getAltitudeMSL()},
        {"attitude.roll", report.getAttitude().getRoll()},
        {"attitude.pitch", report.getAttitude().getPitch()},
        {"attitude.yaw", report.getAttitude().getYaw()},
        {"attitudeRate.roll", rate.getAttitudeRate().getRoll()},
        {"attitudeRate.pitch", rate.getAttitudeRate().getPitch()},
        {"attitudeRate.yaw", rate.getAttitudeRate().getYaw()},
        {"speed.north", report.getSpeed().getNorth()}, {"speed.east", report.getSpeed().getEast()},
        {"speed.down", report.getSpeed().getDown()},
        {"acceleration.north", report.getAcceleration().getNorth()},
        {"acceleration.east", report.getAcceleration().getEast()},
        {"acceleration.down", report.getAcceleration().getDown()},
        {"wanderAngle", report.getWanderAngle()}, {"magneticHeading", report.getMagneticHeading()},
        {"covariance.positionPositionPnPn", covariance.getPositionPositionPnPn(), true},
        {"covariance.positionPositionPnPe", covariance.getPositionPositionPnPe()},
        {"covariance.positionPositionPnPd", covariance.getPositionPositionPnPd()},
        {"covariance.positionPositionPePe", covariance.getPositionPositionPePe(), true},
        {"covariance.positionPositionPePd", covariance.getPositionPositionPePd()},
        {"covariance.positionPositionPdPd", covariance.getPositionPositionPdPd(), true},
        {"covariance.positionVelocityPnVn", covariance.getPositionVelocityPnVn()},
        {"covariance.positionVelocityPnVe", covariance.getPositionVelocityPnVe()},
        {"covariance.positionVelocityPnVd", covariance.getPositionVelocityPnVd()},
        {"covariance.positionVelocityPeVe", covariance.getPositionVelocityPeVe()},
        {"covariance.positionVelocityPeVd", covariance.getPositionVelocityPeVd()},
        {"covariance.positionVelocityPdVd", covariance.getPositionVelocityPdVd()},
        {"covariance.velocityVelocityVnVn", covariance.getVelocityVelocityVnVn(), true},
        {"covariance.velocityVelocityVnVe", covariance.getVelocityVelocityVnVe()},
        {"covariance.velocityVelocityVnVd", covariance.getVelocityVelocityVnVd()},
        {"covariance.velocityVelocityVeVe", covariance.getVelocityVelocityVeVe(), true},
        {"covariance.velocityVelocityVeVd", covariance.getVelocityVelocityVeVd()},
        {"covariance.velocityVelocityVdVd", covariance.getVelocityVelocityVdVd(), true},
    };
    for (const auto& field : fields) {
        if (!std::isfinite(field.value)) return std::string(field.name) + " must be finite";
        if (field.variance && field.value < 0) return std::string(field.name) + " must be nonnegative";
    }
    return std::nullopt;
}

class ImageChannelImpl final : public ChannelCore, public ir::ImageChannel {
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::send;
    using ChannelCore::registerMetadataCallback;
    mel::RequestFor<ir::CameraCommandResp> send(ir::CameraCommand command) override { return camera_command(command); }
    mel::RequestFor<ir::NavigationReportResp> send(mel::NavigationReport report) override {
        if (in_provider_callback())
            return failed<ir::NavigationReportResp>(mel::ErrorCode::InvalidState, "submit navigation outside provider callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::NavigationReportResp>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        Callback<ir::NavigationReportResp> callback;
        ir::NavigationReportResp reply;
        mel::RequestFor<ir::NavigationReportResp> result;
        try {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<ir::NavigationReportResp>(mel::ErrorCode::InvalidState, "channel is detached");
            if (const auto error = navigation_error(report))
                return failed<ir::NavigationReportResp>(mel::ErrorCode::InvalidParameters, *error);
            if (latest_navigation_ && report.getSystemTime() <= latest_navigation_->getSystemTime())
                return failed<ir::NavigationReportResp>(mel::ErrorCode::InvalidState, "systemTime must strictly increase within this Image attachment");
            // All potentially allocating preparation precedes the no-throw commit.
            { std::lock_guard lock(callback_mutex_); callback = navigation_callback_; }
            reply.setCommandID(0); // Unassigned: NavigationReport has no correlation IDs.
            reply.setReqId(0);
            reply.setSystemTime(report.getSystemTime()); // Exact echo, not an acknowledgement clock.
            result = ready(reply);
            static_assert(std::is_nothrow_copy_constructible_v<mel::NavigationReport>);
            admit_callback(callback);
            latest_navigation_.emplace(report);
        } catch (const std::bad_alloc&) {
            return failed<ir::NavigationReportResp>(mel::ErrorCode::InsufficientLocalResources, "navigation acknowledgement allocation failed");
        }
        invoke(callback, reply, nullptr, true);
        return result;
    }
    ir::Return registerMetadataCallback(Callback<ir::BadPixelList> callback) override {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        const auto self = shared_from_this();
        const ir::BadPixelList snapshot; // Pinned defaults: zero bytes/count, empty vector.
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return ir::Return::Fail;
            if (!runtime_->camera_capabilities.known_empty_defect_map) return ir::Return::NotSupported;
            const auto result = register_callback(bad_pixel_callback_, std::move(callback));
            if (result != ir::Return::Success) return result;
            // The slot is immutable after registration. Admit before releasing
            // operations so concurrent detach drains even not-yet-entered dispatch.
            admit_callback(bad_pixel_callback_);
        }
        invoke(bad_pixel_callback_, snapshot, nullptr, true);
        return ir::Return::Success;
    }
    GIGE_UNSUPPORTED_CALLBACK(ir::OpticalDistortionMap)
    GIGE_UNSUPPORTED_CALLBACK(ir::LineOfSightReport)
    GIGE_UNSUPPORTED_CALLBACK(ir::LineOfSightQuaternion)
    GIGE_UNSUPPORTED_CALLBACK(ir::LineOfSightEuler)
    ir::Return registerMetadataCallback(Callback<ir::NavigationReportResp> callback) override {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        const auto self = shared_from_this();
        std::lock_guard operation(runtime_->operations);
        return register_callback(navigation_callback_, std::move(callback));
    }
    GIGE_UNSUPPORTED_CALLBACK(ir::CandidateObjectMessage)
    GIGE_UNSUPPORTED_CALLBACK(ir::CandidateObjectPreProcMessage)
    GIGE_UNSUPPORTED_CALLBACK(ir::NUC_TempData)
    ir::Return registerMetadataCallback(const std::function<void(ir::Channel&, const ir::LOS3D_KinematicsType* const)>&) override {
        return ir::Return::NotSupported;
    }
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::CameraCommandResp* const)> callback) override {
        return register_callback(camera_callback_, std::move(callback));
    }

private:
    void on_camera_reply(const ir::CameraCommandResp& reply) override {
        Callback<ir::CameraCommandResp> callback;
        { std::lock_guard lock(callback_mutex_); callback = camera_callback_; }
        invoke(callback, reply);
    }
    Callback<ir::CameraCommandResp> camera_callback_;
    Callback<ir::NavigationReportResp> navigation_callback_;
    Callback<ir::BadPixelList> bad_pixel_callback_;
    // One private, attachment-local report; never read by frame publication.
    std::optional<mel::NavigationReport> latest_navigation_; // Runtime::operations
};

class C2ChannelImpl final : public ChannelCore, public ir::C2Channel {
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::send;
    using ChannelCore::registerMetadataCallback;
    mel::RequestFor<ir::CameraCommandResp> send(ir::CameraCommand command) override { return camera_command(command); }
    mel::RequestFor<ir::Return> send(ir::BIT_Command command) override {
        if (in_provider_callback())
            return failed<ir::Return>(mel::ErrorCode::InvalidState, "schedule BIT outside provider callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::Return>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        ConfigurationOutcome outcome;
        std::optional<mel::BIT_Status> payload;
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<ir::Return>(mel::ErrorCode::InvalidState, "channel is detached");
            const bool initiate = !command.getInitiateBIT_ID().empty();
            const auto& selected = initiate ? command.getInitiateBIT_ID() : command.getCancelBIT_ID();
            const auto reject = [&](backend::ErrorCode code, const char* message) {
                outcome = ConfigurationOutcome::failure({code, message});
            };
            if (!selected.empty()) {
                if (selected.size() != 1)
                    reject(backend::ErrorCode::invalid_argument, "selected BIT operation requires exactly one test ID");
                else if (selected.front() != 1)
                    reject(backend::ErrorCode::not_found, "unknown BIT test ID; only numeric ID 1 is recognized");
                else if (!initiate)
                    reject(backend::ErrorCode::invalid_state, "no active test: software BIT is synchronous and has no queued work");
                else {
                    try {
                        auto snapshot = health_snapshot(*runtime_, true);
                        if (snapshot.bit_error) std::rethrow_exception(snapshot.bit_error);
                        payload = std::move(snapshot.bit);
                        outcome.success = true;
                    } catch (const std::exception& error) {
                        outcome = ConfigurationOutcome::failure({backend::ErrorCode::resource_exhausted,
                            std::string("BIT report preparation failed: ") + error.what()});
                        runtime_->report(outcome.description);
                    } catch (...) {
                        reject(backend::ErrorCode::resource_exhausted, "BIT report preparation failed: unknown internal failure");
                        runtime_->report(outcome.description);
                    }
                }
            } else if (!command.getClearFaultCode().empty())
                reject(backend::ErrorCode::unsupported, "clearFaultCode is unsupported; real recovery retires current software faults");
            else reject(backend::ErrorCode::invalid_argument, "BIT command has no selected operation");
        }
        auto result = outcome.error ? failed<ir::Return>(map_error(outcome.error->code), outcome.description)
                                    : ready(ir::Return::Success);
        if (payload) {
            Callback<mel::BIT_Status> callback;
            { std::lock_guard lock(callback_mutex_); callback = bit_callback_; }
            invoke(callback, *payload);
        }
        on_command_status(outcome.status(command.getCommandID()));
        return result;
    }
    GIGE_UNSUPPORTED_SEND(ir::CalibrationConfigurationCmd, mel::CalibrationConfiguration)
    GIGE_UNSUPPORTED_SEND(ir::CalibrationStatusCmd, mel::CalibrationStatus)
    GIGE_UNSUPPORTED_SEND(ir::CameraProtectCmd, ir::CameraProtectCmdResp)
    GIGE_UNSUPPORTED_SEND(ir::EraseCommand, ir::EraseCommandType)
    GIGE_UNSUPPORTED_SEND(ir::SystemTrackDataResponse, ir::CommandStatus)
    mel::RequestFor<ir::MFA_Mode> send(ir::ModeCmd command) override {
        // Same admission precedence as configuration/BIT: no recursive status
        // dispatch or operation-lock wait from any provider callback thread.
        if (in_provider_callback())
            return failed<ir::MFA_Mode>(mel::ErrorCode::InvalidState, "submit ModeCmd outside provider callbacks");
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return failed<ir::MFA_Mode>(mel::ErrorCode::InvalidState, "control is closing");
        const auto self = shared_from_this();
        ConfigurationOutcome outcome;
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return failed<ir::MFA_Mode>(mel::ErrorCode::InvalidState, "channel is detached");
            const auto state = command.getState();
            const auto mode = command.getMode();
            const bool operational = state == mel::MFA_State::Operate || state == mel::MFA_State::OperateRxOnly ||
                                     state == mel::MFA_State::OperateTxOnly;
            const bool compatible_mode = operational ? mode != ir::MFA_Mode::Unused : mode == ir::MFA_Mode::Unused;
            const auto reject = [&](backend::ErrorCode code, const char* message) {
                outcome = ConfigurationOutcome::failure({code, message});
            };
            if (state <= mel::MFA_State::NotSet || state >= mel::MFA_State::MaxExclusive ||
                mode > ir::MFA_Mode::ScanBarSched || !compatible_mode)
                reject(backend::ErrorCode::invalid_argument, "invalid ModeCmd state/mode combination");
            else if (state == mel::MFA_State::Standby && !unset_scan(command.getScanParameters()))
                reject(backend::ErrorCode::invalid_argument, "Standby requires every ScanParam field at its upstream default");
            else if (state != mel::MFA_State::Standby)
                reject(backend::ErrorCode::unsupported, "only Standby/Unused is implemented");
            else {
                bool valid, active;
                {
                    // Do not prepare a Health snapshot: it may commit BIT
                    // observations. Readiness and ownership are stable against
                    // lifecycle/configuration changes under operations.
                    std::lock_guard lock(runtime_->state_mutex);
                    valid = runtime_->initialized && runtime_->configuration_valid;
                    active = runtime_->image_active;
                }
                if (!valid)
                    reject(backend::ErrorCode::invalid_state, "Standby requires initialized provider and known-valid configuration");
                else if (active) {
                    const auto image_pin = runtime_->image_channel.lock();
                    if (!image_pin || image_pin->is_detached())
                        reject(backend::ErrorCode::invalid_state, "owned Image acquisition is unavailable for shutdown");
                    else {
                        // Reuse disable's Image implementation, not disable()
                        // itself: operations is already held. No buffer release,
                        // unbinding, detach or diagnostic-history reset.
                        image_pin->stop_locked();
                        outcome.success = true;
                    }
                } else outcome.success = true; // Idempotent, no backend I/O.
            }
        }
        auto result = outcome.error ? failed<ir::MFA_Mode>(map_error(outcome.error->code), outcome.description)
                                    : ready(ir::MFA_Mode::Unused);
        on_command_status(outcome.status(command.getCommandID()));
        return result;
    }
    mel::RequestFor<ir::Return> send(ir::ConfigSetCommand command) override { return config_set(command); }
    ir::Return registerMetadataCallback(Callback<mel::BIT_Configuration> callback) override {
        return register_initial(catalogue_callback_, std::move(callback), [&] { return bit_catalogue(); });
    }
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::CommandStatus* const)> callback) override {
        return register_callback(status_callback_, std::move(callback));
    }
    GIGE_UNSUPPORTED_CALLBACK(mel::CalibrationConfiguration)
    GIGE_UNSUPPORTED_CALLBACK(ir::CameraProtectCmdResp)
    GIGE_UNSUPPORTED_CALLBACK(mel::CalibrationStatus)
    ir::Return registerMetadataCallback(Callback<mel::BIT_Status> callback) override {
        return register_initial(bit_callback_, std::move(callback), [&] {
            auto snapshot = health_snapshot(*runtime_);
            if (snapshot.bit_error) std::rethrow_exception(snapshot.bit_error);
            return std::move(*snapshot.bit);
        });
    }
private:
    template <class T, class Prepare>
    ir::Return register_initial(Callback<T>& slot, Callback<T> callback, Prepare prepare) {
        if (in_provider_callback() || is_detached()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        const auto self = shared_from_this();
        if (!callback) return is_detached() ? ir::Return::Fail : ir::Return::BadPointer;
        std::optional<T> payload;
        {
            std::lock_guard operation(runtime_->operations);
            if (is_detached()) return ir::Return::Fail;
            if (!callback) return ir::Return::BadPointer;
            { std::lock_guard lock(callback_mutex_); if (slot) return ir::Return::Fail; }
            try { payload.emplace(prepare()); }
            catch (const std::exception& error) { runtime_->report(error.what()); return ir::Return::Fail; }
            catch (...) { runtime_->report("BIT initial publication preparation failed"); return ir::Return::Fail; }
            const auto registered = register_callback(slot, callback);
            if (registered != ir::Return::Success) return registered;
        }
        invoke(callback, *payload);
        return ir::Return::Success;
    }
    void on_command_status(const ir::CommandStatus& status) override {
        Callback<ir::CommandStatus> callback;
        { std::lock_guard lock(callback_mutex_); callback = status_callback_; }
        invoke(callback, status);
    }
    Callback<ir::CommandStatus> status_callback_;
    Callback<mel::BIT_Configuration> catalogue_callback_;
    Callback<mel::BIT_Status> bit_callback_;
};

class HealthChannelImpl final : public ChannelCore, public ir::HealthStatusChannel {
    struct Publisher {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<bool> running{true};
        bool prompted{true};
    };
public:
    using ChannelCore::ChannelCore;
    using ChannelCore::send;
    using ChannelCore::registerMetadataCallback;
    ~HealthChannelImpl() override { stop_auxiliary_locked(); }

    ir::Return enable() override {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (is_detached()) return ir::Return::Fail;
        if (publisher_ && publisher_->running) return ir::Return::Success;
        try {
            auto state = std::make_shared<Publisher>();
            { std::lock_guard lock(callback_mutex_); publisher_ = state; }
            worker_ = std::thread([state, weak = weak_from_this(), runtime = runtime_] {
                run(state, weak, runtime);
            });
            return ir::Return::Success;
        } catch (const std::exception& error) {
            stop_auxiliary_locked();
            runtime_->report(error.what());
            return ir::Return::Fail;
        }
    }
    ir::Return registerMetadataCallback(Callback<mel::MFA_Status> callback) override {
        const auto result = register_callback(status_callback_, std::move(callback));
        if (result == ir::Return::Success) prompt();
        return result;
    }
    ir::Return registerMetadataCallback(Callback<mel::MFA_StatusDetailed> callback) override {
        const auto result = register_callback(detailed_callback_, std::move(callback));
        if (result == ir::Return::Success) prompt();
        return result;
    }
    GIGE_UNSUPPORTED_CALLBACK(ir::LFStatus)
    ir::Return registerMetadataCallback(Callback<mel::BIT_Status> callback) override {
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        const auto self = shared_from_this();
        const auto result = register_callback(bit_callback_, std::move(callback));
        if (result == ir::Return::Success) prompt();
        return result;
    }
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::SubsystemStatusResp* const)> callback) override {
        // Same registration contract and lifetime pins as BIT_Status; dispatch
        // only on the existing Health worker, never inline on this thread.
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        const auto self = shared_from_this();
        const auto result = register_callback(subsystem_callback_, std::move(callback));
        if (result == ir::Return::Success) prompt();
        return result;
    }
    GIGE_UNSUPPORTED_CALLBACK(mel::DiscreteStatus)
    GIGE_UNSUPPORTED_CALLBACK(mel::MFA_SecurityAuditRecord)
    GIGE_UNSUPPORTED_CALLBACK(ir::NUC_TempData)

private:
    void prompt() {
        std::shared_ptr<Publisher> state;
        { std::lock_guard lock(callback_mutex_); state = publisher_; }
        if (!state) return;
        { std::lock_guard lock(state->mutex); state->prompted = true; }
        state->changed.notify_all();
    }
    void stop_auxiliary_locked() override {
        std::shared_ptr<Publisher> state;
        { std::lock_guard lock(callback_mutex_); state = publisher_; }
        if (state) {
            { std::lock_guard lock(state->mutex); state->running = false; }
            state->changed.notify_all();
        }
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) worker_.detach();
            else worker_.join();
        }
    }
    void publish(const Publisher& publisher) {
        Callback<mel::MFA_Status> status_callback;
        Callback<mel::MFA_StatusDetailed> detailed_callback;
        Callback<mel::BIT_Status> bit_callback;
        Callback<ir::SubsystemStatusResp> subsystem_callback;
        {
            std::lock_guard lock(callback_mutex_);
            status_callback = status_callback_;
            detailed_callback = detailed_callback_;
            bit_callback = bit_callback_;
            subsystem_callback = subsystem_callback_;
        }
        // Subsystem-only subscribers still need the canonical BIT commit: its
        // revision and current-fault observations feed BIT_report.
        const bool want_subsystem = static_cast<bool>(subsystem_callback);
        const auto s = health_snapshot(*runtime_, false, static_cast<bool>(bit_callback) || want_subsystem,
                                       want_subsystem);
        const auto state = !s.initialized ? mel::MFA_State::Unknown :
            !s.configuration_valid || (s.active && s.failed) ? mel::MFA_State::Degraded :
            s.active ? mel::MFA_State::Operate : mel::MFA_State::Standby;
        const auto description = !s.initialized ? "Provider not initialized" :
            !s.configuration_valid ? "Actual camera configuration is not known valid" :
            s.active && s.failed ? "Owned Image acquisition has observed a terminal failure; explicitly disable before recovery" :
            s.active ? "Image acquisition is logically owned without an observed terminal failure" :
            "Idle with known valid configuration; explicit Image enable may be attempted (not a physical connection probe)";
        mel::MFA_Status status(state, description,
            "Experimental Mono8 image-stream profile with provider-software BIT and bounded Standby ModeCmd; no task scheduling, scanning, hardware BIT or calibration",
            mel::StateTransitionStatus::NotTransitioning,
            mel::About(std::string(ProviderSoftware::name) + " (software provider)", "", ProviderSoftware::version_text, "", ""), {});
        mel::MFA_StatusDetailed detailed;
        const auto add = [&](const char* name, std::string value) {
            detailed.addStatus(mel::NameValuePair(std::string("gige_mel.") + name, std::move(value)));
        };
        add("health_schema", "1");
        add("backend_id", runtime_->backend_id);
        add("device_id", runtime_->settings.device_id);
        add("configuration_valid", s.configuration_valid ? "true" : "false");
        add("configuration_generation", std::to_string(s.generation));
        add("acquisition_state", !s.active ? "idle" : s.failed ? "faulted" : "running");
        add("delivered_frames", std::to_string(s.statistics.delivered_frames));
        add("dropped_no_buffer", std::to_string(s.statistics.dropped_no_buffer));
        add("rejected_frames", std::to_string(s.statistics.rejected_frames));
        add("callback_failures", std::to_string(s.statistics.callback_failures));
        add("stream_failures", std::to_string(s.statistics.stream_failures));
        add("last_error_history", s.statistics.last_error);
        invoke(status_callback, status, &publisher.running);
        invoke(detailed_callback, detailed, &publisher.running);
        // A failed canonical BIT preparation suppresses both BIT-derived
        // reports for this sample; Status/Detailed above remain published.
        if (s.bit) invoke(bit_callback, *s.bit, &publisher.running);
        else if (s.bit_error) {
            try { std::rethrow_exception(s.bit_error); }
            catch (const std::exception& error) { runtime_->report(std::string("Health BIT publication preparation failed: ") + error.what()); }
            catch (...) { runtime_->report("Health BIT publication preparation failed"); }
        }
        // Appended last; dispatches the revision paired with this capture.
        if (s.subsystem) invoke(subsystem_callback, *s.subsystem, &publisher.running);
        else if (s.subsystem_error) {
            try { std::rethrow_exception(s.subsystem_error); }
            catch (const std::exception& error) { runtime_->report(std::string("Health subsystem publication preparation failed: ") + error.what()); }
            catch (...) { runtime_->report("Health subsystem publication preparation failed"); }
        }
    }
    static void run(const std::shared_ptr<Publisher>& state, const std::weak_ptr<ChannelCore>& weak,
                    const std::shared_ptr<Runtime>& runtime) noexcept {
        while (state->running) {
            {
                std::lock_guard lock(state->mutex);
                state->prompted = false;
            }
            try {
                // Pins are scoped to preparation/dispatch, NEVER the wait. Their
                // destruction may itself tear down Control on this worker.
                const auto control_pin = runtime->control.load().lock();
                const auto channel = std::dynamic_pointer_cast<HealthChannelImpl>(weak.lock());
                if (!control_pin || !channel || channel->is_detached()) break;
                channel->publish(*state);
            } catch (...) {
                try { runtime->report("Health publication preparation failed"); } catch (...) {}
            }
            std::unique_lock lock(state->mutex);
            state->changed.wait_for(lock, 100ms, [&] { return !state->running || state->prompted; });
        }
    }
    Callback<mel::MFA_Status> status_callback_;
    Callback<mel::MFA_StatusDetailed> detailed_callback_;
    Callback<mel::BIT_Status> bit_callback_;
    Callback<ir::SubsystemStatusResp> subsystem_callback_;
    std::shared_ptr<Publisher> publisher_; // writes under callback_mutex AND operations
    std::thread worker_; // operations; defensive destructor on its own worker
};
#undef GIGE_UNSUPPORTED_SEND
#undef GIGE_UNSUPPORTED_CALLBACK

class ControlImpl final : public ir::Control {
public:
    explicit ControlImpl(std::shared_ptr<Runtime> runtime) : runtime_(std::move(runtime)) {}
    ~ControlImpl() override {
        std::lock_guard operation(runtime_->operations);
        for (const auto& channel : channels_) channel->detach_locked();
    }

    ir::Return init(const std::string& aperture_config_id) override {
        if (in_provider_callback()) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        if (!aperture_config_id.empty() && aperture_config_id != "mono8-bootstrap") return ir::Return::NotSupported;
        try {
            if (!runtime_->initialized) {
                auto opened = runtime_->backend->open(runtime_->settings.device_id);
                if (!opened) { runtime_->report(opened.error().message); return ir::Return::Fail; }
                auto device = std::move(opened).value();
                auto actual = device->configuration();
                const auto caps = device->capabilities();
                if (!actual || !valid_layout(actual.value(), caps)) {
                    (void)device->close();
                    runtime_->report("backend initial configuration does not satisfy the selected monochrome profile");
                    return ir::Return::Fail;
                }
                {
                    std::lock_guard lock(runtime_->state_mutex);
                    runtime_->device = std::move(device);
                    runtime_->configuration = actual.value();
                    runtime_->camera_capabilities = caps;
                    runtime_->configuration_valid = true;
                    runtime_->initialized = true;
                }
            }
            if (capabilities_.empty()) {
                mel::UCI_ID image_id, c2_id, health_id;
                std::array<std::uint8_t, mel::UUID_SIZE> image_uuid{}, c2_uuid{};
                image_uuid.back() = 1;
                c2_uuid.back() = 2;
                image_id.setUUID(image_uuid);
                c2_id.setUUID(c2_uuid);
                image_id.setDescriptiveLabel(runtime_->settings.instance + "/image");
                c2_id.setDescriptiveLabel(runtime_->settings.instance + "/c2");
                auto health_uuid = c2_uuid;
                health_uuid.back() = 3;
                health_id.setUUID(health_uuid);
                health_id.setDescriptiveLabel(runtime_->settings.instance + "/health");
                capabilities_.push_back(capability(*runtime_, ir::ChannelType::IRSTImage, image_id));
                capabilities_.push_back(capability(*runtime_, ir::ChannelType::CommandAndControl, c2_id));
                capabilities_.push_back(capability(*runtime_, ir::ChannelType::HealthAndStatus, health_id));
            }
            return ir::Return::Success;
        } catch (const std::exception& error) {
            runtime_->report(error.what());
            return ir::Return::Fail;
        }
    }
    const std::vector<ir::ChannelCapability>& getCapabilities() const override { return capabilities_; }
    mel::VersionInfo getVersionInfo() const override {
        // API version 4 / numeric library version 1 are unchanged local fields;
        // the text carries the provider software version.
        return mel::VersionInfo(4, 1, ProviderSoftware::name, std::string(ProviderSoftware::version_text) +
            " experimental Mono8 image-stream profile against pinned MEL 4.0; not full MEL conformance");
    }
    std::shared_ptr<ir::Channel> attachChannel(const ir::Config& config) override {
        if (in_provider_callback()) return {};
        std::lock_guard operation(runtime_->operations);
        if (!runtime_->initialized) return {};
        const auto type = config.getChannelType();
        if (type != ir::ChannelType::IRSTImage && type != ir::ChannelType::CommandAndControl &&
            type != ir::ChannelType::HealthAndStatus) return {};
        const auto found = std::find_if(capabilities_.begin(), capabilities_.end(), [&](const auto& cap) {
            return cap.getChanID().getUUID() == config.getChanID().getUUID() && cap.getChannelTypes().front() == type;
        });
        if (found == capabilities_.end()) return {};
        if (std::any_of(channels_.begin(), channels_.end(), [type](const auto& channel) { return channel->type() == type; })) return {};
        if (type == ir::ChannelType::IRSTImage && !config.getImgLstnr()) return {};
        std::shared_ptr<ChannelCore> channel;
        if (type == ir::ChannelType::IRSTImage) channel = std::make_shared<ImageChannelImpl>(runtime_, config);
        else if (type == ir::ChannelType::CommandAndControl) channel = std::make_shared<C2ChannelImpl>(runtime_, config);
        else channel = std::make_shared<HealthChannelImpl>(runtime_, config);
        channels_.push_back(channel);
        if (type == ir::ChannelType::IRSTImage) runtime_->image_channel = channel;
        return channel;
    }
    ir::Return detachChannel(std::shared_ptr<ir::Channel> channel) override {
        if (in_provider_callback()) return ir::Return::Fail;
        const auto control_pin = runtime_->control.load().lock();
        if (!control_pin) return ir::Return::Fail;
        std::lock_guard operation(runtime_->operations);
        const auto found = std::find(channels_.begin(), channels_.end(), channel);
        if (found == channels_.end()) return ir::Return::BadPointer;
        (*found)->detach_locked();
        channels_.erase(found);
        return ir::Return::Success;
    }

private:
    std::shared_ptr<Runtime> runtime_;
    // An immutable discovery snapshot once init returns. Per-channel capability
    // queries report current configuration, avoiding races through const& data.
    std::vector<ir::ChannelCapability> capabilities_;
    std::vector<std::shared_ptr<ChannelCore>> channels_;
};

class Manager final : public API_Manager {
public:
    explicit Manager(std::shared_ptr<Runtime> value) : runtime(std::move(value)) {}
    std::shared_ptr<Runtime> runtime;
};

} // namespace

std::shared_ptr<API_Manager> make_api_manager(ProviderSettings settings, std::shared_ptr<backend::ICameraBackend> backend) {
    if (!backend || settings.instance.empty() || settings.device_id.empty() || settings.maximum_registered_buffers == 0 ||
        settings.maximum_registered_buffers > 256 || !std::isfinite(settings.horizontal_radians_per_pixel) ||
        !std::isfinite(settings.vertical_radians_per_pixel) || settings.horizontal_radians_per_pixel <= 0 ||
        settings.vertical_radians_per_pixel <= 0 || settings.timestamp_domain == backend::ClockDomain::unspecified) return {};
    auto runtime = std::make_shared<Runtime>();
    runtime->settings = std::move(settings);
    runtime->backend_id = backend->id();
    runtime->backend = std::move(backend);
    return std::make_shared<Manager>(std::move(runtime));
}

ProviderStatistics provider_statistics(const std::shared_ptr<API_Manager>& manager) {
    const auto concrete = std::dynamic_pointer_cast<Manager>(manager);
    if (!concrete) return {};
    const auto& r = *concrete->runtime;
    std::lock_guard lock(r.state_mutex);
    return {r.delivered.load(), r.dropped.load(), r.rejected.load(), r.callback_failures.load(), r.stream_failures.load(), r.last_error};
}

std::shared_ptr<ir::Control> control_for(std::string_view instance, const std::shared_ptr<API_Manager>& manager) {
    const auto concrete = std::dynamic_pointer_cast<Manager>(manager);
    if (!concrete || concrete->runtime->settings.instance != instance) return {};
    const auto& runtime = concrete->runtime;
    if (in_provider_callback()) return {};
    std::lock_guard operation(runtime->operations);
    if (auto control = runtime->control.load().lock()) return control;
    auto control = std::make_shared<ControlImpl>(runtime);
    runtime->control.store(control);
    return control;
}
std::shared_ptr<ir::Buffer> buffer_for(std::string_view instance, const std::shared_ptr<API_Manager>& manager) {
    const auto concrete = std::dynamic_pointer_cast<Manager>(manager);
    if (!concrete || concrete->runtime->settings.instance != instance) return {};
    return std::make_shared<HostBuffer>(concrete->runtime->affinity);
}

} // namespace gige_mel
