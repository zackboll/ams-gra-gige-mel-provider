#pragma once

#include "runtime.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

struct CallbackGuard;
/// Top of the shared per-thread ancestry (initially null). The declaration and
/// sole definition in callback_context.cpp both expose constant initialization
/// so consumers of the hidden TLS symbol do not need a dynamic-init thunk.
extern constinit thread_local const CallbackGuard* callback_context;
/// Shared per-thread sink reentry flag, initially false; same single-definition
/// and constinit contract as callback_context.
extern constinit thread_local bool in_navigation_submission;

/** @brief Push a non-owning runtime identity on the calling thread's callback stack.
 * Nested dispatch preserves the entire ancestry, including other managers.
 * Frames are stack objects, not runtime owners. Dispatch paths retain their
 * channel/Runtime (or captured worker state), and pin Control as applicable,
 * so these raw identities remain valid even if consumer code drops handles.
 */
struct CallbackGuard {
    const Runtime* const runtime;
    const CallbackGuard* const previous;
    /// Push @p value without allocation. The caller pins that Runtime through destruction.
    explicit CallbackGuard(const Runtime* value) noexcept;
    /// Restore the previous frame on the same thread, in LIFO order.
    ~CallbackGuard();
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

/** @brief Mark an optional backend navigation call made with operations held.
 * This is a reentry flag, not a callback frame or a lifetime pin. It is not
 * nestable: submission rejects callback/sink reentry before constructing it.
 */
struct NavigationSubmissionGuard {
    /// Set the flag on the calling thread; it must previously be clear.
    NavigationSubmissionGuard() noexcept;
    /// Clear the flag on normal return or unwinding.
    ~NavigationSubmissionGuard();
};

/// Whether this thread is in any provider callback or guarded navigation submission.
bool in_provider_callback() noexcept;
/// Search all callback ancestors for @p runtime; used by defensive teardown
/// rather than treating callbacks on unrelated managers as a reason to skip draining.
bool in_runtime_callback(const Runtime* runtime) noexcept;

} // namespace provider_detail
} // namespace gige_mel
