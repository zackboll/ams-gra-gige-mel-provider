#include "join_observer.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <iostream>
#include <pthread.h>
#include <stdexcept>

namespace {
using Join = int (*)(pthread_t, void**);
thread_local std::atomic<bool>* observation = nullptr;

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