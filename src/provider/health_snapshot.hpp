#pragma once

#include "runtime.hpp"
#include <mel/library/BIT_Configuration.h>
#include <mel/library/BIT_Status.h>
#include <irmel/library/health-status/SubsystemStatusResp.h>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/** @brief Value capture of provider ownership/readiness and optional software reports.
 * BIT and subsystem errors are carried separately from usable core state.
 * This is not a camera probe; active/failed describe logically owned acquisition.
 */
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

/// Build the fixed two-item software-state test catalogue (no hardware BIT).
mel::BIT_Configuration bit_catalogue();
/** @brief Capture current software state and optionally commit derived observations.
 * @param r Borrowed, pinned manager session.
 * @param execute Replace the latest synchronous BIT completion if BIT is prepared.
 * @param prepare_bit Observe fault creation/retirement and assemble canonical BIT;
 * false bypasses all derived work, including subsystem preparation.
 * @param prepare_subsystem Prepare the subsystem report paired with the BIT revision.
 * @return Core state plus requested payloads or their captured preparation exceptions.
 * @note Takes state_mutex internally (must not already be held), not operations;
 * no backend I/O or callbacks. Fault observation/completion uses host realtime
 * Unix-epoch nanoseconds, not camera time. Commit fault observations and completion
 * only after BIT formation; increment revision for a new completion or changed
 * observations. Subsystem preparation consumes a sequence only after formation
 * and never rolls back the BIT commit. Both counters wrap modulo 2^32.
 * Core capture can still throw (for example, copying diagnostic history).
 */
HealthSnapshot health_snapshot(Runtime& r, bool execute = false, bool prepare_bit = true,
                               bool prepare_subsystem = false);

} // namespace provider_detail
} // namespace gige_mel
