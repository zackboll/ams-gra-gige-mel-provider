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
// This thread's CLOCK_REALTIME repeats one valid instant (a host clock that
// does not advance between two reads).
class Fixed {
public:
    Fixed() noexcept;
    ~Fixed();
    Fixed(const Fixed&) = delete;
    Fixed& operator=(const Fixed&) = delete;
};
}