#pragma once

#include "controlled_gvcp.hpp"

#include <atomic>
#include <condition_variable>
#include <thread>

namespace gige_mel::controlled_gvcp {
// Experimental fixture protocol, NOT physical-camera bootstrap declarations.
inline constexpr std::uint32_t fixture_magic_address = 0xffff0000;
inline constexpr std::uint32_t fixture_magic = 0x534f0039;
inline constexpr std::uint32_t fixture_identity_address = 0xffff0004;
inline constexpr std::uint32_t control_privilege_address = 0x00000a00;
enum class OwnerState { NotStarted, ClaimingControl, NotOwned, Owned, ControlLost,
                        OutcomeUnresolved, Closing, Closed };
enum class CloseOutcome { not_owned, released, control_lost, unresolved };
struct HeartbeatPolicy {
    std::chrono::milliseconds lease{500}; // Must match the explicitly configured fixture.
    std::chrono::milliseconds interval{100};
    std::chrono::milliseconds command_budget{50};
    std::chrono::milliseconds cleanup_budget{50};
};

class ControlOwner final : public IControlledRegisters {
public:
    static Result<std::unique_ptr<ControlOwner>> create(const std::string& ipv4,
        std::uint16_t port, HeartbeatPolicy = {});
    ~ControlOwner() override;
    ControlOwner(const ControlOwner&) = delete;
    ControlOwner& operator=(const ControlOwner&) = delete;
    Status acquire(TransactionOptions);
    Status heartbeat(TransactionOptions);
    Result<std::uint32_t> read_register(std::uint32_t, TransactionOptions) override;
    Status write_register(std::uint32_t, std::uint32_t, TransactionOptions) override;
    Status read_memory(std::uint32_t, std::span<std::byte>, TransactionOptions) override;
    Status write_memory(std::uint32_t, std::span<const std::byte>, TransactionOptions) override;
    CloseOutcome shutdown() noexcept;
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