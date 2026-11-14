#include "join_observer.hpp"

#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>
#include <pthread.h>
#include <stdexcept>

namespace {
using Join = int (*)(pthread_t, void**);
thread_local std::atomic<bool>* observation = nullptr;
thread_local test_join::SelfDetach* self_detach = nullptr;

Join forwarding_join() noexcept {
    // RTLD_NEXT preserves the dynamic sanitizer interceptor when present; never
    // dlopen libc or resolve a versioned libc-only symbol to bypass it.
    static const auto next = reinterpret_cast<Join>(::dlsym(RTLD_NEXT, "pthread_join"));
    if (!next || next == &::pthread_join) std::abort();
    return next;
}
}

namespace test_join {
void prepare() {
    const auto next = forwarding_join();
    Dl_info info{};
    if (!::dladdr(reinterpret_cast<void*>(next), &info) || !info.dli_fname)
        throw std::runtime_error("could not validate pthread_join forwarding target");
    std::cout << "Test pthread_join forwarding target: " << info.dli_fname << '\n';
}
Observe::Observe(std::atomic<bool>& entered) noexcept { observation = &entered; }
Observe::~Observe() { observation = nullptr; }

void SelfDetach::arm() {
    if (self_detach) std::abort();
    self_detach = this;
}
void SelfDetach::intercept(pthread_t thread) noexcept {
    if (!::pthread_equal(thread, ::pthread_self())) std::abort();
    {
        std::lock_guard lock(mutex_);
        if (detached_) std::abort();
        worker_ = thread;
        detached_ = true;
    }
    changed_.notify_all();
}
void SelfDetach::join() {
    if (joined_) return;
    pthread_t worker;
    {
        std::unique_lock lock(mutex_);
        // Fail closed: never unwind captured fixture objects under a live worker.
        if (!changed_.wait_for(lock, std::chrono::seconds(5), [this] { return detached_; })) std::abort();
        worker = worker_;
    }
    if (::pthread_join(worker, nullptr) != 0) std::abort();
    joined_ = true;
}
SelfDetach::~SelfDetach() { if (started_) join(); }
}

extern "C" int pthread_join(pthread_t thread, void** result) {
    const auto next = forwarding_join();
    // Once-only, nonthrowing notification on the specifically armed stopping
    // thread. Unrelated joins (including std::async future cleanup) aren't proof.
    if (auto* entered = observation) {
        observation = nullptr;
        entered->store(true, std::memory_order_release);
    }
    return next(thread, result);
}

extern "C" int pthread_detach(pthread_t thread) noexcept {
    if (auto* selected = self_detach) {
        self_detach = nullptr;
        selected->intercept(thread);
        return 0;
    }
    using Detach = int (*)(pthread_t);
    static const auto next = reinterpret_cast<Detach>(::dlsym(RTLD_NEXT, "pthread_detach"));
    if (!next || next == &::pthread_detach) std::abort();
    return next(thread);
}
