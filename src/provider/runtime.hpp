#pragma once

#include "mel_types.hpp"
#include <gige_mel/task_execution.hpp>
#include <mel/library/CompletedBIT.h>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

class ChannelCore;
class ControlImpl;
class C2ChannelImpl;
class HostBuffer;
struct StreamState;

/** @brief Shared state of one provider manager session, not one Control attachment.
 *
 * The manager, Control, attached or retained channels, and active worker state
 * can all pin this object. A successfully initialized device stays open across
 * channel detach and Control replacement. Host buffers retain only the affinity
 * token, not this object or the device. Weak publications of Control, channels,
 * acquisition and registered buffers avoid ownership cycles.
 *
 * State | Synchronization
 * ----- | ---------------
 * Settings, backend identity, backend and affinity | Fixed before publication by the manager
 * Device, camera capabilities and optional LOS/Stack/task declarations | Published at initialization, then fixed
 * Attachment and acquisition ownership changes, memory reservations, LOS subscription IDs, task session and duplicate history | operations
 * Initialization, actual configuration/validity/generation, acquisition publication, tasking flag, diagnostics and retained BIT/subsystem state | state_mutex for snapshots and updates; lifecycle/configuration writers also serialize via operations
 * Control publication, counters | Atomic; counters are sampled individually, not as one atomic tuple
 *
 * The backend task session is uniquely owned here; C2 establishes it and
 * Scheduling uses it. Its presence excludes Image/Stack enable and camera
 * configuration. The published tasking flag is ownership, not physical activity.
 */
struct Runtime {
    ProviderSettings settings;
    std::shared_ptr<backend::ICameraBackend> backend;
    std::string backend_id; // immutable, acquired before any publisher exists
    std::shared_ptr<backend::ICameraDevice> device;
    std::shared_ptr<const int> affinity{std::make_shared<const int>(0)};
    /// Serializes provider mutations. Workers do not acquire it for delivery;
    /// consumer dispatch does not hold it, and lock-taking reentry is rejected.
    std::mutex operations;
    mutable std::mutex state_mutex;
    backend::CameraConfiguration configuration;
    backend::CameraCapabilities camera_capabilities;
    backend::LosSupport los_support; // immutable after init, before discovery
    backend::StackSupport stack_support;
    backend::TaskExecutionSupport task_support;
    std::unique_ptr<backend::ITaskExecutionSession> task_session; // operations
    std::array<std::uint32_t, 256> task_ids{}; // bounded session duplicate history
    std::size_t task_admissions{0};
    bool tasking_active{false}; // state_mutex; genuine session, not physical activity
    std::vector<std::weak_ptr<HostBuffer>> registered_memory; // operations
    std::uint64_t los_subscription{0}; // operations; never reused across attachments
    bool initialized{false};
    bool configuration_valid{false};
    bool image_active{false};
    std::weak_ptr<StreamState> acquisition; // guarded by state_mutex
    std::uint64_t configuration_generation{1};
    std::atomic<std::weak_ptr<ControlImpl>> control;
    std::weak_ptr<ChannelCore> image_channel; // guarded by operations
    std::weak_ptr<ChannelCore> acquisition_channel; // operations, Image OR Stack
    std::atomic<std::uint64_t> delivered{0}, dropped{0}, rejected{0}, callback_failures{0}, stream_failures{0};
    std::string last_error;
    // Manager lifetime; bounded latest completion and two category observations.
    std::shared_ptr<const mel::CompletedBIT> completed_bit;
    std::array<std::optional<std::chrono::nanoseconds>, 2> fault_observations;
    // Manager lifetime, guarded by state_mutex. bit_revision is committed with
    // completed_bit/fault_observations; subsystem_sequence is consumed only by a
    // successfully prepared SubsystemStatusResp. Zero is the initial state.
    std::uint32_t bit_revision{0};
    std::uint32_t subsystem_sequence{0};

    /// At last owner release, retire a residual task session and attempt device
    /// close. Neither the close result nor an exception from close is propagated.
    ~Runtime();

    /// Replace diagnostic history under state_mutex; this is not current Health
    /// state. Do not call with state_mutex held. Preparing the message may throw.
    void report(std::string message);
};

} // namespace provider_detail
} // namespace gige_mel
