#include "runtime.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

Runtime::~Runtime() {
    if (task_session) task_session->retire();
    if (device) {
        try { (void)device->close(); } catch (...) { /* destructors must not throw */ }
    }
}

void Runtime::report(std::string message) {
    std::lock_guard lock(state_mutex);
    last_error = std::move(message);
}

} // namespace provider_detail
} // namespace gige_mel
