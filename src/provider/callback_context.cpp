#include "callback_context.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

// Stack frames require no allocation between admission and consumer entry.
constinit thread_local const CallbackGuard* callback_context = nullptr;
// Sink execution holds operations; same-thread reentry must be rejected.
constinit thread_local bool in_navigation_submission = false;

CallbackGuard::CallbackGuard(const Runtime* value) noexcept
    : runtime(value), previous(callback_context) { callback_context = this; }

CallbackGuard::~CallbackGuard() { callback_context = previous; }

NavigationSubmissionGuard::NavigationSubmissionGuard() noexcept { in_navigation_submission = true; }

NavigationSubmissionGuard::~NavigationSubmissionGuard() { in_navigation_submission = false; }

bool in_provider_callback() noexcept { return callback_context != nullptr || in_navigation_submission; }

bool in_runtime_callback(const Runtime* runtime) noexcept {
    for (auto frame = callback_context; frame; frame = frame->previous) {
        if (frame->runtime == runtime) return true;
    }
    return false;
}

} // namespace provider_detail
} // namespace gige_mel
