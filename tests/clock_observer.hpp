#pragma once

// Test-executable clock failure only; no provider hook or installed dependency.
namespace test_clock {
class Unavailable {
public:
    Unavailable() noexcept;
    ~Unavailable();
    Unavailable(const Unavailable&) = delete;
    Unavailable& operator=(const Unavailable&) = delete;
};
}