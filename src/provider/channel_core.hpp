#pragma once

#include "runtime.hpp"
#include "results.hpp"
#include "callback_context.hpp"
#include <irmel/library/c2/C2Channel.h>
#include <condition_variable>
#include <thread>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/** @brief Private common attachment machinery for the concrete MEL channels.
 * Owns registration, acquisition and callback bookkeeping, not camera policy.
 * This class and its hooks are implementation details, not a supported subclass API.
 *
 * Runtime::operations serializes lifecycle and command mutations. The callback
 * mutex protects slots, admission and reservation counts; consumer code runs
 * without either mutex or the runtime state mutex held by the dispatch path.
 * Public operations that take the operation lock reject provider callback/sink
 * reentry across managers. Dispatchers pin objects through consumer entry and
 * return. Independent submissions/workers can dispatch concurrently; consumers
 * must synchronize shared callback state and not wait for work that drains them.
 *
 * Unless a path catches them explicitly, allocation, callback copies, backend
 * exceptions and result construction can throw instead of yielding a MEL result.
 */
class ChannelCore : public virtual ir::Channel, public std::enable_shared_from_this<ChannelCore> {
public:
    /// Retain the Runtime and attachment configuration (including its listener).
    ChannelCore(std::shared_ptr<Runtime> runtime, ir::Config config);
    /// Defensive acquisition stop/unbind; derived cleanup is not dispatched here.
    ~ChannelCore() override;

    /// Type fixed by the attachment configuration.
    ir::ChannelType type() const;
    /// Read the atomic, irreversible detach marker.
    bool is_detached() const;

    /// Ready Success while attached, InvalidState error after detach; no I/O or callback.
    mel::RequestFor<ir::Return> sendKeepAliveRep() override;
    /** @brief Echo the communications request's command and request IDs.
     * Pins Control and this channel; returns a ready reply, optionally also
     * dispatching it inline, subject to metadata admission. No device I/O.
     * Returns InvalidState if detached, Control cannot be pinned or the thread
     * is inside backend navigation submission. Ordinary callbacks may call this
     * overload; avoid unbounded recursive communications dispatch.
     */
    mel::RequestFor<ir::ChannelCommsTestRep> send(ir::ChannelCommsTestReq request) override;

    /** @brief Retain a destination in the stopped Image/Stack buffer pool.
     * @param buffer Initialized same-session HostBuffer with sufficient byte
     * capacity, exactly the Host flag, and no owner or outstanding delivery.
     * @note Under operations, enforce the pool limit and reject overlapping
     * reserved ranges, even when the other buffer is no longer registered.
     * @return Success after binding; Fail on lifecycle/context, capacity of the
     * pool or overlap/in-use rejection, BadPointer for identity/size errors,
     * NotSupported for unsupported flags or non-acquisition registration.
     * Allocation failure during insertion unbinds the candidate and propagates.
     */
    ir::Return registerBuffer(std::shared_ptr<ir::Buffer> buffer) override;
    /// Under operations, remove a registered buffer only when disabled and
    /// available. Fail for reentry/closing/detached/enabled or outstanding use;
    /// BadPointer for a nonmember, NotSupported for Health. Does not release data.
    ir::Return unregisterBuffer(std::shared_ptr<ir::Buffer> buffer) override;
    /** @brief Start the attachment, aligning worker state with the current configuration.
     * Image/Stack require an exclusive idle acquisition slot, no task session,
     * valid configuration, a listener and a nonempty pool of available buffers
     * sized for the current payload. Starts the matching source and auxiliary
     * work; worker callbacks may start before return, not inline on this thread.
     * Repeated enable succeeds unless the owned stream has faulted. Disable is
     * needed to clean up a faulted stream. The non-image base path just marks enabled.
     * Acquires operations and returns Fail for reentry/closing/detached or
     * rejected startup; allocation before startup and uncaught exceptions can escape.
     */
    ir::Return enable() override;
    /** @brief Stop acquisition and applicable auxiliary work under operations.
     * Rejects callback/sink context, missing Control or detached handles with
     * Fail; otherwise returns Success after stop. Keeps attachment, callback
     * slots, registration and outstanding bytes. Joins workers in the normal
     * external path, but does not close metadata admission or drain every
     * synchronous metadata callback; that is detach's responsibility.
     */
    ir::Return disable() override;
    /// Build a by-value capability from current retained configuration, not the
    /// discovery vector or a fresh device query. Readable even after detach.
    ir::ChannelCapability getCapabilities() const override;
    /// Install the communications reply slot once; no initial publication.
    /// @see register_callback
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const ir::ChannelCommsTestRep* const)> callback) override;

    /** @brief Close admission, stop workers, drain reservations and unbind the pool.
     * @pre Caller holds operations, not the state or callback mutex.
     * A reserved callback counts even before consumer entry: external detach
     * must not return before it finishes. Defensive teardown from the same
     * runtime's callback ancestry skips its own metadata drain to avoid deadlock.
     * Unbinding removes channel ownership, not the consumer's delivery obligation.
     */
    void detach_locked();

protected:
    /// Consumer callable with a borrowed channel and payload valid during invocation.
    template <class T> using Callback = std::function<void(ir::Channel&, const T*)>;

    /// Move a nonempty callable into an empty slot under callback_mutex_. No
    /// dispatch; BadPointer for an empty callable on the usable path, Fail for
    /// callback/sink reentry, detach or duplicate registration.
    template <class T> ir::Return register_callback(Callback<T>& slot, Callback<T> callback) {
        if (in_provider_callback() || is_detached()) return ir::Return::Fail;
        if (!callback) return ir::Return::BadPointer;
        std::lock_guard lock(callback_mutex_);
        if (is_detached()) return ir::Return::Fail;
        if (slot) return ir::Return::Fail;
        slot = std::move(callback);
        return ir::Return::Success;
    }
    /** @brief Reserve a nonempty callback before releasing operations.
     * @pre Caller holds operations and has checked attachment. Arrange no
     * throwing or early exit after reservation before invoke(..., true) takes
     * ownership. An empty slot needs no reservation. Detach waits even if the
     * submitting thread has not yet reached consumer code.
     */
    template <class T> void admit_callback(const Callback<T>& callback) {
        if (callback) { std::lock_guard lock(callback_mutex_); ++active_metadata_callbacks_; }
    }
    /** @brief Dispatch inline with a thread-local guard and scoped reservation.
     * @param callback Borrowed stable callable (usually copied under callback_mutex_).
     * @param message Borrowed payload; consumers must copy values needed later.
     * @param admission Optional borrowed worker running flag checked at admission.
     * @param admitted True to consume an existing reservation for a nonempty
     * callback, even if detach has since begun; otherwise attempt admission now.
     * @pre Pin channel/Runtime through return and hold no provider lock at dispatch.
     * Empty, detached or stopped nonreserved dispatches are suppressed as
     * applicable. A scope guard releases each owned reservation on every exit
     * and wakes detach. Consumer exceptions are counted/reported, not propagated.
     */
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
    /** @brief Shared callback-free configuration transaction, not a rollback boundary.
     * @pre Caller holds operations, has admitted the request and pins Control/channel.
     * @param requested Complete ROI in pixels, exposure in nanoseconds and rate in Hz.
     * @param profile_valid Whether caller-specific command fields satisfy the profile.
     * Rejects active tasking/acquisition or invalid profile/layout before device I/O.
     * After apply returns (including an error), read back actual configuration.
     * With a returned readback result, advance generation; retain actual values
     * only if readback succeeds and the layout is valid, otherwise mark invalid.
     * Valid readback can be retained after partial apply failure. Success of the
     * command additionally requires successful apply and exact readback equality.
     * Readback error takes precedence. A thrown backend exception can interrupt
     * this sequence; neither rejection nor exception guarantees unchanged hardware.
     */
    ConfigurationOutcome configure_locked(const backend::CameraConfiguration& requested, bool profile_valid = true);

    /// Parse a complete schema-v1 snapshot, serialize configuration, then prepare
    /// a ready Return or error future and attempt inline C2 status dispatch.
    /// Reentry, closing and detached rejection send no status. Detach may suppress
    /// a status not yet admitted; the future is authoritative.
    mel::RequestFor<ir::Return> config_set(const ir::ConfigSetCommand& command);

    /// Apply a complete staring command (zero stages), not a partial update.
    /// Uses whole-Hz rate and nanosecond exposure. Input system time does not
    /// schedule execution. The typed reply echoes command ID, leaves size zero
    /// (no wire format), and uses a steady-clock nanosecond response timestamp.
    /// Returns a typed reply, possibly commandSuccess=false, or an error future.
    /// After unlocking and preparing the result, attempt the Image response first
    /// (even for a C2 submission), then submitting-channel status. Error-only
    /// outcomes omit the Image reply; admission at dispatch may lose to detach.
    mel::RequestFor<ir::CameraCommandResp> camera_command(const ir::CameraCommand& command);

    /// Route a typed camera reply on the selected Image attachment. Default no-op;
    /// called outside operation/state locks with recipient pinned.
    virtual void on_camera_reply(const ir::CameraCommandResp&);
    /// Route correlated C2 status outside operation/state locks. Default no-op;
    /// hook may synchronously dispatch to the registered consumer.
    virtual void on_command_status(const ir::CommandStatus&);
    /// Open acquisition-associated auxiliary work with operations held, before
    /// launching the frame worker. Default succeeds without work; false fails enable.
    virtual bool start_auxiliary_locked();
    /// Stop applicable auxiliary work for disable, detach (including Control
    /// destruction) or failed acquisition startup. Default no-op. Normally
    /// operations is held; derived destructors may perform defensive final cleanup.
    virtual void stop_auxiliary_locked();
    std::shared_ptr<Runtime> runtime_;
    ir::Config config_;
    std::mutex callback_mutex_;

private:
    // C2 Standby is the only command allowed to stop the attached acquisition.
    // The caller pins Control, C2 and Image/Stack, rejects callback reentry, validates
    // the request, and holds Runtime::operations (never state/callback mutexes).
    friend class C2ChannelImpl;
    /// Stop/wake/join an owned acquisition and clear its publication, not its
    /// buffer pool. Called under operations, or defensively at final destruction;
    /// detach the thread handle instead of joining self on a worker teardown path.
    void stop_locked();

    /// Validate exact source-declared Stack product, generation and owned payload;
    /// invalid products fault the worker. Copy one packed stack, reserve admission
    /// against detach and call onImageStack (never onImage). A non-success return
    /// or throw stops acquisition but leaves a published buffer delivered.
    static void run_stacks(const std::shared_ptr<StreamState>& state,
                           const std::weak_ptr<ChannelCore>& weak) noexcept;

    /// Validate complete Image layout, span, generations and integration-start
    /// timestamp in the selected domain, then strip stride padding and publish
    /// before onImage. Bad frames are rejected; consumer exceptions, clock
    /// generation changes, timestamp regressions and terminal receive failures
    /// fault the worker. A callback throw never releases the delivered buffer.
    /// Both delivery loops drop when no destination is available, cancel only
    /// unpublished fills, and retain state independently of the weak channel.
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
