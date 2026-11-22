#pragma once

// Internal experiment. No SDK, MEL, socket, acquisition or sensor protocol.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace gige_mel::control {
using Clock = std::chrono::steady_clock;
struct Generation {
    std::uint64_t value{}; // Zero denotes absence of evidence.
    friend bool operator==(Generation, Generation) = default;
};

enum class Outcome { not_attempted, acknowledged, uncertain, control_lost, unsupported };
struct Result {
    Generation generation; // Session of this submission, not an echoed wire token.
    Outcome outcome;
    bool unresolved{}; // Sticky for this session, including on control loss.
};

// A single owned fixture command value, not a register or navigation encoding.
struct Command { std::uint32_t value; };

class OperationContext {
public:
    const Generation generation;
    const Clock::time_point deadline;
    bool cancelled() const noexcept {
        return caller_.stop_requested() || session_.stop_requested();
    }
private:
    friend class Authority;
    OperationContext(Generation g, Clock::time_point d, std::stop_token caller, std::stop_token session)
        : generation(g), deadline(d), caller_(caller), session_(session) {}
    std::stop_token caller_, session_;
};

enum class Completion { pre_send, possible_effect, ack };
struct Evidence {
    Completion completion;
    Generation echo{}; // Endpoint evidence, never synthesized from the local context.
    bool lost{};       // Qualified loss evidence for this operation's session.
};

// Qualification precondition, not a timeout wrapper: execute and destruction
// must finish cooperatively, with no retained context, provider reentry, retries
// that can duplicate effects, or workers surviving retirement. pre_send requires
// proof the external transport operation was not entered. ack means transport
// confirmation only. The authority treats unexpected exceptions as possible effects.
class Executor {
public:
    virtual ~Executor() = default;
    virtual Evidence execute(Command, const OperationContext&) = 0;
};

enum class Phase { unavailable, open, closing, retired };
struct SessionState {
    Phase phase;
    bool lost{}, unresolved{};
};

namespace test { struct FixtureAdmission; }

// Owns exactly one admitted generation, or no session at all. No queue, worker,
// borrowed native service, rebind or recovery API. Keep alive through all calls.
class Authority final {
public:
    Authority() = default; // No qualified transport: submit returns unsupported.
    ~Authority();
    Authority(const Authority&) = delete;
    Authority& operator=(const Authority&) = delete;

    Result submit(Generation, Command, Clock::time_point, std::stop_token = {});
    void invalidate(Generation); // Terminal qualified control-loss notification.
    SessionState state() const;
    // Closes admission then cancels, drains and retires. False only for same-thread
    // reentry from execution/retirement; no state change on that refusal.
    bool shutdown();

private:
    // Deliberately no production grant. Only the test fixture can adopt an executor.
    friend struct test::FixtureAdmission;
    Authority(Generation, std::unique_ptr<Executor>);
    const Generation generation_{};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    SessionState state_{Phase::unavailable};
    std::thread::id running_, retiring_;
    std::stop_source cancel_;
    std::unique_ptr<Executor> executor_;
};
} // namespace gige_mel::control