#pragma once

#include "runtime.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

// One shared callback ancestry stack and submission guard per thread, across
// all provider components. Definitions live only in callback_context.cpp.
// Keep constant initialization visible to every consuming translation unit.
struct CallbackGuard;
extern constinit thread_local const CallbackGuard* callback_context;
extern constinit thread_local bool in_navigation_submission;

struct CallbackGuard {
    const Runtime* const runtime;
    const CallbackGuard* const previous;
    explicit CallbackGuard(const Runtime* value) noexcept;
    ~CallbackGuard();
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

struct NavigationSubmissionGuard {
    NavigationSubmissionGuard() noexcept;
    ~NavigationSubmissionGuard();
};

bool in_provider_callback() noexcept;
bool in_runtime_callback(const Runtime* runtime) noexcept;

} // namespace provider_detail
} // namespace gige_mel
