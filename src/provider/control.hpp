#pragma once

#include "runtime.hpp"

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/** @brief Owns discovery and the attached channel set for one Control lifetime.
 * The Runtime can outlive this wrapper. Initialize before discovery or
 * attachment; do not race discovery reads with initialization.
 */
class ControlImpl final : public ir::Control {
public:
    /// Retain the manager session without opening the device.
    explicit ControlImpl(std::shared_ptr<Runtime> runtime);
    /// Under the operation lock, detach owned channels and retire tasking.
    /// Retained detached handles and delivered buffers are not revoked.
    ~ControlImpl() override;

    /** @brief Open and validate the device once per Runtime, then populate discovery for this Control.
     * @param aperture_config_id Empty or `mono8-bootstrap`; any other ID is unsupported.
     * @return Success, NotSupported for another ID, or Fail for callback
     * reentry, failed bootstrap or a caught standard exception.
     * @note Acquires operations. Initial readback must fit the supported layout;
     * optional metadata and channel declarations are qualified before discovery.
     * Failure after publication need not undo the device or discovery work already
     * done. Nonstandard exceptions and failures while reporting can escape.
     */
    ir::Return init(const std::string& aperture_config_id) override;
    /// Borrow this Control's discovery vector for its lifetime after successful
    /// init. It is not updated by configuration commands; use
    /// ChannelCore::getCapabilities for a current by-value snapshot.
    const std::vector<ir::ChannelCapability>& getCapabilities() const override;
    /// Return the fixed API/library fields and experimental provider software identity.
    mel::VersionInfo getVersionInfo() const override;
    /** @brief Create and retain at most one attachment of each advertised type.
     * @param config Copied into the channel, retaining its listener. Match the
     * discovered type and UUID (full identity for Stack and Scheduling). Image
     * needs an Image listener; Stack needs a StackedImageListener.
     * @return A shared, initially disabled channel, or empty on callback reentry,
     * missing discovery or an invalid/duplicate request. Allocation may throw.
     * @note Serializes via operations; neither starts acquisition nor calls a listener.
     * Image and Stack may coexist as attachments but not as active acquisitions.
     */
    std::shared_ptr<ir::Channel> attachChannel(const ir::Config& config) override;
    /** @brief Permanently detach an owned channel and remove it from this Control.
     * @param channel Shared handle to the exact attached object; not consumed
     * from the caller. Other references may survive as detached handles.
     * @return Success after stop and drain, BadPointer for a nonmember, or Fail
     * on callback reentry or if Control cannot be pinned.
     * @note Takes operations; may wait for consumer code. Never releases delivered bytes.
     */
    ir::Return detachChannel(std::shared_ptr<ir::Channel> channel) override;

private:
    std::shared_ptr<Runtime> runtime_;
    std::vector<ir::ChannelCapability> capabilities_;
    std::vector<std::shared_ptr<ChannelCore>> channels_;
};

} // namespace provider_detail
} // namespace gige_mel
