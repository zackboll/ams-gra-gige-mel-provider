#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;

// No provider, SDK, interposition or sanitizer annotations. Each invocation is
// one fixed experiment, not retry-until-pass. The detached modes intentionally
// retain the original readiness-only synchronization for diagnosis.
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string mode = argv[1];
    if (mode != "value-join" && mode != "exit-join" &&
        mode != "exit-detach" && mode != "exit-detach-handshake" &&
        mode != "value-detach" && mode != "callback-vs-exit") return 2;
    const int count = std::stoi(argv[2]);
    if (count < 1) return 2;
    for (int i = 0; i < count; ++i) {
        std::promise<void> done;
        auto ready = done.get_future();
        std::promise<void> callback;
        auto completed = callback.get_future();
        std::promise<void> release;
        auto proceed = release.get_future();
        const bool at_exit = mode != "value-join" && mode != "value-detach";
        std::thread worker([&] {
            if (at_exit) done.set_value_at_thread_exit();
            else done.set_value();
            if (mode == "exit-detach-handshake") callback.set_value();
            if (mode == "callback-vs-exit") {
                callback.set_value();
                proceed.wait();
            }
        });
        if (mode == "exit-detach" || mode == "exit-detach-handshake" || mode == "value-detach") worker.detach();
        if (mode == "exit-detach-handshake") completed.wait();
        if (mode == "callback-vs-exit") {
            completed.wait();
            if (ready.wait_for(0s) != std::future_status::timeout) std::abort();
            release.set_value();
        }
        const auto status = ready.wait_for(2s);
        if (worker.joinable()) worker.join();
        if (status != std::future_status::ready) throw std::runtime_error("readiness watchdog expired");
    }
    std::cout << mode << ": " << count << " iterations completed\n";
}