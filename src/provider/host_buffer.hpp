#pragma once

#include "mel_types.hpp"
#include <mutex>

namespace gige_mel {
namespace [[gnu::visibility("hidden")]] provider_detail {

/** @brief Session-affine wrapper for borrowed consumer memory, not a pixel allocator.
 *
 * Operation | Delivery transition
 * --------- | -------------------
 * acquire | available to filling (requires registration)
 * publish | filling to delivered before consumer entry
 * cancel_fill | filling to available if publication did not happen
 * release | delivered to available, only at the consumer's explicit request
 *
 * Registration is independent of that state machine. A registered buffer or
 * any outstanding fill/delivery reserves its byte range against overlapping
 * registration in the same session. Detach can clear registration without
 * ending delivery. The consumer must keep storage alive through registration
 * and any outstanding delivery, and stop reading before release. The internal
 * mutex protects bookkeeping, not arbitrary client access to the bytes.
 */
class HostBuffer final : public ir::Buffer {
public:
    /// Retain a session identity token, without retaining the session itself.
    explicit HostBuffer(std::shared_ptr<const int> affinity);

    /** @brief Install a borrowed host range only while unregistered and available.
     * @param address Non-null base address.
     * @param size Nonzero capacity in bytes, representable as int64 and not wrapping the address range.
     * @param context Opaque consumer value returned unchanged by getContext().
     * @return Success, BadPointer for an invalid range, or Fail if in use.
     * Failure leaves the previous address, size and context intact.
     */
    ir::Return init(void* address, std::size_t size, std::int64_t context) override;
    /// Borrow the current base address (initially null), without changing ownership.
    void* getBufferAddress() const override;
    /// Same base as the buffer address; the packed payload has no prefix.
    void* getImageAddress() const override;
    /// Capacity in bytes, initially zero, not the size of the last payload.
    std::int64_t getSize() const override;
    /// Read the opaque consumer-supplied value, initially zero.
    std::int64_t getContext() const override;

    /// End a delivery, even after detach or from its callback. Fail if not delivered.
    ir::Return release() override;
    /// Copy flags under the buffer mutex; allocation can throw.
    ir::Return getFlags(std::vector<ir::BufferFlag>& flags) const override;
    /// Append a missing flag only while unregistered and available; otherwise no-op.
    void addFlag(ir::BufferFlag flag) override;
    /// Replace flags only while unregistered and available; otherwise no-op.
    void setFlags(const std::vector<ir::BufferFlag>& flags) override;

    /** @brief Claim a registration for a non-null, non-owning channel identity.
     * @param owner Borrowed identity tag; never dereferenced.
     * @param affinity Session identity token to match.
     * @param minimum_size Capacity required in bytes.
     * @pre Caller serializes session registration and overlap checks under operations.
     * @return Success only for the same affinity, initialized capacity of at
     * least minimum_size bytes, exactly the Host flag, and an unowned available
     * buffer. Rejections return BadPointer, NotSupported or Fail, respectively
     * for identity/size, flags or in-use state, without claiming ownership.
     */
    ir::Return bind(const void* owner, const std::shared_ptr<const int>& affinity, std::size_t minimum_size);
    /// Clear registration ownership only. In particular, a delivered buffer
    /// still reserves its bytes and needs release(); unbind is not cancellation.
    void unbind();
    /// Whether filling or delivered, independently of registration.
    bool outstanding() const;
    /// Whether registered or outstanding, hence protecting the byte range.
    bool reserved() const;
    /// Test the half-open address interval [start, end) against this reserved range.
    bool overlaps_reserved(std::uintptr_t start, std::uintptr_t end) const;
    /// Reserve a registered available destination of at least minimum_size bytes for one fill.
    bool acquire(std::size_t minimum_size);
    /// Mark the completed copy delivered. Caller must own the fill from acquire().
    void publish();
    /// Abandon only an unpublished fill; leave delivered data and registration intact.
    void cancel_fill();

private:
    enum class State { available, filling, delivered };
    std::shared_ptr<const int> affinity_;
    mutable std::mutex mutex_;
    void* address_{nullptr};
    std::size_t size_{0};
    std::int64_t context_{0};
    const void* owner_{nullptr};
    State state_{State::available};
    std::vector<ir::BufferFlag> flags_{ir::BufferFlag::Host};
};

} // namespace provider_detail
} // namespace gige_mel
