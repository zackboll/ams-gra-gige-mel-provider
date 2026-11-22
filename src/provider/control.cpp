#include "control.hpp"
#include "channel_core.hpp"
#include "channels.hpp"
#include "capabilities.hpp"
#include "software_identity.hpp"
#include "tasking.hpp"
#include "test_hooks.hpp"
#include <irmel/library/stacked-image/StackedImageListener.h>
#include <algorithm>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

ControlImpl::ControlImpl(std::shared_ptr<Runtime> runtime) : runtime_(std::move(runtime)) {}

ControlImpl::~ControlImpl() {
    std::lock_guard operation(runtime_->operations);
    for (const auto& channel : channels_) channel->detach_locked();
    retire_tasking(*runtime_);
}

ir::Return ControlImpl::init(const std::string& aperture_config_id) {
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
            backend::LosSupport los;
            backend::StackSupport stacks;
            backend::TaskExecutionSupport tasks;
            if (auto* source = dynamic_cast<backend::ITaskExecutionSource*>(device.get())) {
                tasks = source->task_execution_support();
                if (!backend::valid_task_support(tasks)) tasks = {};
            }
            if (auto* source = dynamic_cast<backend::IStackedImageSource*>(device.get())) {
                stacks = source->stacked_image_support();
                if (!backend::valid_stack_support(stacks)) stacks = {};
            }
            if (auto* source = dynamic_cast<backend::ILineOfSightSource*>(device.get()))
                los = source->line_of_sight_support();
            if (los.frame != backend::LosFrame::sensor_frd_intrinsic_zyx ||
                los.time_domain != backend::LosTimeDomain::source_relative ||
                los.epoch != backend::LosEpoch::declared_source_origin ||
                los.source_id.empty() || los.provenance.empty() || los.epoch_meaning.empty()) {
                los.euler = false;
                los.report = false;
            }
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
                runtime_->los_support = std::move(los);
                runtime_->stack_support = std::move(stacks);
                runtime_->task_support = std::move(tasks);
                runtime_->configuration_valid = true;
                runtime_->initialized = true;
            }
        }
        if (capabilities_.empty()) {
            std::vector<ir::ChannelCapability> candidate;
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
            candidate.push_back(capability(*runtime_, ir::ChannelType::IRSTImage, image_id));
            GIGE_MEL_DISCOVERY_SEAM(after_first_entry);
            candidate.push_back(capability(*runtime_, ir::ChannelType::CommandAndControl, c2_id));
            candidate.push_back(capability(*runtime_, ir::ChannelType::HealthAndStatus, health_id));
            if (runtime_->task_support.virtual_stare) {
                auto uuid = health_uuid;
                uuid.back() = 5;
                candidate.push_back(capability(*runtime_, ir::ChannelType::Scheduling,
                    mel::UCI_ID(uuid, runtime_->settings.instance + "/scheduling")));
            }
            if (runtime_->stack_support.supported) {
                auto uuid = health_uuid;
                uuid.back() = 4;
                candidate.push_back(capability(*runtime_, ir::ChannelType::StackedImage,
                    mel::UCI_ID(uuid, runtime_->settings.instance + "/stacked-image")));
            }
            GIGE_MEL_DISCOVERY_SEAM(before_publication);
            capabilities_.swap(candidate);
        }
        return ir::Return::Success;
    } catch (const std::exception& error) {
        runtime_->report(error.what());
        return ir::Return::Fail;
    }
}

const std::vector<ir::ChannelCapability>& ControlImpl::getCapabilities() const { return capabilities_; }

mel::VersionInfo ControlImpl::getVersionInfo() const {
    // API version 4 / numeric library version 1 are unchanged local fields;
    // the text carries the provider software version.
    return mel::VersionInfo(4, 1, ProviderSoftware::name, std::string(ProviderSoftware::version_text) +
        " experimental Mono8 image-stream profile against pinned MEL 4.0; not full MEL conformance");
}

std::shared_ptr<ir::Channel> ControlImpl::attachChannel(const ir::Config& config) {
    if (in_provider_callback()) return {};
    std::lock_guard operation(runtime_->operations);
    if (!runtime_->initialized) return {};
    const auto type = config.getChannelType();
    if (type != ir::ChannelType::IRSTImage && type != ir::ChannelType::CommandAndControl &&
        type != ir::ChannelType::HealthAndStatus && type != ir::ChannelType::StackedImage &&
        type != ir::ChannelType::Scheduling) return {};
    const auto found = std::find_if(capabilities_.begin(), capabilities_.end(), [&](const auto& cap) {
        return cap.getChanID().getUUID() == config.getChanID().getUUID() && cap.getChannelTypes().front() == type &&
            ((type != ir::ChannelType::StackedImage && type != ir::ChannelType::Scheduling) || cap.getChanID() == config.getChanID());
    });
    if (found == capabilities_.end()) return {};
    if (std::any_of(channels_.begin(), channels_.end(), [type](const auto& channel) { return channel->type() == type; })) return {};
    if (type == ir::ChannelType::IRSTImage && !config.getImgLstnr()) return {};
    if (type == ir::ChannelType::StackedImage &&
        !std::dynamic_pointer_cast<ir::StackedImageListener>(config.getImgLstnr())) return {};
    std::shared_ptr<ChannelCore> channel;
    if (type == ir::ChannelType::IRSTImage) channel = make_image_channel(runtime_, config);
    else if (type == ir::ChannelType::StackedImage) channel = make_stacked_image_channel(runtime_, config);
    else if (type == ir::ChannelType::CommandAndControl) channel = make_c2_channel(runtime_, config);
    else if (type == ir::ChannelType::Scheduling) channel = make_scheduling_channel(runtime_, config);
    else channel = make_health_channel(runtime_, config);
    channels_.push_back(channel);
    if (type == ir::ChannelType::IRSTImage) runtime_->image_channel = channel;
    return channel;
}

ir::Return ControlImpl::detachChannel(std::shared_ptr<ir::Channel> channel) {
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

} // namespace provider_detail
} // namespace gige_mel
