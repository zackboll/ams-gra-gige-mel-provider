#include "clock_observer.hpp"
#include <cerrno>
#include <cstdlib>
#include <dlfcn.h>
#include <time.h>

namespace {
thread_local bool unavailable = false;
thread_local bool fixed = false;
thread_local timespec fixed_value{};
using Clock = int (*)(clockid_t, timespec*);
Clock forwarding_clock() noexcept {
    static const auto next = reinterpret_cast<Clock>(::dlsym(RTLD_NEXT, "clock_gettime"));
    if (!next || next == &::clock_gettime) std::abort();
    return next;
}
}
namespace test_clock {
Unavailable::Unavailable() noexcept { (void)forwarding_clock(); unavailable = true; }
Unavailable::~Unavailable() { unavailable = false; }
Fixed::Fixed() noexcept {
    if (forwarding_clock()(CLOCK_REALTIME, &fixed_value) != 0) std::abort();
    fixed = true;
}
Fixed::~Fixed() { fixed = false; }
}
extern "C" int clock_gettime(clockid_t clock, timespec* value) noexcept {
    if (unavailable && clock == CLOCK_REALTIME) { errno = EIO; return -1; }
    if (fixed && clock == CLOCK_REALTIME) { *value = fixed_value; return 0; }
    return forwarding_clock()(clock,value);
}