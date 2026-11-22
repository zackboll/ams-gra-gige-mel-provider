#include "results.hpp"
#include <cmath>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

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

} // namespace provider_detail
} // namespace gige_mel
