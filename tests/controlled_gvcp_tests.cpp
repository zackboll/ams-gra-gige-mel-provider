#include "transport/controlled_gvcp.hpp"
#include "transport/gvcp_wire.hpp"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace gv = gige_mel::controlled_gvcp;
namespace wire = gv::wire;
using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #x); } while (false)

// Independent test-device codec: deliberately does not call the client's codec.
unsigned get16(const Bytes& b, std::size_t i) {
    return std::to_integer<unsigned>(b.at(i)) * 256 + std::to_integer<unsigned>(b.at(i + 1));
}
std::uint32_t get32(const Bytes& b, std::size_t i) {
    return (get16(b, i) << 16) | get16(b, i + 2);
}
void set16(Bytes& b, std::size_t i, unsigned v) {
    b.at(i) = std::byte((v / 256) & 255); b.at(i + 1) = std::byte(v & 255);
}
void append32(Bytes& b, std::uint32_t v) {
    for (unsigned shift : {24U, 16U, 8U, 0U}) b.push_back(std::byte((v >> shift) & 255));
}
Bytes ack(unsigned command, unsigned id, const Bytes& payload, unsigned status = 0) {
    Bytes b(8); set16(b, 0, status); set16(b, 2, command);
    set16(b, 4, static_cast<unsigned>(payload.size())); set16(b, 6, id);
    b.insert(b.end(), payload.begin(), payload.end()); return b;
}
Bytes word(std::uint32_t v) { Bytes b; append32(b, v); return b; }
gv::TransactionOptions budget(std::chrono::milliseconds limit = 500ms, std::stop_token stop = {}) {
    return {gv::Clock::now() + limit, stop};
}
enum class Mode { success, drop, delay, pending, bad_id, bad_command, malformed, wrong_sender,
                  device_error, partial, unavailable, hostile_then_good, duplicate };
struct Device {
    struct Scheduled { gv::Clock::time_point at; Bytes bytes; sockaddr_in peer; bool rogue{}; };
    int fd{-1}, rogue{-1};
    std::uint16_t port{};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Mode> modes;
    std::vector<Bytes> commands;
    std::map<std::uint32_t, std::uint32_t> registers;
    std::map<std::uint32_t, Bytes> memory;
    std::vector<Scheduled> scheduled;
    std::exception_ptr error;
    std::jthread worker;

    static int socket_on_loopback(std::uint16_t& port_out) {
        int s = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); CHECK(s >= 0);
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { close(s); CHECK(false); }
        socklen_t size = sizeof(address);
        CHECK(getsockname(s, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        CHECK(address.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        port_out = ntohs(address.sin_port); return s;
    }
    explicit Device(std::vector<Mode> plan = {}) : modes(std::move(plan)) {
        fd = socket_on_loopback(port); std::uint16_t ignored{}; rogue = socket_on_loopback(ignored);
        worker = std::jthread([this](std::stop_token stop) {
            try { run(stop); } catch (...) { std::lock_guard lock(mutex); error = std::current_exception(); changed.notify_all(); }
        });
    }
    ~Device() { worker.request_stop(); worker.join(); close(rogue); close(fd); }
    void wait_commands(std::size_t n) {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 1s, [&] { return commands.size() >= n || error; }));
        if (error) std::rethrow_exception(error);
    }
    std::size_t count() { std::lock_guard lock(mutex); if (error) std::rethrow_exception(error); return commands.size(); }
    std::uint32_t reg(std::uint32_t a) { std::lock_guard lock(mutex); return registers[a]; }
    Bytes mem(std::uint32_t a) { std::lock_guard lock(mutex); return memory[a]; }
    void send(const Scheduled& item) {
        CHECK(sendto(item.rogue ? rogue : fd, item.bytes.data(), item.bytes.size(), MSG_DONTWAIT,
              reinterpret_cast<const sockaddr*>(&item.peer), sizeof(item.peer)) == static_cast<ssize_t>(item.bytes.size()));
    }
    void handle(Bytes b, sockaddr_in peer) {
        std::lock_guard lock(mutex);
        CHECK(peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        CHECK(b.size() >= 12 && b[0] == std::byte{0x42} && b[1] == std::byte{1});
        CHECK(get16(b, 4) == b.size() - 8 && get16(b, 6) != 0);
        unsigned cmd = get16(b, 2), id = get16(b, 6);
        auto a = get32(b, 8);
        Mode mode = commands.size() < modes.size() ? modes[commands.size()] : Mode::success;
        commands.push_back(b);
        Bytes payload;
        if (cmd == 0x80) { CHECK(b.size() == 12); payload = word(registers[a]); }
        else if (cmd == 0x82) {
            CHECK(b.size() == 16);
            if (mode != Mode::device_error && mode != Mode::unavailable) registers[a] = get32(b, 12);
            payload = word(1);
        } else if (cmd == 0x84) {
            CHECK(b.size() == 16 && get32(b, 12) <= 508 && get32(b, 12) % 4 == 0);
            payload = word(a); auto data = memory[a]; data.resize(get32(b, 12));
            payload.insert(payload.end(), data.begin(), data.end());
        } else {
            CHECK(cmd == 0x86 && b.size() <= 520 && (b.size() - 12) % 4 == 0);
            if (mode != Mode::device_error && mode != Mode::unavailable) {
                memory[a] = Bytes(b.begin() + 12, b.end());
                if (mode == Mode::partial) memory[a].resize(4);
            }
            payload = word(a);
        }
        changed.notify_all();
        auto response = ack(cmd + 1, id, payload);
        const auto now = gv::Clock::now();
        auto queue = [&](Bytes bytes, std::chrono::milliseconds delay = 0ms, bool other = false) {
            scheduled.push_back({now + delay, std::move(bytes), peer, other});
        };
        switch (mode) {
        case Mode::drop: case Mode::unavailable: return;
        case Mode::delay: queue(response, 140ms); return;
        case Mode::pending:
            for (int i = 0; i < 50; ++i) queue(ack(0x89, id, word(60000)), i * 5ms);
            return;
        case Mode::bad_id: set16(response, 6, id + 100); break;
        case Mode::bad_command: set16(response, 2, 0x87); break;
        case Mode::malformed: set16(response, 4, 500); break;
        case Mode::wrong_sender: queue(response, 0ms, true); return;
        case Mode::device_error: case Mode::partial: queue(ack(cmd + 1, id, {}, 0x8006)); return;
        case Mode::hostile_then_good: {
            queue(response, 0ms, true); // correct bytes, wrong endpoint
            auto bad = response; bad[0] = std::byte{0x42}; queue(bad);
            bad = response; set16(bad, 0, 0x0020); queue(bad);
            bad = response; set16(bad, 6, id + 1); queue(bad);
            bad = response; set16(bad, 2, 0x89); queue(bad);
            for (std::size_t n = 0; n < 8; ++n) queue(Bytes(response.begin(), response.begin() + static_cast<std::ptrdiff_t>(n)));
            bad = response; bad.push_back(std::byte{0}); queue(bad);
            bad = response; bad.resize(2000); set16(bad, 4, 1992); queue(bad);
            queue(response, 15ms); return;
        }
        case Mode::duplicate: queue(response); queue(response, 10ms); return;
        case Mode::success: break;
        }
        queue(response);
    }
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            pollfd event{fd, POLLIN, 0}; CHECK(poll(&event, 1, 1) >= 0);
            if (event.revents & POLLIN) {
                Bytes bytes(2048); sockaddr_in peer{}; socklen_t length = sizeof(peer);
                auto n = recvfrom(fd, bytes.data(), bytes.size(), MSG_DONTWAIT,
                                  reinterpret_cast<sockaddr*>(&peer), &length);
                CHECK(n >= 0); bytes.resize(static_cast<std::size_t>(n)); handle(std::move(bytes), peer);
            }
            for (auto it = scheduled.begin(); it != scheduled.end();) {
                if (it->at <= gv::Clock::now()) { send(*it); it = scheduled.erase(it); }
                else ++it;
            }
        }
    }
};
std::unique_ptr<gv::Client> client(Device& server) {
    auto result = gv::Client::create("127.0.0.1", server.port); CHECK(result); return std::move(result.value());
}

void parser() {
    gv::detail::RequestIds ids;
    for (unsigned i = 1; i <= 65535; ++i) CHECK(ids.take() == i);
    CHECK(!ids.take()); CHECK(!ids.take());
    auto good = ack(0x81, 0x1234, word(0x12345678));
    auto parse = [&](const Bytes& b) { return wire::parse(b, 0x1234, 0x80, 0x100, 4).kind; };
    CHECK(parse(good) == wire::Kind::success);
    for (std::size_t n = 0; n < good.size(); ++n) CHECK(parse(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n))) == wire::Kind::ignore);
    for (unsigned n = 0; n <= 65535; ++n) {
        auto b = good; set16(b, 4, n);
        CHECK((parse(b) == wire::Kind::success) == (n == 4));
        set16(b, 4, 4); set16(b, 0, n);
        CHECK((parse(b) == wire::Kind::success) == (n == 0));
    }
    CHECK(parse(ack(0x81, 0x1234, {}, 0x8006)) == wire::Kind::device_error);
    CHECK(parse(ack(0x81, 0x1234, {}, 0x8fff)) == wire::Kind::device_error);
    CHECK(parse(ack(0x81, 0x1234, {}, 0x80ff)) == wire::Kind::device_error);
    CHECK(parse(ack(0x89, 0x1234, word(60000))) == wire::Kind::pending);
    CHECK(parse(ack(0x89, 0x1235, word(60000))) == wire::Kind::ignore);
    CHECK(parse(ack(0x83, 0x1234, word(1))) == wire::Kind::ignore);
    CHECK(wire::parse(ack(0x85, 1, word(0x104)), 1, 0x84, 0x100, 0).kind == wire::Kind::ignore);
    CHECK(wire::parse(ack(0x87, 1, word(0x104)), 1, 0x86, 0x100, 4).kind == wire::Kind::ignore);
    CHECK(wire::parse(ack(0x83, 1, word(0)), 1, 0x82, 0x100, 4).kind == wire::Kind::ignore);
    CHECK(wire::parse(good, 0x1234, 0x80, 0x100, SIZE_MAX).kind == wire::Kind::ignore);
    CHECK(wire::parse(good, 0x1234, 0x80, 0x100, 0).kind == wire::Kind::ignore);
    // Deterministic adversarial packet corpus, every length including oversized.
    std::uint32_t seed = 0x1273acbd;
    for (std::size_t n = 0; n < 3000; ++n) {
        Bytes b(n % 1024);
        for (auto& v : b) { seed = seed * 1664525 + 1013904223; v = std::byte(seed >> 24); }
        CHECK(parse(b) == wire::Kind::ignore);
    }
    std::cout << "parser: all truncations, 65536 lengths/statuses, correlation, echoes, index, 3000 adversarial packets PASS\n";
}
void success_and_validation() {
    Device server; auto c = client(server);
    CHECK(!gv::Client::create("192.0.2.1", server.port));
    CHECK(!gv::Client::create("0.0.0.0", server.port));
    CHECK(!gv::Client::create("::1", server.port));
    CHECK(!gv::Client::create("localhost", server.port));
    CHECK(!gv::Client::create("127.0.0.1", 3956));
    CHECK(!gv::Client::create("127.0.0.1", 0));
    auto start = gv::Clock::now();
    auto write = c->write_register(0x1234, 0x91ab23cd, budget()); CHECK(write);
    CHECK(gv::Clock::now() - start < 500ms);
    CHECK(write.value().transmitted && write.value().write_completion == gv::WriteCompletion::acknowledged);
    auto read = c->read_register(0x1234, budget()); CHECK(read && read.value() == 0x91ab23cd);
    Bytes pattern(336); for (std::size_t i = 0; i < pattern.size(); ++i) pattern[i] = std::byte((i * 73 + 19) & 255);
    CHECK(c->write_memory(0x200, pattern, budget())); CHECK(server.mem(0x200) == pattern);
    Bytes out(336); CHECK(c->read_memory(0x200, out, budget())); CHECK(out == pattern);
    Bytes maximum(508, std::byte{0x57}); CHECK(c->write_memory(0xfffffe04, maximum, budget()));
    Bytes max_out(508); CHECK(c->read_memory(0xfffffe04, max_out, budget())); CHECK(max_out == maximum);
    auto n = server.count(); CHECK(n == 6);
    CHECK(!c->write_register(1, 1, budget())); CHECK(!c->read_register(3, budget()));
    CHECK(!c->write_memory(0, {}, budget()));
    Bytes too_big(512), tail(3); CHECK(!c->write_memory(0, too_big, budget()));
    CHECK(!c->write_memory(0, tail, budget())); CHECK(!c->write_memory(0xfffffffc, pattern, budget()));
    CHECK(!c->read_register(0, {gv::Clock::time_point::max(), {}}));
    auto expired = c->write_register(0, 1, {gv::Clock::now() - 1ms, {}});
    CHECK(!expired && expired.error().code == gv::ErrorCode::timeout && !expired.error().transmitted);
    CHECK(server.count() == n);
    { std::lock_guard lock(server.mutex);
      CHECK(server.commands[0] == Bytes({std::byte{0x42}, std::byte{1}, std::byte{0}, std::byte{0x82},
          std::byte{0}, std::byte{8}, std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0x12},
          std::byte{0x34}, std::byte{0x91}, std::byte{0xab}, std::byte{0x23}, std::byte{0xcd}})); }
    std::cout << "success: 6 UDP commands, exact numeric/raw 336/508-byte round trips, no fragmentation; validation sends 0 PASS\n";
}
void deadlines() {
    for (auto mode : {Mode::drop, Mode::delay, Mode::pending, Mode::bad_id, Mode::bad_command, Mode::malformed, Mode::wrong_sender, Mode::unavailable}) {
        Device server({mode}); auto c = client(server); auto start = gv::Clock::now();
        auto result = c->write_register(0x100, 0x31415926, budget(65ms)); auto elapsed = gv::Clock::now() - start;
        CHECK(!result && result.error().code == gv::ErrorCode::timeout && result.error().transmitted);
        CHECK(result.error().write_completion == gv::WriteCompletion::uncertain);
        CHECK(elapsed >= 60ms && elapsed < 300ms); server.wait_commands(1);
        CHECK(server.count() == 1); CHECK(server.reg(0x100) == (mode == Mode::unavailable ? 0U : 0x31415926U));
        std::cout << "deadline mode=" << static_cast<int>(mode) << " elapsed_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << " commands=1 uncertain=true PASS\n";
    }
    Device server({Mode::hostile_then_good}); auto c = client(server);
    CHECK(c->read_register(0x100, budget())); CHECK(server.count() == 1);
    Device errors({Mode::device_error, Mode::partial}); auto e = client(errors);
    auto negative = e->write_register(0x100, 9, budget()); CHECK(!negative);
    CHECK(negative.error().code == gv::ErrorCode::device && negative.error().device_status == 0x8006);
    CHECK(negative.error().write_completion == gv::WriteCompletion::uncertain && errors.reg(0x100) == 0);
    Bytes bytes(336, std::byte{0x34}); auto partial = e->write_memory(0x200, bytes, budget());
    CHECK(!partial && partial.error().write_completion == gv::WriteCompletion::uncertain);
    CHECK(errors.mem(0x200) == Bytes(4, std::byte{0x34})); CHECK(errors.count() == 2);
    Device read_failure({Mode::malformed}); auto reader = client(read_failure);
    Bytes unchanged(336, std::byte{0xac});
    CHECK(!reader->read_memory(0x200, unchanged, budget(40ms)));
    CHECK(unchanged == Bytes(336, std::byte{0xac}));
    Device lost_memory({Mode::drop, Mode::drop}); auto bulk = client(lost_memory);
    auto lost = bulk->write_memory(0x200, bytes, budget(40ms));
    CHECK(!lost && lost.error().transmitted && lost.error().write_completion == gv::WriteCompletion::uncertain);
    CHECK(lost_memory.mem(0x200) == bytes && lost_memory.count() == 1);
    Bytes destination(336, std::byte{0x7a});
    auto lost_read = bulk->read_memory(0x200, destination, budget(40ms));
    CHECK(!lost_read && lost_read.error().code == gv::ErrorCode::timeout && lost_read.error().transmitted);
    CHECK(destination == Bytes(336, std::byte{0x7a}) && lost_memory.count() == 2);
    std::cout << "hostile ACKs ignored; negative status preserved; partial write retained, no rollback commands=2 PASS\n";
}
void cancellation_and_late() {
    Device server({Mode::pending}); auto c = client(server); std::stop_source stop;
    stop.request_stop(); auto before = c->write_register(0x100, 9, budget(100ms, stop.get_token()));
    CHECK(!before && before.error().code == gv::ErrorCode::cancelled && !before.error().transmitted);
    CHECK(before.error().write_completion == gv::WriteCompletion::not_attempted && server.count() == 0);
    std::stop_source active; std::optional<gv::Status> result;
    std::jthread caller([&] { result = c->write_register(0x100, 7, budget(1s, active.get_token())); });
    server.wait_commands(1);
    auto start = gv::Clock::now(); active.request_stop(); caller.join();
    auto elapsed = gv::Clock::now() - start;
    CHECK(elapsed < 150ms && result && !*result);
    CHECK(result->error().code == gv::ErrorCode::cancelled && result->error().transmitted);
    CHECK(server.count() == 1);
    Device late({Mode::delay, Mode::drop, Mode::success, Mode::duplicate, Mode::drop}); auto l = client(late);
    CHECK(!l->read_register(0x100, budget(40ms)));
    // Old ACK arrives 140ms after first send, while second command is waiting.
    auto second = l->read_register(0x100, budget(180ms)); CHECK(!second && second.error().code == gv::ErrorCode::timeout);
    CHECK(l->read_register(0x100, budget())); CHECK(l->read_register(0x100, budget()));
    CHECK(!l->read_register(0x100, budget(40ms))); CHECK(late.count() == 5);
    std::cout << "cancel before-send commands=0; receive cancel_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << " commands=1; late/duplicate ACKs cannot satisfy later IDs PASS\n";
}
std::size_t entries(const char* path) {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(path), std::filesystem::directory_iterator{}));
}
std::set<int> socket_fds() {
    std::set<int> result;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code error;
        auto link = std::filesystem::read_symlink(entry.path(), error).string();
        if (!error && link.starts_with("socket:")) result.insert(std::stoi(entry.path().filename()));
    }
    return result;
}
void concurrency_and_resources() {
    Device server({Mode::pending}); auto c = client(server); std::stop_source stop;
    std::jthread first([&] { auto r = c->read_register(0, budget(1s, stop.get_token())); CHECK(!r); });
    server.wait_commands(1);
    std::vector<std::jthread> callers;
    std::atomic<int> busy{};
    for (int i = 0; i < 16; ++i) callers.emplace_back([&] {
        auto r = c->read_register(4, budget(50ms)); if (!r && r.error().code == gv::ErrorCode::busy && !r.error().transmitted) ++busy;
    });
    callers.clear(); CHECK(busy == 16); CHECK(server.count() == 1);
    stop.request_stop(); first.join();
    std::set<unsigned> ids;
    for (int i = 0; i < 20; ++i) CHECK(c->write_register(4, static_cast<std::uint32_t>(i), budget()));
    { std::lock_guard lock(server.mutex); for (const auto& b : server.commands) CHECK(ids.insert(get16(b, 6)).second); }
    CHECK(server.count() == 21);
    // Observe /proc descriptors, local endpoint and thread count, not a public native handle.
    auto fds = entries("/proc/self/fd"), threads = entries("/proc/self/task");
    for (int i = 0; i < 20; ++i) {
        auto existing = socket_fds();
        auto created = client(server); CHECK(entries("/proc/self/fd") == fds + 1);
        CHECK(entries("/proc/self/task") == threads);
        auto current = socket_fds(); std::vector<int> added;
        std::set_difference(current.begin(), current.end(), existing.begin(), existing.end(), std::back_inserter(added));
        CHECK(added.size() == 1); int descriptor = added[0];
        CHECK((fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0);
        CHECK((fcntl(descriptor, F_GETFL) & O_NONBLOCK) != 0);
        sockaddr_in address{}; socklen_t length = sizeof(address);
        CHECK(getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        CHECK(address.sin_family == AF_INET && address.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        CHECK(address.sin_port != 0); created.reset();
        CHECK(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
    }
    CHECK(entries("/proc/self/fd") == fds); CHECK(entries("/proc/self/task") == threads);
    std::uint16_t unused{}; int socket = Device::socket_on_loopback(unused); close(socket);
    auto absent = gv::Client::create("127.0.0.1", unused); CHECK(absent);
    auto start = gv::Clock::now(); auto r = absent.value()->read_register(0, budget(40ms));
    CHECK(!r && gv::Clock::now() - start < 250ms);
    std::cout << "16 concurrent callers busy/no-send; 21 unique command IDs; 20 socket lifetimes fd delta=0 thread delta=0; absent server bounded PASS\n";
}
int main(int argc, char** argv) {
    try {
        CHECK(argc == 2);
        if (std::string(argv[1]) == "parser") parser();
        else { success_and_validation(); deadlines(); cancellation_and_late(); concurrency_and_resources(); }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
