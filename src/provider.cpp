#include <gige_mel/provider.hpp>
#include "provider_internal.hpp"
#include "provider/runtime.hpp"
#include "provider/control.hpp"
#include "provider/host_buffer.hpp"
#include "provider/callback_context.hpp"

#include <cmath>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

class Manager final : public API_Manager {
public:
    explicit Manager(std::shared_ptr<Runtime> value) : runtime(std::move(value)) {}
    std::shared_ptr<Runtime> runtime;
};

} // namespace provider_detail
} // namespace gige_mel

namespace gige_mel {
using namespace provider_detail;

std::shared_ptr<API_Manager> make_api_manager(ProviderSettings settings, std::shared_ptr<backend::ICameraBackend> backend) {
    if (!backend || settings.instance.empty() || settings.device_id.empty() || settings.maximum_registered_buffers == 0 ||
        settings.maximum_registered_buffers > 256 || !std::isfinite(settings.horizontal_radians_per_pixel) ||
        !std::isfinite(settings.vertical_radians_per_pixel) || settings.horizontal_radians_per_pixel <= 0 ||
        settings.vertical_radians_per_pixel <= 0 || settings.timestamp_domain == backend::ClockDomain::unspecified) return {};
    auto runtime = std::make_shared<Runtime>();
    runtime->settings = std::move(settings);
    runtime->backend_id = backend->id();
    runtime->backend = std::move(backend);
    return std::make_shared<Manager>(std::move(runtime));
}

ProviderStatistics provider_statistics(const std::shared_ptr<API_Manager>& manager) {
    const auto concrete = std::dynamic_pointer_cast<Manager>(manager);
    if (!concrete) return {};
    const auto& r = *concrete->runtime;
    std::lock_guard lock(r.state_mutex);
    return {r.delivered.load(), r.dropped.load(), r.rejected.load(), r.callback_failures.load(), r.stream_failures.load(), r.last_error};
}

std::shared_ptr<ir::Control> control_for(std::string_view instance, const std::shared_ptr<API_Manager>& manager) {
    const auto concrete = std::dynamic_pointer_cast<Manager>(manager);
    if (!concrete || concrete->runtime->settings.instance != instance) return {};
    const auto& runtime = concrete->runtime;
    if (in_provider_callback()) return {};
    std::lock_guard operation(runtime->operations);
    if (auto control = runtime->control.load().lock()) return control;
    auto control = std::make_shared<ControlImpl>(runtime);
    runtime->control.store(control);
    return control;
}
std::shared_ptr<ir::Buffer> buffer_for(std::string_view instance, const std::shared_ptr<API_Manager>& manager) {
    const auto concrete = std::dynamic_pointer_cast<Manager>(manager);
    if (!concrete || concrete->runtime->settings.instance != instance) return {};
    return std::make_shared<HostBuffer>(concrete->runtime->affinity);
}

} // namespace gige_mel
