#pragma once

#include <gige_mel/backend.hpp>

namespace gige_mel::backend {

// Optional matched-C++ source interface, not a plugin ABI. No MEL/SDK types.
enum class TaskCoordinates { unspecified, virtual_aircraft_origin };
enum class TaskClock { unspecified, session_relative_virtual };
enum class TaskPhase { idle, admitted, executing, completed, cancelled, failed };

struct TaskExecutionSupport {
    bool virtual_stare{false};
    TaskCoordinates coordinates{TaskCoordinates::unspecified};
    TaskClock clock{TaskClock::unspecified};
    double azimuth{0}, elevation{0}; // exact qualified virtual radians, no transform
    std::chrono::nanoseconds minimum_duration{0}, maximum_duration{0};
    std::uint32_t active_capacity{0}, queue_capacity{0}, session_admissions{0};
    bool cancel_current{false}, atomic_immediate_replacement{false};
    std::string source_id, provenance, origin_meaning;
    std::uint64_t generation{0};
    bool operator==(const TaskExecutionSupport&) const = default;
};

inline bool valid_task_support(const TaskExecutionSupport& s) {
    return s.virtual_stare && s.coordinates == TaskCoordinates::virtual_aircraft_origin &&
        s.clock == TaskClock::session_relative_virtual && s.azimuth == 0 && s.elevation == 0 &&
        s.minimum_duration.count() > 0 && s.maximum_duration >= s.minimum_duration &&
        s.maximum_duration <= std::chrono::seconds{60} && s.active_capacity == 1 && s.queue_capacity == 0 &&
        s.session_admissions > 0 && s.session_admissions <= 256 && s.cancel_current &&
        s.atomic_immediate_replacement && !s.source_id.empty() && !s.provenance.empty() &&
        !s.origin_meaning.empty() && s.generation != 0;
}

struct VirtualStareRequest {
    std::uint32_t command_id{0}, request_id{0};
    std::chrono::nanoseconds system_time{0}, duration{0};
    double azimuth{0}, elevation{0};
};
struct TaskObservation {
    TaskPhase phase{TaskPhase::idle};
    std::optional<VirtualStareRequest> task;
    std::chrono::nanoseconds virtual_time{0};
};

class ITaskExecutionSession {
public:
    virtual ~ITaskExecutionSession() = default;
    // Every call completes by deadline. No callbacks into the provider. Failed
    // admission MUST leave the old task unchanged. Success owns the new request,
    // cancelling the old active task atomically; no queue exists.
    virtual Status admit(VirtualStareRequest request, std::chrono::steady_clock::time_point deadline) = 0;
    virtual Result<TaskObservation> observe(std::chrono::steady_clock::time_point deadline) = 0;
    // Success establishes cancellation (not merely a cancellation request).
    // Failure retains ownership and permits retry. No active task is an error.
    virtual Status cancel_current(std::chrono::steady_clock::time_point deadline) = 0;
    // Mandatory bounded retirement even after cancellation/source failure.
    // On return no task/resources remain owned; failure is a terminal failed
    // outcome, never a claim of successful task cancellation. Destructor too.
    // retire/destruction must complete within 100 ms, including failure paths.
    virtual void retire() noexcept = 0;
};

class ITaskExecutionSource {
public:
    virtual ~ITaskExecutionSource() = default;
    virtual TaskExecutionSupport task_execution_support() const = 0; // immutable
    // Opens an idle, genuine task-admission session; origin is virtual time zero.
    virtual Result<std::unique_ptr<ITaskExecutionSession>> open_task_session(
        std::chrono::steady_clock::time_point deadline) = 0;
};

} // namespace gige_mel::backend