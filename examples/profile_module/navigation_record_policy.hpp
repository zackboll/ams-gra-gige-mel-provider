#pragma once

#include "profiles/camera_profile.hpp"
#include "profiles/navigation_handler.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <functional>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

// TEST ONLY example of an optional camera-profile navigation handler. It
// encodes selected fields into a FICTIONAL TEST RECORD. The record, its magic,
// field identifiers and solution-state codes are deterministic test values. They
// are NOT a detector, AMS-GRA, GigE Vision, GVCP or register format and contain
// no device addresses. No physical camera support is claimed.
namespace navigation_record {
namespace b = gige_mel::backend;
namespace p = b::profiles;
inline constexpr std::string_view id = "test_navigation_record_v1";
inline constexpr std::string_view format = "test.fictional-navigation-record.v1";
inline constexpr std::array<char, 8> magic{'T', 'E', 'S', 'T', 'O', 'N', 'L', 'Y'};
inline constexpr std::uint16_t record_version = 1;
/// Fixture policy limit on each position variance, in the report's own units.
inline constexpr double maximum_position_variance = 1.0e6;
inline std::atomic<unsigned> live_profiles{}, live_states{};

/// Test-only field identifiers. Values are little-endian: int64 nanoseconds,
/// IEEE-754 binary64 bit patterns, or one fixture state code byte.
enum class Field : std::uint16_t {
    system_time_ns = 0x0001, solution_state = 0x0002,
    latitude = 0x0010, longitude = 0x0011, altitude = 0x0012,
    roll = 0x0020, pitch = 0x0021, yaw = 0x0022,
    variance_pn = 0x0030, variance_pe = 0x0031, variance_pd = 0x0032,
};

/// Fixture codes; aligning has none because this profile does not accept it.
inline std::uint8_t state_code(b::NavigationSolutionState state) {
    switch (state) {
    case b::NavigationSolutionState::free_inertial: return 1;
    case b::NavigationSolutionState::gps: return 2;
    case b::NavigationSolutionState::blended: return 3;
    case b::NavigationSolutionState::aligning: break;
    }
    throw std::logic_error("no fixture code for this solution state");
}

/// Test instrumentation and fault injection, shared by the test that built the
/// profile. Configure on_prepare before concurrent use; counters are atomic.
struct Controls {
    enum class Fault { none, error, throw_runtime, throw_allocation, empty_format, alter_report };
    std::atomic<Fault> fault{Fault::none};
    std::atomic<unsigned> prepared{}, rejected{}, entered{}, states{}, retired{};
    std::function<void()> on_prepare;
};

/// Append one TLV field: uint16 id, uint16 length, then little-endian value bytes.
inline void put(std::vector<std::byte>& out, Field field, std::uint64_t value, std::uint16_t length) {
    const auto identifier = static_cast<std::uint16_t>(field);
    for (unsigned i = 0; i < 2; ++i) out.push_back(static_cast<std::byte>((identifier >> (8 * i)) & 0xffu));
    for (unsigned i = 0; i < 2; ++i) out.push_back(static_cast<std::byte>((length >> (8 * i)) & 0xffu));
    for (unsigned i = 0; i < length; ++i) out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffu));
}

/// Encode the complete fictional record for a policy-admitted report.
inline std::vector<std::byte> encode(const b::NavigationReport& r) {
    const std::pair<Field, double> reals[]{
        {Field::latitude, r.latitude}, {Field::longitude, r.longitude}, {Field::altitude, r.altitude},
        {Field::roll, r.attitude.roll}, {Field::pitch, r.attitude.pitch}, {Field::yaw, r.attitude.yaw},
        {Field::variance_pn, r.position_velocity_covariance_uncertainty.position_position_pn_pn},
        {Field::variance_pe, r.position_velocity_covariance_uncertainty.position_position_pe_pe},
        {Field::variance_pd, r.position_velocity_covariance_uncertainty.position_position_pd_pd}};
    std::vector<std::byte> out;
    for (const char c : magic) out.push_back(static_cast<std::byte>(c));
    const auto count = static_cast<std::uint16_t>(std::size(reals) + 2);
    for (const auto value : {record_version, count})
        for (unsigned i = 0; i < 2; ++i) out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffu));
    put(out, Field::system_time_ns, static_cast<std::uint64_t>(r.system_time.count()), 8);
    put(out, Field::solution_state, state_code(r.state), 1);
    for (const auto& [field, value] : reals) put(out, field, std::bit_cast<std::uint64_t>(value), 8);
    return out;
}

/// Decode for recording executors and tests: field ID -> raw little-endian value.
/// Returns nullopt for anything that is not exactly a well-formed fixture record.
inline std::optional<std::vector<std::pair<std::uint16_t, std::uint64_t>>> decode(std::span<const std::byte> in) {
    const auto read = [&](std::size_t at, unsigned length) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < length; ++i) value |= std::to_integer<std::uint64_t>(in[at + i]) << (8 * i);
        return value;
    };
    if (in.size() < 12) return std::nullopt;
    for (std::size_t i = 0; i < magic.size(); ++i)
        if (in[i] != static_cast<std::byte>(magic[i])) return std::nullopt;
    if (read(8, 2) != record_version) return std::nullopt;
    const auto count = read(10, 2);
    std::vector<std::pair<std::uint16_t, std::uint64_t>> fields;
    std::size_t at = 12;
    for (std::uint64_t n = 0; n < count; ++n) {
        if (in.size() - at < 4) return std::nullopt;
        const auto field = static_cast<std::uint16_t>(read(at, 2));
        const auto length = static_cast<unsigned>(read(at + 2, 2));
        if (length > 8 || in.size() - at - 4 < length) return std::nullopt;
        fields.emplace_back(field, read(at + 4, length));
        at += 4 + length;
    }
    if (at != in.size()) return std::nullopt;
    return fields;
}

/// @brief Profile B state: camera behavior delegated to the shared monochrome
/// representation, plus the optional navigation handler with its own policy.
class State final : public p::ICameraProfileState, public p::IProfileNavigationHandler {
public:
    State(std::shared_ptr<const p::ICameraProfileState> representation, std::shared_ptr<Controls> controls)
        : representation_(std::move(representation)), controls_(std::move(controls)) {
        ++controls_->states; ++live_states;
    }
    ~State() override { ++controls_->retired; --live_states; }
    b::CameraCapabilities capabilities() const override { return representation_->capabilities(); }
    b::Result<b::CameraConfiguration> configuration(b::device_features::IDeviceFeatures& features,
                                                   const p::ConnectionCheck& check) const override {
        return representation_->configuration(features, check);
    }
    b::Result<b::CameraConfiguration> apply(b::device_features::IDeviceFeatures& features,
        const b::CameraConfiguration& request, const p::ConnectionCheck& check, const p::BeforeWrite& before) const override {
        return representation_->apply(features, request, check, before);
    }
    b::Result<b::CameraConfiguration> preflight(b::device_features::IDeviceFeatures& features) const override {
        return representation_->preflight(features);
    }
    p::FrameDecision validate_frame(const p::NativeFrameFacts& facts, const b::CameraConfiguration& frozen,
                                    b::PixelFormat pixel_format) const override {
        return representation_->validate_frame(facts, frozen, pixel_format);
    }

    /// Fixture policy: Aligning is unsupported; each position variance must not
    /// exceed maximum_position_variance. Values are copied, never converted.
    b::Result<p::NavigationOperation> prepare_navigation(const b::NavigationReport& report) const override {
        using Fault = Controls::Fault;
        ++controls_->entered;
        if (controls_->on_prepare) controls_->on_prepare();
        switch (controls_->fault.load()) {
        case Fault::error: return b::Error{b::ErrorCode::io_error, "test profile injected preparation error"};
        case Fault::throw_runtime: throw std::runtime_error("test profile injected preparation exception");
        case Fault::throw_allocation: throw std::bad_alloc{};
        default: break;
        }
        if (report.state == b::NavigationSolutionState::aligning) {
            ++controls_->rejected;
            return b::Error{b::ErrorCode::unsupported, "test profile does not accept Aligning navigation solutions"};
        }
        const auto& c = report.position_velocity_covariance_uncertainty;
        for (const double variance : {c.position_position_pn_pn, c.position_position_pe_pe, c.position_position_pd_pd}) {
            if (variance > maximum_position_variance) {
                ++controls_->rejected;
                return b::Error{b::ErrorCode::invalid_argument, "test profile position variance exceeds its fixture limit"};
            }
        }
        p::NavigationOperation operation{std::string(format), report, encode(report)};
        if (controls_->fault.load() == Fault::empty_format) operation.format.clear();
        if (controls_->fault.load() == Fault::alter_report) operation.report.latitude = -operation.report.latitude;
        ++controls_->prepared;
        return operation;
    }
private:
    const std::shared_ptr<const p::ICameraProfileState> representation_;
    const std::shared_ptr<Controls> controls_;
};

class Profile final : public p::ICameraProfile {
public:
    explicit Profile(std::shared_ptr<Controls> controls) : controls_(std::move(controls)) { ++live_profiles; }
    ~Profile() override { --live_profiles; }
    std::string_view id() const noexcept override { return navigation_record::id; }
    b::Result<std::shared_ptr<const p::ICameraProfileState>> open(b::device_features::IDeviceFeatures& features) const override {
        auto representation = p::make_mono_free_running_profile()->open(features);
        if (!representation) return representation.error();
        return std::shared_ptr<const p::ICameraProfileState>(
            std::make_shared<State>(std::move(representation).value(), controls_));
    }
private:
    const std::shared_ptr<Controls> controls_;
};
inline std::shared_ptr<const p::ICameraProfile> make(std::shared_ptr<Controls> controls = std::make_shared<Controls>()) {
    return std::make_shared<Profile>(std::move(controls));
}
} // namespace navigation_record
