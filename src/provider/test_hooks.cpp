#include "test_hooks.hpp"

#if defined(GIGE_MEL_PROVIDER_TEST_SEAMS)
namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

thread_local test_seams::UnsupportedStatusHook unsupported_status_hook = nullptr;
thread_local void* unsupported_status_hook_context = nullptr;
thread_local test_seams::TaskHook task_hook = nullptr;
thread_local void* task_hook_context = nullptr;
std::atomic<test_seams::StackHook> stack_hook{nullptr};
std::atomic<void*> stack_hook_context{nullptr};

} // namespace provider_detail
} // namespace gige_mel

namespace gige_mel::test_seams {
using namespace provider_detail;
void set_unsupported_status_hook(UnsupportedStatusHook hook, void* context) noexcept {
    unsupported_status_hook = hook;
    unsupported_status_hook_context = context;
}
void set_stack_hook(StackHook hook, void* context) noexcept {
    stack_hook_context = context;
    stack_hook = hook;
}
void set_task_hook(TaskHook hook, void* context) noexcept {
    task_hook = hook;
    task_hook_context = context;
}
} // namespace test_seams
#endif
