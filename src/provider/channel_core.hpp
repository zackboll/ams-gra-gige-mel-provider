#pragma once

#include "runtime.hpp"
#include "results.hpp"
#include "callback_context.hpp"
#include <irmel/library/c2/C2Channel.h>
#include <condition_variable>
#include <thread>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

class ChannelCore : public virtual ir::Channel, public std::enable_shared_from_this<ChannelCore> {
public:
    ChannelCore(std::shared_ptr<Runtime> runtime, ir::Config config);
    ~ChannelCore() override;

    ir::ChannelType type() const;
    bool is_detached() const;

    mel::RequestFor<ir::Return> sendKeepAliveRep() override;
    mel::RequestFor<ir::ChannelCommsTestRep> send(ir::ChannelCommsTestReq request) override;

    ir::Return registerBuffer(std::shared_ptr<ir::Buffer> buffer) override;
    ir::Return unregisterBuffer(std::shared_ptr<ir::Buffer> buffer) override;
    ir::Return enable() override;
    ir::Return disable() override;
    ir::ChannelCapability getCapabilities() const override;
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::ChannelCommsTestRep* const)> callback) override;

    void detach_locked();

protected:
    template <class T> using Callback = std::function<void(ir::Channel&, const T*)>;

    template <class T> ir::Return register_callback(Callback<T>& slot, Callback<T> callback) {
        if (in_provider_callback() || is_detached()) return ir::Return::Fail;
        if (!callback) return ir::Return::BadPointer;
        std::lock_guard lock(callback_mutex_);
        if (is_detached()) return ir::Return::Fail;
        if (slot) return ir::Return::Fail;
        slot = std::move(callback);
        return ir::Return::Success;
    }
    // Used only when the caller holds operations and will unconditionally invoke
    // after a no-throw commit. Detach must drain even a not-yet-entered callback.
    template <class T> void admit_callback(const Callback<T>& callback) {
        if (callback) { std::lock_guard lock(callback_mutex_); ++active_metadata_callbacks_; }
    }
    template <class T> void invoke(const Callback<T>& callback, const T& message,
                                  const std::atomic<bool>* admission = nullptr, bool admitted = false) {
        if (!callback) return;
        if (!admitted) {
            std::lock_guard lock(callback_mutex_);
            if (is_detached() || (admission && !admission->load())) return;
            ++active_metadata_callbacks_;
        }
        // From here the reservation is owned by this scope: every exit, including
        // any exception, releases it and wakes a draining detach.
        struct Release {
            ChannelCore& channel;
            ~Release() {
                { std::lock_guard lock(channel.callback_mutex_); --channel.active_metadata_callbacks_; }
                channel.callback_cv_.notify_all();
            }
        } reservation{*this};
        CallbackGuard guard(runtime_.get());
        try { callback(*this, &message); }
        catch (...) {
            ++runtime_->callback_failures;
            try { runtime_->report(admission ? "Health metadata callback threw; periodic reporting continues" :
                "metadata callback threw; successful operation remains committed"); } catch (...) {}
        }
    }
    // Callback-free shared transaction. Caller holds operations, has admitted
    // this submission and pins Control/channel through result and dispatch.
    ConfigurationOutcome configure_locked(const backend::CameraConfiguration& requested, bool profile_valid = true);

    mel::RequestFor<ir::Return> config_set(const ir::ConfigSetCommand& command);

    mel::RequestFor<ir::CameraCommandResp> camera_command(const ir::CameraCommand& command);

    virtual void on_camera_reply(const ir::CameraCommandResp&);
    virtual void on_command_status(const ir::CommandStatus&);
    virtual bool start_auxiliary_locked();
    // Called by disable AND internal detach, including Control destruction.
    virtual void stop_auxiliary_locked();
    std::shared_ptr<Runtime> runtime_;
    ir::Config config_;
    std::mutex callback_mutex_;

private:
    // C2 Standby is the only command allowed to stop the attached acquisition.
    // The caller pins Control, C2 and Image/Stack, rejects callback reentry, validates
    // the request, and holds Runtime::operations (never state/callback mutexes).
    friend class C2ChannelImpl;
    void stop_locked();

    static void run_stacks(const std::shared_ptr<StreamState>& state,
                           const std::weak_ptr<ChannelCore>& weak) noexcept;

    static void run(const std::shared_ptr<StreamState>& state, const std::weak_ptr<ChannelCore>& weak) noexcept;

    std::atomic<bool> detached_{false};
    bool enabled_{false}; // guarded by Runtime::operations
    Callback<ir::ChannelCommsTestRep> comms_callback_;
    std::condition_variable callback_cv_;
    std::size_t active_metadata_callbacks_{0};
    std::vector<std::shared_ptr<HostBuffer>> buffers_;
    std::shared_ptr<StreamState> stream_state_;
    std::thread worker_;
};

} // namespace provider_detail
} // namespace gige_mel
