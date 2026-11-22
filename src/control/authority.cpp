#include "authority.hpp"

#include <utility>

namespace gige_mel::control {
Authority::Authority(Generation generation, std::unique_ptr<Executor> executor)
    : generation_(generation), state_{generation.value && executor ? Phase::open : Phase::unavailable},
      executor_(std::move(executor)) {}

Authority::~Authority() { shutdown(); }

SessionState Authority::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void Authority::invalidate(Generation generation) {
    std::lock_guard lock(mutex_);
    if (generation != generation_ || !generation.value || state_.phase == Phase::retired) return;
    state_.lost = true;
    // The private token is polled only. No stop callback runs under this lock.
    cancel_.request_stop();
}

Result Authority::submit(Generation generation, Command command, Clock::time_point deadline,
                         std::stop_token stop) {
    std::unique_lock lock(mutex_);
    if (state_.phase == Phase::unavailable) return {generation, Outcome::unsupported};
    if (generation != generation_) return {generation, Outcome::not_attempted};
    if (state_.lost) return {generation, Outcome::control_lost, state_.unresolved};
    const OperationContext context(generation, deadline, stop, cancel_.get_token());
    if (state_.phase != Phase::open || state_.unresolved || running_ != std::thread::id{} ||
        deadline == Clock::time_point::max() || Clock::now() >= deadline || context.cancelled())
        return {generation, Outcome::not_attempted, state_.unresolved};

    running_ = std::this_thread::get_id();
    lock.unlock();
    Evidence evidence{Completion::possible_effect};
    try { evidence = executor_->execute(command, context); }
    catch (...) { /* No proof of pre-send refusal after executor entry. */ }
    lock.lock();

    if (evidence.lost) {
        state_.lost = true;
        cancel_.request_stop();
    }
    Outcome outcome = Outcome::not_attempted;
    if (evidence.completion != Completion::pre_send) {
        if (evidence.completion == Completion::ack && evidence.echo == generation_ &&
            state_.phase == Phase::open && !context.cancelled() && Clock::now() < deadline) {
            outcome = Outcome::acknowledged;
        } else {
            state_.unresolved = true;
            outcome = Outcome::uncertain;
        }
    }
    if (state_.lost) outcome = Outcome::control_lost;
    running_ = {};
    changed_.notify_all();
    return {generation, outcome, state_.unresolved};
}

bool Authority::shutdown() {
    std::unique_lock lock(mutex_);
    const auto self = std::this_thread::get_id();
    if (running_ == self || retiring_ == self) return false;
    if (state_.phase == Phase::closing) {
        changed_.wait(lock, [&] { return state_.phase == Phase::retired; });
        return true;
    }
    if (state_.phase == Phase::retired) return true;
    state_.phase = Phase::closing;
    retiring_ = self;
    cancel_.request_stop();
    changed_.wait(lock, [&] { return running_ == std::thread::id{}; });
    auto executor = std::move(executor_);
    lock.unlock();
    executor.reset();
    lock.lock();
    retiring_ = {};
    state_.phase = Phase::retired;
    changed_.notify_all();
    return true;
}
} // namespace gige_mel::control