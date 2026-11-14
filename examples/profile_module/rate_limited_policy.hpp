#pragma once

#include "profiles/camera_profile.hpp"
#include <algorithm>
#include <atomic>

// Example policy, independently compiled without a provider or camera SDK link.
// It composes the existing representation algorithms, but admits only <=20 Hz
// writes/runs and rejects odd frame IDs. No physical camera support is claimed.
namespace rate_limited {
namespace b = gige_mel::backend;
namespace p = b::profiles;
inline constexpr std::string_view id = "test_rate_limited_v1";
inline std::atomic<unsigned> live_profiles{}, live_states{};
struct Counts {
    std::atomic<unsigned> created{}, destroyed{}, opened{}, states{}, retired{};
    std::atomic<unsigned> reads{}, applies{}, preflights{}, frames{};
};

class State final : public p::ICameraProfileState {
public:
    // Different state structure: owned delegated representation, fixed admission
    // limit and independent instrumentation, not mono::NativeSnapshot inheritance.
    State(std::shared_ptr<const p::ICameraProfileState> representation, std::shared_ptr<Counts> counts)
        : representation_(std::move(representation)), counts_(std::move(counts)) { ++counts_->states; ++live_states; }
    ~State() override { ++counts_->retired; --live_states; }
    b::CameraCapabilities capabilities() const override {
        auto caps = representation_->capabilities();
        caps.maximum_frame_rate_hz = std::min(caps.maximum_frame_rate_hz, maximum_rate_);
        return caps;
    }
    b::Result<b::CameraConfiguration> configuration(b::device_features::IDeviceFeatures& features,
                                                   const p::ConnectionCheck& check) const override {
        ++counts_->reads;
        return representation_->configuration(features, check); // truthful, even above admission limit
    }
    b::Result<b::CameraConfiguration> apply(b::device_features::IDeviceFeatures& features,
        const b::CameraConfiguration& request, const p::ConnectionCheck& check, const p::BeforeWrite& before) const override {
        ++counts_->applies;
        if (auto status = check(); !status) return status.error();
        if (request.frame_rate_hz > maximum_rate_)
            return b::Error{b::ErrorCode::unsupported, "test profile admits at most 20 Hz; no writes"};
        return representation_->apply(features, request, check, before);
    }
    b::Result<b::CameraConfiguration> preflight(b::device_features::IDeviceFeatures& features) const override {
        ++counts_->preflights;
        auto frozen = representation_->preflight(features);
        if (!frozen) return frozen.error();
        if (frozen.value().frame_rate_hz > maximum_rate_)
            return b::Error{b::ErrorCode::unsupported, "test profile native rate exceeds 20 Hz"};
        return frozen;
    }
    p::FrameDecision validate_frame(const p::NativeFrameFacts& facts, const b::CameraConfiguration& frozen,
                                    b::PixelFormat format) const override {
        ++counts_->frames;
        auto decision = representation_->validate_frame(facts, frozen, format);
        if (decision.verdict == p::FrameVerdict::accept && facts.frame_id % 2 != 0)
            return {p::FrameVerdict::reject, b::ErrorCode::unsupported, 0, 0, "test profile rejects odd frame IDs"};
        return decision;
    }
private:
    const std::shared_ptr<const p::ICameraProfileState> representation_;
    const std::uint32_t maximum_rate_{20};
    const std::shared_ptr<Counts> counts_;
};

class Profile final : public p::ICameraProfile {
public:
    explicit Profile(std::shared_ptr<Counts> counts) : counts_(std::move(counts)) { ++counts_->created; ++live_profiles; }
    ~Profile() override { ++counts_->destroyed; --live_profiles; }
    std::string_view id() const noexcept override { return rate_limited::id; }
    b::Result<std::shared_ptr<const p::ICameraProfileState>> open(b::device_features::IDeviceFeatures& features) const override {
        ++counts_->opened;
        auto representation = p::make_mono_free_running_profile()->open(features);
        if (!representation) return representation.error();
        return std::shared_ptr<const p::ICameraProfileState>(
            std::make_shared<State>(std::move(representation).value(), counts_));
    }
private:
    const std::shared_ptr<Counts> counts_;
};
inline std::shared_ptr<const p::ICameraProfile> make(std::shared_ptr<Counts> counts = std::make_shared<Counts>()) {
    return std::make_shared<Profile>(std::move(counts));
}
} // namespace rate_limited