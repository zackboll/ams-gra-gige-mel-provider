#pragma once

#include <gige_mel/backend.hpp>
#include <irmel/library/irmel-types/Control.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace gige_mel {

/// @brief Session settings for embedding a selected backend in the MEL provider.
/// Defaults select the SDK-free synthetic fixture; they do not configure a
/// physical camera. The backend supplies actual camera configuration at init.
/// @see make_api_manager
struct ProviderSettings {
    /// Nonempty, exact session name for Control and Buffer factory lookup and
    /// channel labels. Not a general SDK or camera selector.
    std::string instance{"synthetic"};
    /// Nonempty backend-specific device ID passed unchanged to open at init.
    std::string device_id{"synthetic-0"};
    /// Per-channel registration limit, inclusive range [1, 256]. Not the
    /// number of native acquisition buffers.
    std::size_t maximum_registered_buffers{16};
    /// Finite positive horizontal radians per pixel; multiplied by image width
    /// for MEL field of view. Default is virtual optics, not a measurement.
    double horizontal_radians_per_pixel{0.001};
    /// Finite positive vertical radians per pixel; multiplied by image height.
    /// Use independently characterized values for physical optics.
    double vertical_radians_per_pixel{0.001};
    /// Required clock label on accepted Image frames. Must not be unspecified;
    /// matching it does not convert clocks or qualify integration-start timing.
    backend::ClockDomain timestamp_domain{backend::ClockDomain::host_monotonic};
};

/// @brief Provider-side software observations for one manager session.
/// Counters start at zero and accumulate across disable/re-enable, detach, and
/// Control replacement in the same session. They are not native camera counters.
struct ProviderStatistics {
    /// Image frames and stack products published to a destination buffer, counted
    /// before invoking the listener (including deliveries whose listener throws).
    std::uint64_t delivered_frames{};
    /// Incoming frames/products dropped because no registered destination was
    /// available with sufficient capacity. Not native pool starvation or loss.
    std::uint64_t dropped_no_buffer{};
    /// Rejected Image frame descriptors/payloads and malformed Stack products.
    /// A malformed stack also faults its acquisition; rejected Image frames are skipped.
    std::uint64_t rejected_frames{};
    /// Caught consumer callback exceptions and non-success Stack listener returns.
    std::uint64_t callback_failures{};
    /// Recorded Image/Stack acquisition failures, including unexpected stop,
    /// terminal source errors, and worker failures. Excludes ordinary polling
    /// timeouts and expected cancellation; not a count of all failed API calls.
    std::uint64_t stream_failures{};
    /// Most recently recorded diagnostic (possibly empty), not an error queue
    /// or current fault state. Success does not clear it.
    std::string last_error;
};

/// @brief Embed a backend in a new, independent MEL manager session.
/// @param settings Session settings, moved into the session on success.
/// @param backend Non-null backend; the session holds shared ownership. Sharing
/// a backend between sessions does not bypass its device exclusivity rules.
/// @return A manager, or an empty pointer for a null backend, empty name or ID,
/// invalid buffer limit or angular pitch, or an unspecified timestamp domain.
/// Allocation and other unexpected C++ exceptions are not translated here.
/// @note Synchronous construction copies the backend ID but neither enumerates
/// nor opens a device. Control::init opens the exact device ID. Equal instance
/// names do not share sessions. Use the upstream getControl/getBuffer with this
/// manager and its exact instance; getAPI_Manager is the standard factory and
/// in this library accepts only `synthetic`.
/// @warning Control retains the session independently of the manager. Releasing
/// Control detaches its channels; retained channels can keep the runtime alive.
/// The device is logically closed at runtime destruction, not on manager reset
/// or channel detach alone. This is a source-level C++ interface, not a stable
/// cross-toolchain plugin ABI; upstream C-linkage factories still use C++ types.
/// Keep any dynamically loaded library alive until all its objects, callbacks,
/// futures and leases are retired.
/// @note Enable acquisition only after init, attachment, listener setup and
/// registration of sufficiently sized host destinations. Consumers own the
/// allocations and must preserve them while registered or held. Callbacks may
/// release delivered buffers but must defer lifecycle/configuration mutations
/// to outside any provider callback, including callbacks on another session.
/// @see @ref md_docs_2consumer-integration "Integrating a MEL Consumer"
std::shared_ptr<API_Manager> make_api_manager(
    ProviderSettings settings, std::shared_ptr<backend::ICameraBackend> backend);

/// @brief Read a synchronized, owned snapshot of project diagnostics.
/// @param manager A manager created by this provider (including its standard
/// factory); a null or foreign manager yields a default, empty snapshot.
/// @return A value with its own diagnostic string, valid independently of the
/// manager. Copying the diagnostic can allocate and throw.
/// @note Safe during acquisition and from consumer callbacks with a valid
/// manager handle. The diagnostic is copied under the state lock and counters
/// are loaded independently; the snapshot is not a single transactional event.
/// @warning Not a MEL InstrumentationReport, hardware-fault indication, or
/// proof of device health. A default snapshot does not identify the manager.
ProviderStatistics provider_statistics(const std::shared_ptr<API_Manager>& manager);

} // namespace gige_mel
