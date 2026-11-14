#include "session_selection.hpp"

namespace gige_mel::selection {
namespace b = backend;
namespace {

// Keep the profile/device binding attached to the backend through the existing
// provider embedding seam. No device/stream wrapper changes adapter semantics.
class BoundBackend final : public b::ICameraBackend {
public:
    BoundBackend(std::shared_ptr<b::ICameraBackend> backend, std::string device_id)
        : backend_(std::move(backend)), device_id_(std::move(device_id)) {}
    std::string_view id() const noexcept override { return backend_->id(); }
    b::Result<std::vector<b::DeviceInfo>> enumerate() override { return backend_->enumerate(); }
    b::Result<std::shared_ptr<b::ICameraDevice>> open(std::string_view device_id) override {
        if (device_id != device_id_)
            return b::Error{b::ErrorCode::invalid_argument, "device ID differs from the explicit session binding"};
        return backend_->open(device_id);
    }
private:
    std::shared_ptr<b::ICameraBackend> backend_;
    const std::string device_id_;
};

} // namespace

b::Result<SelectedSession> select_session(const BackendCatalog& catalog, SessionSelection request) {
    if (request.device_id.empty() || request.device_id.find('\0') != std::string::npos)
        return b::Error{b::ErrorCode::invalid_argument, "an exact, nonempty device ID is required"};
    auto selected = catalog.create(request.backend_id, request.profile_id);
    if (!selected) return selected.error();
    request.settings.device_id = request.device_id;
    auto bound = std::make_shared<BoundBackend>(std::move(selected).value(), request.device_id);
    return SelectedSession{std::move(request.backend_id), std::move(request.device_id),
                           std::move(request.profile_id), std::move(request.settings), std::move(bound)};
}

} // namespace gige_mel::selection
