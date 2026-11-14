#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <pthread.h>

// Linux test-executable interposition only. No provider hooks or shipped switch.
namespace test_join {
void prepare(); // Resolve/validate RTLD_NEXT before any observation is armed.
class Observe {
public:
    explicit Observe(std::atomic<bool>& entered) noexcept;
    ~Observe();
    Observe(const Observe&) = delete;
    Observe& operator=(const Observe&) = delete;
};

// A callback arms only its own worker. Observe its self-detach request, but
// retain the native join right for this test's reaper. No provider owner is
// retained and no production code/hook is changed. Join covers TLS callbacks
// AND libstdc++'s release of the thread-exit notification's shared state.
class SelfDetach {
public:
    void arm(); // Selected worker only, before releasing external owners.
    void started() noexcept { started_ = true; } // Caller, after enable succeeds.
    void join(); // Caller; bounded admission watchdog, then real pthread_join.
    ~SelfDetach(); // Also reap before fixture unwind on assertion failure.
    void intercept(pthread_t thread) noexcept;
    SelfDetach() = default;
    SelfDetach(const SelfDetach&) = delete;
    SelfDetach& operator=(const SelfDetach&) = delete;
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    pthread_t worker_{};
    bool detached_{false};
    bool started_{false}; // Caller thread only.
    bool joined_{false}; // Caller thread only.
};
}