#pragma once

#include "runtime.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/// Give one source call a cooperative deadline 100 ms from steady-clock now.
/// No watchdog, thread interruption or hard wall-clock bound is enforced here.
std::chrono::steady_clock::time_point task_deadline();
/// Call the source synchronously and translate exceptions to backend I/O errors.
/// Copies/allocations preparing the callable or translated error may still throw.
template<class F> auto task_call(F call) -> decltype(call()) {
    try { return call(); }
    catch (const std::exception& e) { return backend::Error{backend::ErrorCode::io_error, e.what()}; }
    catch (...) { return backend::Error{backend::ErrorCode::io_error, "task source threw"}; }
}

/// Force retirement and destroy the unique session, reset duplicate history
/// and unpublish tasking under state_mutex. Caller holds operations, not
/// state_mutex. No consumer callback drain here; relies on cooperative source
/// retirement even after cancellation/observation failure.
void retire_tasking(Runtime& r) noexcept;

} // namespace provider_detail
} // namespace gige_mel
