// Test-only application composition. Not installed or a provider factory.
#include <gige_mel/provider.hpp>
#include "../support/recording_stack_backend.hpp"
namespace {
auto recording = std::make_shared<recording_stack::Recording>();
}
std::shared_ptr<API_Manager> recording_manager() {
    return gige_mel::make_api_manager({}, std::make_shared<recording_stack::Backend>(recording));
}
void deliver_recording() {
    recording->latest->deliver();
}