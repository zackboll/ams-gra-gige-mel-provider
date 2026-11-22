#pragma once

#include "runtime.hpp"
#include <mel/library/BIT_Configuration.h>
#include <mel/library/BIT_Status.h>
#include <irmel/library/health-status/SubsystemStatusResp.h>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

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
    bool tasking{false};
};

mel::BIT_Configuration bit_catalogue();
HealthSnapshot health_snapshot(Runtime& r, bool execute = false, bool prepare_bit = true,
                               bool prepare_subsystem = false);

} // namespace provider_detail
} // namespace gige_mel
