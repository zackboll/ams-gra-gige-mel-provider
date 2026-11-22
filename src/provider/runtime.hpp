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

struct Runtime {
    ProviderSettings settings;
    std::shared_ptr<backend::ICameraBackend> backend;
    std::string backend_id; // immutable, acquired before any publisher exists
    std::shared_ptr<backend::ICameraDevice> device;
    std::shared_ptr<const int> affinity{std::make_shared<const int>(0)};
    // Mutations serialize here. A worker never takes this lock. Callbacks run
    // outside it; their reentrant mutations are rejected before acquiring it.
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

    ~Runtime();

    void report(std::string message);
};

} // namespace provider_detail
} // namespace gige_mel
