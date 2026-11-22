#pragma once

#include "controlled_gvcp.hpp"

#include <atomic>
#include <condition_variable>
#include <thread>

namespace gige_mel::controlled_gvcp {
/// Fixture bootstrap probe address; not a physical-camera contract.
inline constexpr std::uint32_t fixture_magic_address = 0xffff0000;
/// Fixed loopback fixture marker, not authentication or a session generation.
inline constexpr std::uint32_t fixture_magic = 0x534f0039;
/// Fixture-only source-endpoint identity query, not universal GigE Vision.
inline constexpr std::uint32_t fixture_identity_address = 0xffff0004;
/// Privilege field used by this experiment; these declarations do not specify
/// a physical camera's bootstrap or prove authority over one.
inline constexpr std::uint32_t control_privilege_address = 0x00000a00;

/// Local ownership evidence; loss and uncertainty both fence further operations.
enum class OwnerState {
    NotStarted,       ///< Created, no acquisition procedure started.
    ClaimingControl,  ///< Explicit claim procedure underway.
    NotOwned,         ///< Procedure failed before transmitting a claim; terminal.
    Owned,            ///< Claim acknowledged and fixture identity verified.
    ControlLost,      ///< Privilege/identity mismatch or local lease expiration.
    OutcomeUnresolved,///< Ownership or effects cannot be confirmed; no replay/release.
    Closing,          ///< Retirement in progress.
    Closed            ///< Locally retired; says nothing by itself about release.
};

/// Persisted retirement result: no claim, ACKed release, loss evidence, or uncertainty.
enum class CloseOutcome { not_owned, released, control_lost, unresolved };

/// Explicit fixture timings, not negotiated from a sensor. Interval and budgets
/// must be positive and shorter than the matching fixture lease (at most 60 s).
struct HeartbeatPolicy {
    std::chrono::milliseconds lease{500}; // Must match the explicitly configured fixture.
    std::chrono::milliseconds interval{100};
    std::chrono::milliseconds command_budget{50};
    std::chrono::milliseconds cleanup_budget{50};
};

/// @brief Experimental owner for the loopback fixture, not a production control path.
/// One private client serializes claim, data operations, heartbeat and release.
/// Ordinary calls require Owned and reject access to reserved privilege and
/// fixture ranges. Contended admission allows up to 2 ms, capped by the caller
/// deadline, and can return busy. Calls and heartbeat share the same non-reusing
/// ID sequence; deadlines are capped by policy and, once owned, the local lease.
/// A mismatch or local lease expiration establishes ControlLost; failed heartbeat
/// reads and other ambiguous failures can fence as OutcomeUnresolved. Invalid
/// calls, busy and ordinary pre-send cancellation/timeout need not fence. No
/// implicit reacquire.
/// @warning A fresh socket can reuse a local port and begin again at ID 1.
/// Delayed old ACKs can match new requests; fixture identity is not a session
/// epoch. Not integrated with production acquisition or qualified to coexist
/// with SDK-owned control sessions.
class ControlOwner final : public IControlledRegisters {
public:
    /// Validate fixture timing and create one loopback client without claiming.
    static Result<std::unique_ptr<ControlOwner>> create(const std::string& ipv4,
        std::uint16_t port, HeartbeatPolicy = {});
    ~ControlOwner() override;
    ControlOwner(const ControlOwner&) = delete;
    ControlOwner& operator=(const ControlOwner&) = delete;
    /// @brief Explicitly claim control from NotStarted using the fixture protocol.
    /// Supply an absolute deadline and stop token. The command budget further
    /// caps one shared deadline for the entire claim sequence.
    /// @return Acknowledged claim only after checking fixture marker, vacant
    /// privilege, matched claim ACK and fixture endpoint identity. Success starts
    /// the heartbeat worker. Failure during the procedure fences the owner as
    /// NotOwned (no transmitted claim) or OutcomeUnresolved (may have claimed).
    /// @note The fixture arbitrates competing claims, not a preceding read.
    /// Admission refusal need not start the procedure; no transition retry.
    Status acquire(TransactionOptions);

    /// Check privilege and fixture identity on the private client. A scheduled
    /// worker does the same at the policy interval. Refresh conservative local
    /// evidence from the start of the exchange, not the ACK arrival time.
    Status heartbeat(TransactionOptions);
    Result<std::uint32_t> read_register(std::uint32_t, TransactionOptions) override;
    Status write_register(std::uint32_t, std::uint32_t, TransactionOptions) override;
    Status read_memory(std::uint32_t, std::span<std::byte>, TransactionOptions) override;
    Status write_memory(std::uint32_t, std::span<const std::byte>, TransactionOptions) override;
    /// @brief Stop admission, request cancellation, join heartbeat, and retire the socket.
    /// @return Persistent outcome: not_owned, released on a matching release ACK,
    /// control_lost, or unresolved (including a failed release). One release is
    /// attempted only with still-valid owned evidence, within the cleanup budget
    /// and lease. Closing the socket is not proof of remote release.
    /// @note Repeated/concurrent calls do not retry cleanup. Cancellation is
    /// cooperative and cannot undo a transmitted operation. Keep the owner alive
    /// across calls and join external callers before destruction.
    CloseOutcome shutdown() noexcept;

    /// Synchronized snapshot of local evidence, not a native device check.
    OwnerState state() const;
private:
    ControlOwner(std::unique_ptr<Client> client, HeartbeatPolicy policy)
        : client_(std::move(client)), policy_(policy) {}
    template<class F> auto operation(TransactionOptions, F&&) -> decltype(std::declval<F>()(TransactionOptions{}));
    Status refresh(TransactionOptions); // mutex held, original deadline preserved
    void failed(const Failure&);
    void scheduler(std::stop_token);
    static bool reserved(std::uint32_t, std::size_t);
    std::unique_ptr<Client> client_; // Never escapes; no recreation or allocator reset.
    const HeartbeatPolicy policy_;
    mutable std::mutex mutex_;
    std::mutex close_mutex_;
    std::condition_variable changed_;
    std::atomic<bool> closing_{};
    std::stop_source cancel_;
    OwnerState state_{OwnerState::NotStarted};
    CloseOutcome outcome_{CloseOutcome::not_owned};
    Clock::time_point lease_end_{}, next_heartbeat_{};
    std::jthread worker_;
};
} // namespace gige_mel::controlled_gvcp