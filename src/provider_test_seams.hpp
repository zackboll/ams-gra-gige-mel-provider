#pragma once

// GIGE-MEL-042-R1 test-only synchronization seam. Never installed, never part of
// the provider library: src/provider.cpp compiles these points to nothing unless
// GIGE_MEL_PROVIDER_TEST_SEAMS is defined, which only the uninstalled
// gige_mel_provider_test_seams test build does.
#if !defined(GIGE_MEL_PROVIDER_TEST_SEAMS)
#error "provider_test_seams.hpp is only for the GIGE_MEL_PROVIDER_TEST_SEAMS test build"
#endif

namespace gige_mel::test_seams {

// Points inside one unsupported C2 submission (C2ChannelImpl::reject_unsupported).
enum class UnsupportedStatusPoint {
    // Callback context rejected and Control/channel pinned; Runtime::operations
    // not yet taken; nothing reserved.
    before_admission,
    // Admitted under Runtime::operations, registered CommandStatus callback (if
    // any) snapshotted and reserved; every provider lock released; the callback
    // not yet invoked.
    after_reservation,
};

// Must not throw: the after_reservation point holds a callback reservation.
using UnsupportedStatusHook = void (*)(UnsupportedStatusPoint, void* context) noexcept;

// Thread-local: only unsupported C2 submissions made by the calling thread run
// the hook. Pass nullptr to clear.
void set_unsupported_status_hook(UnsupportedStatusHook hook, void* context) noexcept;

enum class StackPoint { before_admission, after_reservation };
using StackHook = void (*)(StackPoint, void*) noexcept;
// Set before enable, clear after disable/detach. Invoked only on stack worker.
void set_stack_hook(StackHook hook, void* context) noexcept;

} // namespace gige_mel::test_seams
