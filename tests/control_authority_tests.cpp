#include "control/authority.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace c = gige_mel::control;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::to_string(__LINE__) + ": " #x); } while (false)

namespace gige_mel::control::test {
struct FixtureAdmission {
    static Authority grant(Generation generation, std::unique_ptr<Executor> executor) {
        return Authority(generation, std::move(executor));
    }
};
}

namespace {
constexpr c::Generation a{101}, b{102};
auto deadline() { return c::Clock::now() + 2s; }

// Bounded rendezvous for test ordering; no scheduler-delay assumptions or sleeps.
class Gate {
public:
    void arrive() {
        std::lock_guard lock(mutex_);
        arrived_ = true;
        changed_.notify_all();
    }
    void wait_for_arrival() {
        std::unique_lock lock(mutex_);
        CHECK(changed_.wait_until(lock, deadline(), [&] { return arrived_; }));
    }
    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
    bool wait_for_release(const c::OperationContext& context) {
        std::unique_lock lock(mutex_);
        while (!context.cancelled() && c::Clock::now() < context.deadline) {
            if (released_) return true;
            changed_.wait_until(lock, std::min(context.deadline, c::Clock::now() + 2ms));
        }
        return false;
    }
    bool wait_for_release() {
        std::unique_lock lock(mutex_);
        return changed_.wait_until(lock, deadline(), [&] { return released_; });
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool arrived_{}, released_{};
};

enum class Mode { ack, refuse, before_send, in_flight, lost_ack, loss_with_ack, reentry, stale_ack, no_epoch, throws };
struct Observed {
    Gate gate;
    Gate* retirement_gate{};
    std::atomic<unsigned> entries{}, sends{}, active{}, retirements{};
    std::atomic<std::uint32_t> applied{};
    bool shutdown_refused{}, nested_refused{}, drained{}, closed_before_retire{}, retirement_released{};
    c::Authority* authority{}; // Kept alive by the test throughout all calls.
};

// No thread or blocking external transport. The simulated endpoint applies at
// most once; waiting for the response uses the same absolute budget as pre-send.
// No retransmission, pending-response extensions or provider calls.
class Fixture final : public c::Executor {
public:
    Fixture(Observed& obs, Mode mode, c::Generation echo) : obs_(obs), mode_(mode), echo_(echo) {}
    ~Fixture() override {
        obs_.drained = obs_.active == 0;
        if (obs_.authority)
            obs_.closed_before_retire = obs_.authority->state().phase == c::Phase::closing;
        if (obs_.retirement_gate) {
            obs_.retirement_gate->arrive();
            obs_.retirement_released = obs_.retirement_gate->wait_for_release();
        }
        ++obs_.retirements;
    }

    c::Evidence execute(c::Command command, const c::OperationContext& context) override {
        using enum c::Completion;
        struct Running {
            std::atomic<unsigned>& active;
            ~Running() { --active; }
        } running{obs_.active};
        CHECK(++obs_.active == 1);
        ++obs_.entries;
        // Only an explicit synthetic endpoint grant enables a transmission.
        if (!echo_.value || mode_ == Mode::refuse) return {pre_send};
        if (mode_ == Mode::before_send) {
            obs_.gate.arrive();
            if (!obs_.gate.wait_for_release(context)) return {pre_send};
        }
        if (context.cancelled() || c::Clock::now() >= context.deadline) return {pre_send};
        ++obs_.sends;
        obs_.applied = command.value;
        if (mode_ == Mode::in_flight || mode_ == Mode::lost_ack) {
            obs_.gate.arrive();
            if (!obs_.gate.wait_for_release(context)) return {possible_effect};
        }
        if (mode_ == Mode::reentry) {
            obs_.nested_refused = obs_.authority->submit(context.generation, {9}, context.deadline).outcome ==
                                  c::Outcome::not_attempted;
            obs_.shutdown_refused = !obs_.authority->shutdown();
        }
        if (mode_ == Mode::throws) throw std::runtime_error("after simulated effect");
        if (mode_ == Mode::stale_ack) return {ack, a}; // Only the old endpoint result is delivered.
        if (mode_ == Mode::no_epoch) return {ack}; // The expected epoch is missing, never locally filled in.
        return {ack, echo_, mode_ == Mode::loss_with_ack};
    }
private:
    Observed& obs_;
    Mode mode_;
    c::Generation echo_;
};

auto grant(Observed& obs, Mode mode, c::Generation generation = a) {
    return c::test::FixtureAdmission::grant(generation, std::make_unique<Fixture>(obs, mode, generation));
}

void admission_and_refusal() {
    using enum c::Outcome;
    c::Authority unavailable;
    CHECK(unavailable.submit(a, {1}, deadline()).outcome == unsupported);
    Observed obs;
    auto owner = grant(obs, Mode::ack);
    CHECK(owner.submit(b, {1}, deadline()).outcome == not_attempted);
    CHECK(owner.submit(a, {1}, c::Clock::now()).outcome == not_attempted);
    CHECK(owner.submit(a, {1}, c::Clock::time_point::max()).outcome == not_attempted);
    std::stop_source stop;
    stop.request_stop();
    CHECK(owner.submit(a, {1}, deadline(), stop.get_token()).outcome == not_attempted);
    CHECK(obs.entries == 0 && obs.sends == 0);
    const auto result = owner.submit(a, {2}, deadline());
    CHECK(result.outcome == acknowledged && result.generation == a && !result.unresolved);
    CHECK(obs.sends == 1 && obs.applied == 2);
    CHECK(owner.shutdown() && owner.shutdown());
    CHECK(owner.submit(a, {3}, deadline()).outcome == not_attempted);
    CHECK(obs.sends == 1 && obs.retirements == 1 && obs.drained);

    Observed refused;
    auto refuses = grant(refused, Mode::refuse);
    CHECK(refuses.submit(a, {1}, deadline()).outcome == not_attempted);
    CHECK(refuses.submit(a, {2}, deadline()).outcome == not_attempted);
    CHECK(refused.entries == 2 && refused.sends == 0 && !refuses.state().unresolved);
}

void concurrent_and_reentrant() {
    using enum c::Outcome;
    Observed obs;
    auto owner = grant(obs, Mode::in_flight);
    c::Result result{};
    std::jthread first([&] { result = owner.submit(a, {11}, deadline()); });
    obs.gate.wait_for_arrival();
    CHECK(owner.submit(a, {22}, deadline()).outcome == not_attempted);
    CHECK(obs.entries == 1 && obs.sends == 1 && obs.active == 1);
    obs.gate.release();
    first.join();
    CHECK(result.outcome == acknowledged);
    CHECK(owner.submit(a, {33}, deadline()).outcome == acknowledged);
    CHECK(obs.sends == 2 && obs.applied == 33);

    Observed nested;
    auto reentrant = grant(nested, Mode::reentry);
    nested.authority = &reentrant;
    CHECK(reentrant.submit(a, {44}, deadline()).outcome == acknowledged);
    CHECK(nested.shutdown_refused && nested.nested_refused);
    CHECK(nested.entries == 1 && nested.sends == 1);
    CHECK(reentrant.shutdown() && nested.closed_before_retire && nested.drained);
}

void interruption() {
    using enum c::Outcome;
    for (const auto mode : {Mode::before_send, Mode::in_flight}) {
        Observed obs;
        auto owner = grant(obs, mode);
        std::stop_source stop;
        c::Result result{};
        const auto end = deadline();
        std::jthread sender([&] { result = owner.submit(a, {11}, end, stop.get_token()); });
        obs.gate.wait_for_arrival();
        stop.request_stop();
        sender.join();
        CHECK(c::Clock::now() < end); // Cancellation, not deadline fallback.
        if (mode == Mode::before_send) {
            CHECK(result.outcome == not_attempted && !result.unresolved && obs.sends == 0);
            obs.gate.release();
            CHECK(owner.submit(a, {22}, deadline()).outcome == acknowledged && obs.sends == 1);
        } else {
            CHECK(result.outcome == uncertain && result.unresolved && obs.applied == 11);
            CHECK(owner.submit(a, {22}, deadline()).outcome == not_attempted && obs.sends == 1);
        }
    }
    Observed no_send;
    auto before_send = grant(no_send, Mode::before_send);
    CHECK(before_send.submit(a, {5}, c::Clock::now() + 100ms).outcome == not_attempted);
    CHECK(no_send.entries == 1 && no_send.sends == 0 && !before_send.state().unresolved);

    Observed lost_ack;
    auto owner = grant(lost_ack, Mode::lost_ack);
    const auto end = c::Clock::now() + 100ms;
    const auto result = owner.submit(a, {7}, end);
    CHECK(c::Clock::now() >= end && c::Clock::now() < end + 1s);
    CHECK(result.outcome == uncertain && result.unresolved && lost_ack.applied == 7);
    CHECK(owner.submit(a, {7}, deadline()).outcome == not_attempted && lost_ack.sends == 1);
    CHECK(owner.shutdown() && owner.state().unresolved);

    Observed failed;
    auto throwing = grant(failed, Mode::throws);
    CHECK(throwing.submit(a, {8}, deadline()).outcome == uncertain);
    CHECK(throwing.state().unresolved && failed.sends == 1 && failed.applied == 8);
}

void control_loss_and_retirement() {
    using enum c::Outcome;
    Observed lost;
    auto owner = grant(lost, Mode::in_flight);
    c::Result loss{};
    const auto loss_end = deadline();
    std::jthread sender([&] { loss = owner.submit(a, {11}, loss_end); });
    lost.gate.wait_for_arrival();
    owner.invalidate(a);
    CHECK(owner.submit(a, {22}, deadline()).outcome == control_lost);
    sender.join();
    CHECK(c::Clock::now() < loss_end);
    CHECK(loss.outcome == control_lost && loss.unresolved && lost.applied == 11 && lost.sends == 1);
    CHECK(owner.shutdown() && owner.state().lost && owner.state().unresolved);

    Observed simultaneous;
    auto ack_and_loss = grant(simultaneous, Mode::loss_with_ack);
    const auto r = ack_and_loss.submit(a, {11}, deadline());
    CHECK(r.outcome == control_lost && r.unresolved && simultaneous.sends == 1);

    Observed closing;
    Gate retiring;
    closing.retirement_gate = &retiring;
    auto shuts = grant(closing, Mode::in_flight);
    closing.authority = &shuts;
    c::Result interrupted{};
    const auto end = deadline();
    std::jthread in_flight([&] { interrupted = shuts.submit(a, {11}, end); });
    closing.gate.wait_for_arrival();
    bool closed1{}, closed2{};
    std::jthread close1([&] { closed1 = shuts.shutdown(); });
    retiring.wait_for_arrival();
    CHECK(c::Clock::now() < end && shuts.state().phase == c::Phase::closing);
    std::jthread close2([&] { closed2 = shuts.shutdown(); });
    CHECK(shuts.submit(a, {22}, deadline()).outcome == not_attempted);
    retiring.release();
    close1.join(); close2.join(); in_flight.join();
    CHECK(closed1 && closed2 && shuts.shutdown());
    CHECK(interrupted.outcome == uncertain && interrupted.unresolved);
    CHECK(closing.sends == 1 && closing.retirements == 1 && closing.drained &&
          closing.closed_before_retire && closing.retirement_released);
    CHECK(shuts.state().phase == c::Phase::retired && shuts.state().unresolved);
}

void replacement() {
    using enum c::Outcome;
    Observed former;
    auto old = grant(former, Mode::ack, a);
    CHECK(old.submit(a, {1}, deadline()).outcome == acknowledged);
    CHECK(old.shutdown());

    // This is an independently granted fixture session, not recovery by close/reopen.
    Observed replacement;
    auto fresh = grant(replacement, Mode::stale_ack, b);
    fresh.invalidate(a);
    CHECK(!fresh.state().lost);
    CHECK(fresh.submit(a, {1}, deadline()).outcome == not_attempted && replacement.sends == 0);
    const auto old_ack = fresh.submit(b, {2}, deadline());
    CHECK(old_ack.outcome == uncertain && old_ack.unresolved && old_ack.generation == b);
    CHECK(fresh.submit(b, {3}, deadline()).outcome == not_attempted);
    CHECK(replacement.sends == 1 && replacement.applied == 2);
    CHECK(fresh.shutdown());

    Observed untagged;
    auto missing_epoch = grant(untagged, Mode::no_epoch, b);
    CHECK(missing_epoch.submit(b, {1}, deadline()).outcome == uncertain);
    CHECK(missing_epoch.submit(b, {2}, deadline()).outcome == not_attempted && untagged.sends == 1);
}
} // namespace

int main() {
    try {
        admission_and_refusal();
        concurrent_and_reentrant();
        interruption();
        control_loss_and_retirement();
        replacement();
        std::cout << "SDK-free control authority contracts passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}