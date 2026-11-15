#include "gvcp_control_owner.hpp"

#include <algorithm>

namespace gige_mel::controlled_gvcp {
namespace {
Failure refusal(ErrorCode code = ErrorCode::device) { return Failure{code}; }
bool valid(TransactionOptions o) { return o.deadline != Clock::time_point::max(); }
bool admit(std::unique_lock<std::mutex>& lock, TransactionOptions options) {
    const auto until = std::min(options.deadline, Clock::now() + std::chrono::milliseconds{2});
    do {
        if (lock.try_lock()) return true;
        if (options.stop.stop_requested()) return false;
        std::this_thread::yield();
    } while (Clock::now() < until);
    return false;
}
}
Result<std::unique_ptr<ControlOwner>> ControlOwner::create(const std::string& ip,
        std::uint16_t port, HeartbeatPolicy policy) {
    using namespace std::chrono_literals;
    if (policy.interval <= 0ms || policy.command_budget <= 0ms || policy.cleanup_budget <= 0ms ||
        policy.lease > 60s || policy.interval >= policy.lease || policy.command_budget >= policy.lease ||
        policy.cleanup_budget >= policy.lease) return refusal(ErrorCode::invalid_argument);
    auto client = Client::create(ip, port);
    if (!client) return client.error();
    auto owner = std::unique_ptr<ControlOwner>(new ControlOwner(std::move(client.value()), policy));
    return owner;
}
ControlOwner::~ControlOwner() { shutdown(); }
OwnerState ControlOwner::state() const { std::lock_guard lock(mutex_); return state_; }

void ControlOwner::failed(const Failure& error) {
    if (error.code == ErrorCode::invalid_argument || error.code == ErrorCode::busy ||
        (!error.transmitted && (error.code == ErrorCode::cancelled || error.code == ErrorCode::timeout))) return;
    // Neither a negative ACK nor a transport failure proves controller identity.
    state_ = OwnerState::OutcomeUnresolved;
    outcome_ = CloseOutcome::unresolved;
}
Status ControlOwner::acquire(TransactionOptions options) {
    if (!valid(options)) return refusal(ErrorCode::invalid_argument);
    std::unique_lock lock(mutex_, std::defer_lock);
    if (!admit(lock, options)) return refusal(ErrorCode::busy);
    if (closing_ || state_ != OwnerState::NotStarted) return refusal();
    if (options.stop.stop_requested()) return refusal(ErrorCode::cancelled);
    if (Clock::now() >= options.deadline) return refusal(ErrorCode::timeout);
    state_ = OwnerState::ClaimingControl;
    std::stop_source combined;
    std::stop_callback caller(options.stop, [&] { combined.request_stop(); });
    std::stop_callback close(cancel_.get_token(), [&] { combined.request_stop(); });
    options.stop = combined.get_token();
    options.deadline = std::min(options.deadline, Clock::now() + policy_.command_budget);
    try {
        auto reject = [&](Failure error, bool transition) -> Status {
            state_ = transition ? OwnerState::OutcomeUnresolved : OwnerState::NotOwned;
            outcome_ = transition ? CloseOutcome::unresolved : CloseOutcome::not_owned;
            return error;
        };
        auto magic = client_->read_register(fixture_magic_address, options);
        if (!magic) return reject(magic.error(), false);
        if (magic.value() != fixture_magic) return reject(refusal(), false);
        auto prior = client_->read_register(control_privilege_address, options);
        if (!prior) return reject(prior.error(), false);
        if (prior.value() != 0) return reject(refusal(), false);
        const auto started = Clock::now();
        auto claim = client_->write_register(control_privilege_address, 2, options);
        if (!claim) return reject(claim.error(), claim.error().transmitted);
        auto identity = client_->read_register(fixture_identity_address, options);
        if (!identity) return reject(identity.error(), true);
        if (identity.value() != 1) return reject(refusal(), true);
        lease_end_ = started + policy_.lease;
        next_heartbeat_ = started + policy_.interval;
        state_ = OwnerState::Owned;
        worker_ = std::jthread([this](std::stop_token stop) { scheduler(stop); });
        changed_.notify_all();
        return claim;
    } catch (...) {
        state_ = OwnerState::OutcomeUnresolved; outcome_ = CloseOutcome::unresolved;
        throw;
    }
}

Status ControlOwner::refresh(TransactionOptions options) {
    const auto started = Clock::now();
    auto privilege = client_->read_register(control_privilege_address, options);
    if (!privilege) {
        state_ = OwnerState::OutcomeUnresolved; outcome_ = CloseOutcome::unresolved;
        return privilege.error();
    }
    if (privilege.value() != 2) {
        state_ = OwnerState::ControlLost; outcome_ = CloseOutcome::control_lost; return refusal();
    }
    auto identity = client_->read_register(fixture_identity_address, options);
    if (!identity) {
        state_ = OwnerState::OutcomeUnresolved; outcome_ = CloseOutcome::unresolved;
        return identity.error();
    }
    if (identity.value() != 1) {
        state_ = OwnerState::ControlLost; outcome_ = CloseOutcome::control_lost; return refusal();
    }
    // A privilege bit alone cannot establish identity. This is fixture-only proof.
    lease_end_ = started + policy_.lease;
    next_heartbeat_ = started + policy_.interval;
    changed_.notify_all();
    return Receipt{};
}

template<class F> auto ControlOwner::operation(TransactionOptions options, F&& action)
        -> decltype(std::declval<F>()(TransactionOptions{})) {
    if (!valid(options)) return refusal(ErrorCode::invalid_argument);
    std::unique_lock lock(mutex_, std::defer_lock);
    if (!admit(lock, options)) return refusal(ErrorCode::busy);
    if (closing_ || state_ != OwnerState::Owned) return refusal();
    if (Clock::now() >= lease_end_) {
        state_ = OwnerState::ControlLost; outcome_ = CloseOutcome::control_lost;
        return refusal(ErrorCode::timeout);
    }
    std::stop_source combined;
    std::stop_callback caller(options.stop, [&] { combined.request_stop(); });
    std::stop_callback close(cancel_.get_token(), [&] { combined.request_stop(); });
    options.stop = combined.get_token();
    options.deadline = std::min({options.deadline, lease_end_, Clock::now() + policy_.command_budget});
    try {
        auto result = action(options);
        // refresh() already classifies readback evidence or uncertainty. Preserve it.
        if (!result && state_ == OwnerState::Owned) failed(result.error());
        if (state_ == OwnerState::Owned && Clock::now() >= lease_end_) {
            state_ = OwnerState::ControlLost; outcome_ = CloseOutcome::control_lost;
        }
        return result;
    } catch (...) {
        state_ = OwnerState::OutcomeUnresolved; outcome_ = CloseOutcome::unresolved;
        throw;
    }
}
Status ControlOwner::heartbeat(TransactionOptions o) {
    return operation(o, [&](TransactionOptions t) { return refresh(t); });
}
bool ControlOwner::reserved(std::uint32_t address, std::size_t size) {
    if (size == 0 || size - 1 > UINT32_MAX - address) return true;
    const auto end = address + size - 1;
    return (address <= control_privilege_address + 3 && end >= control_privilege_address) ||
           end >= fixture_magic_address;
}
Result<std::uint32_t> ControlOwner::read_register(std::uint32_t a, TransactionOptions o) {
    if (reserved(a, 4)) return refusal(ErrorCode::invalid_argument);
    return operation(o, [&](TransactionOptions t) { return client_->read_register(a, t); });
}
Status ControlOwner::write_register(std::uint32_t a, std::uint32_t v, TransactionOptions o) {
    if (reserved(a, 4)) return refusal(ErrorCode::invalid_argument);
    return operation(o, [&](TransactionOptions t) { return client_->write_register(a, v, t); });
}
Status ControlOwner::read_memory(std::uint32_t a, std::span<std::byte> b, TransactionOptions o) {
    if (reserved(a, b.size())) return refusal(ErrorCode::invalid_argument);
    return operation(o, [&](TransactionOptions t) { return client_->read_memory(a, b, t); });
}
Status ControlOwner::write_memory(std::uint32_t a, std::span<const std::byte> b, TransactionOptions o) {
    if (reserved(a, b.size())) return refusal(ErrorCode::invalid_argument);
    return operation(o, [&](TransactionOptions t) { return client_->write_memory(a, b, t); });
}
void ControlOwner::scheduler(std::stop_token stop) {
    try {
        std::unique_lock lock(mutex_);
        while (!stop.stop_requested()) {
            if (state_ != OwnerState::Owned) {
                changed_.wait(lock, [&] { return stop.stop_requested() || state_ == OwnerState::Owned; });
                continue;
            }
            if (changed_.wait_until(lock, next_heartbeat_, [&] { return stop.stop_requested(); })) break;
            if (closing_) break;
            if (state_ != OwnerState::Owned) continue;
            if (Clock::now() >= lease_end_) {
                state_ = OwnerState::ControlLost; outcome_ = CloseOutcome::control_lost; continue;
            }
            refresh({std::min(lease_end_, Clock::now() + policy_.command_budget), cancel_.get_token()});
        }
    } catch (...) {
        std::lock_guard lock(mutex_);
        state_ = OwnerState::OutcomeUnresolved; outcome_ = CloseOutcome::unresolved;
    }
}
CloseOutcome ControlOwner::shutdown() noexcept {
    // Admission/cancellation precede any wait for the operation lock or worker.
    closing_.store(true);
    cancel_.request_stop();
    try {
        std::lock_guard close_lock(close_mutex_);
        { std::lock_guard lock(mutex_); worker_.request_stop(); changed_.notify_all(); }
        if (worker_.joinable()) worker_.join();
        std::lock_guard lock(mutex_);
        if (state_ == OwnerState::Closed) return outcome_;
        const bool owned = state_ == OwnerState::Owned && Clock::now() < lease_end_;
        if (state_ == OwnerState::Owned && !owned) outcome_ = CloseOutcome::control_lost;
        state_ = OwnerState::Closing;
        if (owned) {
            outcome_ = CloseOutcome::unresolved; // Set BEFORE any potentially throwing work.
            auto result = client_->write_register(control_privilege_address, 0,
                {std::min(lease_end_, Clock::now() + policy_.cleanup_budget), {}});
            if (result) outcome_ = CloseOutcome::released;
        }
        client_.reset(); state_ = OwnerState::Closed;
        return outcome_;
    } catch (...) {
        // Ordinary syscall failures are typed results. Contain unexpected teardown exceptions.
        std::lock_guard lock(mutex_);
        outcome_ = CloseOutcome::unresolved; client_.reset(); state_ = OwnerState::Closed;
        return outcome_;
    }
}
} // namespace gige_mel::controlled_gvcp