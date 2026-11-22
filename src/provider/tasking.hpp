#pragma once

#include "runtime.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

std::chrono::steady_clock::time_point task_deadline();
template<class F> auto task_call(F call) -> decltype(call()) {
    try { return call(); }
    catch (const std::exception& e) { return backend::Error{backend::ErrorCode::io_error, e.what()}; }
    catch (...) { return backend::Error{backend::ErrorCode::io_error, "task source threw"}; }
}

// Caller holds Runtime::operations.
void retire_tasking(Runtime& r) noexcept;

} // namespace provider_detail
} // namespace gige_mel
