#pragma once

#include <atomic>

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
}