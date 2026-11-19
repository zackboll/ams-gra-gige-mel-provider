// Test-only embedding composition, never installed or selected by factories.
#include <gige_mel/provider.hpp>
#include "../support/recording_task_backend.hpp"
namespace {
auto recording = std::make_shared<recording_task::Recording>();
}
std::shared_ptr<API_Manager> task_recording_manager() {
    return gige_mel::make_api_manager({}, std::make_shared<recording_task::Backend>(recording));
}
bool task_recording_progress() {
    using namespace std::chrono_literals;
    recording->executing();
    const bool executing = recording->snapshot().phase == gige_mel::backend::TaskPhase::executing;
    recording->advance(5ns);
    return executing && recording->snapshot().phase == gige_mel::backend::TaskPhase::completed;
}
bool task_recording_cancelled() {
    return recording->snapshot().phase == gige_mel::backend::TaskPhase::cancelled;
}