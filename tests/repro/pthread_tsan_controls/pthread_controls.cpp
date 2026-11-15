// GIGE-MEL-040-R2 SDK-free pthread lifecycle controls for sanitizer characterization.
//
// Built twice from this one source:
//   pthread_valid_controls     (PTHREAD_CONTROLS_NEGATIVE undefined): only VALID lifecycles.
//   pthread_negative_controls  (PTHREAD_CONTROLS_NEGATIVE=1): ONE deliberately invalid
//       lifecycle per process, selected on the command line. These characterize
//       sanitizer behavior; they never justify invalid production code and are
//       never mixed into the passing controls or ordinary CTest.
//
// Each stage prints the underlying pthread result codes before continuing.
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {
std::atomic<int> finished{0};

void* worker(void*) {
    finished.fetch_add(1, std::memory_order_release);
    return nullptr;
}

void report(const char* step, int rc) {
    std::printf("RESULT %s rc=%d (%s)\n", step, rc, rc == 0 ? "OK" : std::strerror(rc));
    std::fflush(stdout);
}

int create(pthread_t& thread) {
    const int rc = pthread_create(&thread, nullptr, worker, nullptr);
    report("pthread_create", rc);
    std::printf("PTHREAD_T 0x%lx\n", static_cast<unsigned long>(thread));
    return rc;
}

void wait_finished(int expected) {
    // Bounded wait for the worker to exit its start routine.
    for (int i = 0; i != 2000 && finished.load(std::memory_order_acquire) < expected; ++i) usleep(1000);
    usleep(50000);  // allow glibc thread teardown to complete after return
}

#ifndef PTHREAD_CONTROLS_NEGATIVE
int create_join() {
    pthread_t thread{};
    if (const int rc = create(thread)) return rc;
    const int rc = pthread_join(thread, nullptr);
    report("pthread_join", rc);
    return rc;
}

int create_detach() {
    pthread_t thread{};
    if (const int rc = create(thread)) return rc;
    const int rc = pthread_detach(thread);
    report("pthread_detach", rc);
    wait_finished(finished.load() + 1);
    return rc;
}

int complete_then_join() {
    pthread_t thread{};
    const int before = finished.load();
    if (const int rc = create(thread)) return rc;
    wait_finished(before + 1);  // thread completes BEFORE the valid join
    const int rc = pthread_join(thread, nullptr);
    report("pthread_join_after_completion", rc);
    return rc;
}
#else
// INVALID: the second detach is undefined behavior (POSIX); glibc may return EINVAL.
int repeated_detach() {
    pthread_t thread{};
    if (const int rc = create(thread)) return rc;
    report("pthread_detach_first", pthread_detach(thread));
    std::puts("STAGE before_invalid_second_detach");
    std::fflush(stdout);
    report("pthread_detach_second_INVALID", pthread_detach(thread));
    return 0;
}

// INVALID: detach of an already-joined (retired) thread is undefined behavior.
int detach_after_join() {
    pthread_t thread{};
    if (const int rc = create(thread)) return rc;
    report("pthread_join", pthread_join(thread, nullptr));
    std::puts("STAGE before_invalid_detach_after_join");
    std::fflush(stdout);
    report("pthread_detach_after_join_INVALID", pthread_detach(thread));
    return 0;
}

// INVALID: detach of a thread that already exited after being detached.
int detach_after_detached_exit() {
    pthread_t thread{};
    const int before = finished.load();
    if (const int rc = create(thread)) return rc;
    report("pthread_detach_first", pthread_detach(thread));
    wait_finished(before + 1);
    std::puts("STAGE before_invalid_detach_after_detached_exit");
    std::fflush(stdout);
    report("pthread_detach_after_exit_INVALID", pthread_detach(thread));
    return 0;
}
#endif
}

int main(int argc, char** argv) {
    const std::string_view mode = argc == 2 ? argv[1] : "";
#ifdef PTHREAD_CONTROLS_NEGATIVE
    int (*run)() = mode == "repeated-detach" ? repeated_detach
                 : mode == "detach-after-join" ? detach_after_join
                 : mode == "detach-after-detached-exit" ? detach_after_detached_exit : nullptr;
    if (!run) {
        std::fputs("usage: pthread_negative_controls repeated-detach|detach-after-join|detach-after-detached-exit\n", stderr);
        return 64;
    }
    std::printf("NEGATIVE_CONTROL %s (deliberately invalid lifecycle)\n", argv[1]);
#else
    int (*run)() = mode == "create-join" ? create_join
                 : mode == "create-detach" ? create_detach
                 : mode == "complete-then-join" ? complete_then_join : nullptr;
    if (!run) {
        std::fputs("usage: pthread_valid_controls create-join|create-detach|complete-then-join\n", stderr);
        return 64;
    }
    std::printf("VALID_CONTROL %s\n", argv[1]);
#endif
    std::fflush(stdout);
    const int rc = run();
    std::printf("CONTROL_COMPLETE %s rc=%d\n", argv[1], rc);
    return rc == 0 ? 0 : 1;
}
