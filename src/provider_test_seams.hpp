#pragma once

// GIGE-MEL-042-R1 test-only synchronization seam. Never installed, never part of
// the provider library: provider components omit these points unless
// GIGE_MEL_PROVIDER_TEST_SEAMS is defined, which only the uninstalled
// gige_mel_provider_test_seams test build does.
#if !defined(GIGE_MEL_PROVIDER_TEST_SEAMS)
#error "provider_test_seams.hpp is only for the GIGE_MEL_PROVIDER_TEST_SEAMS test build"
#endif

namespace gige_mel::test_seams {

/// @brief Gates inside one unsupported C2 submission in the uninstalled test build.
enum class UnsupportedStatusPoint {
    // Callback context rejected and Control/channel pinned; Runtime::operations
    // not yet taken; nothing reserved.
    before_admission,
    // Admitted under Runtime::operations, registered CommandStatus callback (if
    // any) snapshotted and reserved; every provider lock released; the callback
    // not yet invoked.
    after_reservation,
};

/// Must not throw: the after_reservation point holds a callback reservation.
using UnsupportedStatusHook = void (*)(UnsupportedStatusPoint, void* context) noexcept;

/// Only unsupported submissions on the calling thread use this hook. Context
/// is borrowed, not owned. Pass nullptr to clear.
void set_unsupported_status_hook(UnsupportedStatusHook hook, void* context) noexcept;

/// Gates on either side of Stack callback admission, with no provider mutex held.
enum class StackPoint { before_admission, after_reservation };
using StackHook = void (*)(StackPoint, void*) noexcept;
/// Global pair, invoked only by the Stack worker. Set before enable and clear
/// after disable/detach: hook and borrowed context are not published as a unit.
void set_stack_hook(StackHook hook, void* context) noexcept;

/// Runs on the submitting thread, after task ack reservation and before delivery.
using TaskHook = void (*)(void*) noexcept;
/// Set the per-thread task hook and borrowed context, or clear with a null hook.
void set_task_hook(TaskHook hook, void* context) noexcept;

} // namespace gige_mel::test_seams
