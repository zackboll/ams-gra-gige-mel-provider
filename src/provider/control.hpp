#pragma once

#include "runtime.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

class ControlImpl final : public ir::Control {
public:
    explicit ControlImpl(std::shared_ptr<Runtime> runtime);
    ~ControlImpl() override;

    ir::Return init(const std::string& aperture_config_id) override;
    const std::vector<ir::ChannelCapability>& getCapabilities() const override;
    mel::VersionInfo getVersionInfo() const override;
    std::shared_ptr<ir::Channel> attachChannel(const ir::Config& config) override;
    ir::Return detachChannel(std::shared_ptr<ir::Channel> channel) override;

private:
    std::shared_ptr<Runtime> runtime_;
    // An immutable discovery snapshot once init returns. Per-channel capability
    // queries report current configuration, avoiding races through const& data.
    std::vector<ir::ChannelCapability> capabilities_;
    std::vector<std::shared_ptr<ChannelCore>> channels_;
};

} // namespace provider_detail
} // namespace gige_mel
