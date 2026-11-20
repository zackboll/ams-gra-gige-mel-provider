#include "navigation_bridge.hpp"

#include <exception>
#include <new>
#include <optional>
#include <utility>

namespace gige_mel::backend::profiles {
namespace {
Error refused(ErrorCode code, std::string text) { return {code, "profile navigation: " + std::move(text)}; }
}

Result<std::unique_ptr<ProfileNavigationBridge>> ProfileNavigationBridge::create(
    std::shared_ptr<const ICameraProfileState> state, std::unique_ptr<INavigationOperationPort> port) {
    if (!state || !port) return refused(ErrorCode::invalid_argument, "bridge requires a profile state and a port");
    const auto* handler = navigation_handler(*state);
    if (!handler) return refused(ErrorCode::unsupported, "selected camera profile provides no navigation handler");
    return std::unique_ptr<ProfileNavigationBridge>(
        new ProfileNavigationBridge(std::move(state), *handler, std::move(port)));
}

ProfileNavigationBridge::ProfileNavigationBridge(std::shared_ptr<const ICameraProfileState> state,
                                                 const IProfileNavigationHandler& handler,
                                                 std::unique_ptr<INavigationOperationPort> port)
    : state_(std::move(state)), handler_(&handler), port_(std::move(port)) {}

ProfileNavigationBridge::~ProfileNavigationBridge() {
    // Destroying the bridge from its own handler or port would free code and
    // data that are still executing; refuse loudly rather than continue.
    if (!shutdown()) std::terminate();
}

bool ProfileNavigationBridge::retired() const {
    std::lock_guard lock(mutex_);
    return phase_ == Phase::retired;
}

Status ProfileNavigationBridge::submit(const NavigationReport& report) {
    const IProfileNavigationHandler* handler = nullptr;
    INavigationOperationPort* port = nullptr;
    {
        std::lock_guard lock(mutex_);
        const auto self = std::this_thread::get_id();
        if (active_ == self || retiring_ == self)
            return refused(ErrorCode::invalid_state, "reentrant submission from the handler or port is refused");
        if (phase_ != Phase::open) return refused(ErrorCode::disconnected, "bridge is closing or retired");
        if (lost_) return refused(ErrorCode::disconnected, "control was lost; no further submission");
        if (unresolved_)
            return refused(ErrorCode::invalid_state, "an earlier uncertain submission is unresolved; no retry");
        if (active_ != std::thread::id{})
            return refused(ErrorCode::busy, "another navigation submission is active (no queue)");
        active_ = self;
        handler = handler_;
        port = port_.get();
    }
    // state_ and port_ stay owned until shutdown() observes active_ cleared.
    std::optional<Status> status;
    try {
        status.emplace(run(*handler, *port, report));
    } catch (...) {
        // Only an allocation failure while building the diagnostic can reach
        // here. The port may have been entered, so never claim a definite outcome.
        std::lock_guard lock(mutex_);
        unresolved_ = true;
        active_ = {};
        changed_.notify_all();
        throw;
    }
    {
        std::lock_guard lock(mutex_);
        active_ = {};
    }
    changed_.notify_all();
    return std::move(*status);
}

Status ProfileNavigationBridge::run(const IProfileNavigationHandler& handler, INavigationOperationPort& port,
                                    const NavigationReport& report) {
    // Stage 1: profile preparation. Every failure here is definite: the port
    // has not been invoked, so nothing can have been transmitted.
    std::optional<NavigationOperation> operation;
    try {
        auto prepared = handler.prepare_navigation(report);
        if (!prepared) return prepared.error();
        operation.emplace(std::move(prepared).value());
    } catch (const std::bad_alloc&) {
        return refused(ErrorCode::resource_exhausted, "handler allocation failed before submission");
    } catch (const std::exception& error) {
        return refused(ErrorCode::invalid_state, std::string("handler threw before submission: ") + error.what());
    } catch (...) {
        return refused(ErrorCode::invalid_state, "handler threw an unknown exception before submission");
    }
    if (operation->format.empty())
        return refused(ErrorCode::invalid_state, "handler returned an operation without a format; not submitted");
    if (!(operation->report == report))
        return refused(ErrorCode::invalid_state, "handler altered the report copy; not submitted");

    // Stage 2: backend-owned port. After entry, absence of proof is uncertainty.
    NavigationExecution evidence;
    try {
        evidence = port.submit(std::move(*operation));
    } catch (...) {
        evidence = {NavigationCompletion::uncertain, ErrorCode::io_error, "port threw after entry", false};
    }
    std::lock_guard lock(mutex_);
    if (evidence.control_lost) lost_ = true;
    switch (evidence.completion) {
    case NavigationCompletion::not_submitted:
        if (lost_) return refused(ErrorCode::disconnected, "control lost before submission: " + evidence.diagnostic);
        return refused(evidence.code, "port did not submit: " + evidence.diagnostic);
    case NavigationCompletion::accepted:
        if (!lost_) return {};
        break; // Acceptance reported with or after control loss is not trusted.
    case NavigationCompletion::uncertain:
        break;
    }
    unresolved_ = true;
    return refused(ErrorCode::timeout, std::string("backend acceptance is uncertain") +
        (lost_ ? " after control loss" : "") + (evidence.diagnostic.empty() ? "" : ": " + evidence.diagnostic));
}

bool ProfileNavigationBridge::shutdown() {
    std::shared_ptr<const ICameraProfileState> state;
    std::unique_ptr<INavigationOperationPort> port;
    {
        std::unique_lock lock(mutex_);
        const auto self = std::this_thread::get_id();
        if (active_ == self || retiring_ == self) return false;
        if (phase_ == Phase::retired) return true;
        if (phase_ == Phase::closing) {
            changed_.wait(lock, [&] { return phase_ == Phase::retired; });
            return true;
        }
        phase_ = Phase::closing; // Admission closes before the drain.
        retiring_ = self;
        changed_.wait(lock, [&] { return active_ == std::thread::id{}; });
        handler_ = nullptr;
        port = std::move(port_);
        state = std::move(state_);
    }
    // Retire outside the lock: the port first (it may use backend resources),
    // then this bridge's reference to the profile state (possibly module code).
    port.reset();
    state.reset();
    {
        std::lock_guard lock(mutex_);
        retiring_ = {};
        phase_ = Phase::retired;
    }
    changed_.notify_all();
    return true;
}

} // namespace gige_mel::backend::profiles
