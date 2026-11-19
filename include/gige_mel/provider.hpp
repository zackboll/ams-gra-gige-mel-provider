#pragma once

#include <gige_mel/backend.hpp>
#include <irmel/library/irmel-types/Control.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace gige_mel {

/// Source-level embedding settings for an SDK adapter, not a stable plugin ABI.
/// Ordinary MEL consumers use the three upstream factories.
struct ProviderSettings {
    std::string instance{"synthetic"};
    std::string device_id{"synthetic-0"};
    std::size_t maximum_registered_buffers{16};
    // Synthetic virtual optics. Hardware integrations MUST replace these with
    // a characterized camera profile; these are not real camera calibration.
    double horizontal_radians_per_pixel{0.001};
    double vertical_radians_per_pixel{0.001};
    backend::ClockDomain timestamp_domain{backend::ClockDomain::host_monotonic};
};

/// Project diagnostics for one manager session.
struct ProviderStatistics {
    std::uint64_t delivered_frames{};
    std::uint64_t dropped_no_buffer{};
    std::uint64_t rejected_frames{};
    std::uint64_t callback_failures{};
    std::uint64_t stream_failures{};
    std::string last_error;
};

/// Creates an independent manager session without device I/O.
/// Control::init opens the selected device. Managers with the same instance
/// name still have independent sessions.
std::shared_ptr<API_Manager> make_api_manager(
    ProviderSettings settings, std::shared_ptr<backend::ICameraBackend> backend);

/// Reads project diagnostics, not a MEL InstrumentationReport extension.
ProviderStatistics provider_statistics(const std::shared_ptr<API_Manager>& manager);

} // namespace gige_mel
