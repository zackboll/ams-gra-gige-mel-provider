#include "tasking.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

constexpr auto task_call_bound = std::chrono::milliseconds{100};
std::chrono::steady_clock::time_point task_deadline() { return std::chrono::steady_clock::now() + task_call_bound; }

// operations held. Mandatory retirement never waits for consumer callbacks.
void retire_tasking(Runtime& r) noexcept {
    if (r.task_session) r.task_session->retire();
    r.task_session.reset();
    r.task_admissions = 0;
    std::lock_guard lock(r.state_mutex);
    r.tasking_active = false;
}

} // namespace provider_detail
} // namespace gige_mel
