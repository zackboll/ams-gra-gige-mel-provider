#include "capabilities.hpp"
#include <limits>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

bool valid_layout(const backend::CameraConfiguration& c, const backend::CameraCapabilities& caps) {
    const auto layout = backend::image_layout(caps.pixel_format, c.width, c.height);
    return layout && c.width <= caps.sensor_width && c.height <= caps.sensor_height &&
        c.offset_x <= caps.sensor_width - c.width && c.offset_y <= caps.sensor_height - c.height &&
        layout->image_bytes <= std::numeric_limits<std::uint32_t>::max();
}

ir::ChannelCapability capability(const Runtime& runtime, ir::ChannelType type,
                                 const mel::UCI_ID& id, const ir::Config* config) {
    std::lock_guard lock(runtime.state_mutex);
    const auto& c = runtime.configuration;
    ir::ChannelCapability cap;
    cap.setChanID(id);
    if (type == ir::ChannelType::StackedImage) {
        const auto& s = runtime.stack_support;
        cap.setWidth(s.width);
        cap.setHeight(s.height);
        cap.setBitDepth(s.logical_bits);
        cap.setRowPitch(s.width);
        cap.setBufferSize(static_cast<std::uint32_t>(s.packed_bytes));
        cap.setImageSize(static_cast<std::uint32_t>(s.packed_bytes));
        cap.setNumberOfBands(1);
        cap.setFormat(ir::PixelFormat::Mono);
    } else if (type != ir::ChannelType::HealthAndStatus && type != ir::ChannelType::Scheduling) {
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
        if (runtime.los_support.euler) metadata.insert(ir::ChannelMetadataCapabilityType::LineOfSightEuler);
        if (runtime.los_support.report) metadata.insert(ir::ChannelMetadataCapabilityType::LineOfSightReport);
        if (runtime.camera_capabilities.known_empty_defect_map)
            metadata.insert(ir::ChannelMetadataCapabilityType::BadPixelList);
    }
    if (type == ir::ChannelType::CommandAndControl) {
        metadata.insert(ir::ChannelMetadataCapabilityType::CommandStatus);
        metadata.insert(ir::ChannelMetadataCapabilityType::BITConfiguration);
        metadata.insert(ir::ChannelMetadataCapabilityType::BITStatus);
    }
    if (type == ir::ChannelType::Scheduling) {
        cap.setTaskScheduleDepth(0); // actual immediate replacement, no queue
        metadata.insert(ir::ChannelMetadataCapabilityType::ExecuteTaskAck);
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

} // namespace provider_detail
} // namespace gige_mel
