#pragma once

#if defined(GIGE_MEL_PROVIDER_TEST_SEAMS)
#include "../provider_test_seams.hpp"
#include <atomic>
#endif

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

// GIGE-MEL-042-R1 synchronization points for the uninstalled test build only.
// The installed provider compiles them to nothing.
#if defined(GIGE_MEL_PROVIDER_TEST_SEAMS)
/// Per-thread C2 seam; constinit at both declaration and sole definition keeps
/// consuming components from needing a hidden TLS dynamic-init thunk.
extern constinit thread_local test_seams::UnsupportedStatusHook unsupported_status_hook;
/// Borrowed context paired with the C2 hook on this thread.
extern constinit thread_local void* unsupported_status_hook_context;
/// Per-thread task seam with the same constant-initialization contract.
extern constinit thread_local test_seams::TaskHook task_hook;
/// Borrowed context paired with the task hook on this thread.
extern constinit thread_local void* task_hook_context;
/// Pause test submissions between reservation and ack dispatch; production no-op.
#define GIGE_MEL_TASK_SEAM() do { if (task_hook) task_hook(task_hook_context); } while (false)
/// Stack worker hook, set/cleared outside the worker lifetime (see set_stack_hook).
extern std::atomic<test_seams::StackHook> stack_hook;
/// Separately atomic borrowed context; do not replace while a worker can invoke it.
extern std::atomic<void*> stack_hook_context;
/// Pause the test Stack worker on either side of admission; production no-op.
#define GIGE_MEL_STACK_SEAM(point) \
    do { if (auto hook = stack_hook.load()) hook(test_seams::StackPoint::point, stack_hook_context.load()); } while (false)
/// Pause a test unsupported submission on either side of admission; production no-op.
#define GIGE_MEL_UNSUPPORTED_STATUS_SEAM(point) \
    do { \
        if (unsupported_status_hook) \
            unsupported_status_hook(test_seams::UnsupportedStatusPoint::point, unsupported_status_hook_context); \
    } while (false)
#else
#define GIGE_MEL_TASK_SEAM() do {} while (false)
#define GIGE_MEL_UNSUPPORTED_STATUS_SEAM(point) do {} while (false)
#define GIGE_MEL_STACK_SEAM(point) do {} while (false)
#endif

} // namespace provider_detail
} // namespace gige_mel
