#include "transport/gvcp_control_owner.hpp"

#include <array>
#include <arpa/inet.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace gv = gige_mel::controlled_gvcp;
using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #x); } while (false)
// Independent byte codec: no client wire helpers, constants or parser used.
unsigned get16(const Bytes& b, std::size_t i) {
    return std::to_integer<unsigned>(b.at(i)) * 256 + std::to_integer<unsigned>(b.at(i + 1));
}
std::uint32_t get32(const Bytes& b, std::size_t i) { return (get16(b, i) << 16) | get16(b, i + 2); }
void set16(Bytes& b, std::size_t i, unsigned v) {
    b.at(i) = std::byte((v >> 8) & 255); b.at(i + 1) = std::byte(v & 255);
}
Bytes word(std::uint32_t v) {
    return {std::byte(v >> 24), std::byte((v >> 16) & 255), std::byte((v >> 8) & 255), std::byte(v & 255)};
}
Bytes ack(unsigned command, unsigned id, Bytes payload, unsigned status = 0) {
    Bytes b(8); set16(b, 0, status); set16(b, 2, command);
    set16(b, 4, static_cast<unsigned>(payload.size())); set16(b, 6, id);
    b.insert(b.end(), payload.begin(), payload.end()); return b;
}
gv::TransactionOptions budget(std::chrono::milliseconds duration = 1s, std::stop_token stop = {}) {
    return {gv::Clock::now() + duration, stop};
}
enum class Fault { good, drop, pending, stale, hostile_then_good, delay };
struct Event { unsigned command, id; std::uint32_t address, value, ip; std::uint16_t port; bool accepted; };
struct Device {
    struct Scheduled { gv::Clock::time_point at; Bytes bytes; sockaddr_in peer; };
    int fd{-1}; std::uint16_t port{};
    std::mutex mutex; std::condition_variable changed;
    std::vector<Event> events;
    std::vector<Scheduled> scheduled;
    std::map<std::size_t, Fault> faults;
    std::map<std::uint32_t, Bytes> memory;
    std::uint32_t controller_ip{}; std::uint16_t controller_port{};
    std::uint32_t privilege{};
    gv::Clock::time_point expires{}, last_activity{};
    std::chrono::milliseconds lease;
    std::atomic<unsigned> claims{}, releases{}, losses{}, writes{}, refused{}, refreshes{};
    std::exception_ptr error;
    std::jthread worker;

    explicit Device(std::map<std::size_t, Fault> plan = {}, std::chrono::milliseconds expiry = 500ms)
        : faults(std::move(plan)), lease(expiry) {
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); CHECK(fd >= 0);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t size = sizeof(address); CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        port = ntohs(address.sin_port);
        worker = std::jthread([this](std::stop_token stop) {
            try { run(stop); } catch (...) { std::lock_guard lock(mutex); error = std::current_exception(); changed.notify_all(); }
        });
    }
    ~Device() { stop(); }
    void stop() { worker.request_stop(); if (worker.joinable()) worker.join(); if (fd >= 0) { close(fd); fd = -1; } }
    void expire() {
        if (privilege && gv::Clock::now() >= expires) {
            privilege = 0; controller_ip = 0; controller_port = 0; ++losses; changed.notify_all();
        }
    }
    void revoke() {
        std::lock_guard lock(mutex);
        // Another controller now owns the camera. Never release this identity.
        controller_port = 1; controller_ip = htonl(INADDR_LOOPBACK); privilege = 2;
        expires = gv::Clock::now() + lease; ++losses;
    }
    void wait(std::size_t n) {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 2s, [&] { return events.size() >= n || error; }));
        if (error) std::rethrow_exception(error);
    }
    std::vector<Event> snapshot() {
        std::lock_guard lock(mutex); if (error) std::rethrow_exception(error); return events;
    }
    void set_fault(std::size_t index, Fault fault) { std::lock_guard lock(mutex); faults[index] = fault; }
    Bytes value(std::uint32_t a) { std::lock_guard lock(mutex); return memory[a]; }
    void handle(const Bytes& b, sockaddr_in peer) {
        CHECK(b.size() >= 12 && b[0] == std::byte{0x42} && b[1] == std::byte{1});
        CHECK(get16(b, 4) == b.size() - 8);
        const auto command = get16(b, 2), id = get16(b, 6);
        const auto address = get32(b, 8), v = b.size() >= 16 ? get32(b, 12) : 0;
        CHECK(id != 0 && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        bool owner = privilege && controller_ip == peer.sin_addr.s_addr && controller_port == peer.sin_port;
        bool accepted = true; Bytes payload;
        if (command == 0x80) {
            CHECK(b.size() == 12);
            if (address == 0xffff0000) payload = word(0x534f0039);
            else if (address == 0xffff0004) payload = word(owner ? 1 : 0);
            else if (address == 0xa00) {
                if (owner) { last_activity = gv::Clock::now(); expires = last_activity + lease; ++refreshes; }
                payload = word(privilege);
            } else { payload = memory[address]; payload.resize(4); }
        } else if (command == 0x82 && address == 0xa00) {
            if (v == 2 && !privilege) {
                controller_ip = peer.sin_addr.s_addr; controller_port = peer.sin_port; privilege = 2;
                last_activity = gv::Clock::now(); expires = last_activity + lease; ++claims;
            } else if (v == 0 && owner) {
                privilege = 0; controller_ip = 0; controller_port = 0; ++releases;
            } else accepted = false;
            payload = word(1);
        } else if (command == 0x82 || command == 0x86) {
            accepted = owner;
            if (accepted) { memory[address] = Bytes(b.begin() + 12, b.end()); ++writes; }
            payload = word(command == 0x82 ? 1 : address);
        } else {
            CHECK(command == 0x84 && v > 0 && v <= 508);
            payload = word(address); auto data = memory[address]; data.resize(v);
            payload.insert(payload.end(), data.begin(), data.end());
        }
        if (!accepted) ++refused;
        const auto index = events.size();
        events.push_back({command, id, address, v, peer.sin_addr.s_addr, peer.sin_port, accepted});
        changed.notify_all();
        auto response = accepted ? ack(command + 1, id, payload) : ack(command + 1, id, {}, 0x8006);
        const auto now = gv::Clock::now();
        auto queue = [&](Bytes bytes, std::chrono::milliseconds delay = 0ms) {
            scheduled.push_back({now + delay, std::move(bytes), peer});
        };
        const auto it = faults.find(index); const auto fault = it == faults.end() ? Fault::good : it->second;
        switch (fault) {
        case Fault::drop: return;
        case Fault::pending:
            for (int i = 0; i < 100; ++i) queue(ack(0x89, id, word(60000)), i * 3ms);
            return;
        case Fault::stale: set16(response, 6, id + 100); queue(response); return;
        case Fault::hostile_then_good: {
            auto bad = response; set16(bad, 6, id + 100); queue(bad);
            bad = response; set16(bad, 2, 0x90); queue(bad); queue(response, 2ms); return;
        }
        case Fault::delay: queue(response, 200ms); return;
        case Fault::good: queue(response); return;
        }
    }
    void run(std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            pollfd event{fd, POLLIN, 0}; CHECK(poll(&event, 1, 1) >= 0);
            std::lock_guard lock(mutex); expire();
            if (event.revents & POLLIN) {
                Bytes b(2048); sockaddr_in peer{}; socklen_t size = sizeof(peer);
                auto n = recvfrom(fd, b.data(), b.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&peer), &size);
                CHECK(n >= 0); b.resize(static_cast<std::size_t>(n)); handle(b, peer);
            }
            for (auto it = scheduled.begin(); it != scheduled.end();) {
                if (it->at > gv::Clock::now()) { ++it; continue; }
                CHECK(sendto(fd, it->bytes.data(), it->bytes.size(), MSG_DONTWAIT,
                    reinterpret_cast<const sockaddr*>(&it->peer), sizeof(it->peer)) == static_cast<ssize_t>(it->bytes.size()));
                it = scheduled.erase(it);
            }
        }
    }
};
std::unique_ptr<gv::ControlOwner> owner(Device& d, gv::HeartbeatPolicy policy = {}) {
    auto r = gv::ControlOwner::create("127.0.0.1", d.port, policy); CHECK(r); return std::move(r.value());
}
void retired(gv::ControlOwner& c, Device& d) {
    const auto before = d.snapshot().size();
    CHECK(!c.write_register(0x100, 2, budget())); CHECK(!c.heartbeat(budget())); CHECK(!c.acquire(budget()));
    c.shutdown(); std::this_thread::sleep_for(4ms); CHECK(d.snapshot().size() == before);
    CHECK(c.state() == gv::OwnerState::Closed);
}
void success() {
    Device d; auto c = owner(d); CHECK(d.snapshot().empty());
    CHECK(c->acquire(budget())); CHECK(c->state() == gv::OwnerState::Owned);
    CHECK(c->heartbeat(budget())); CHECK(c->write_register(0x100, 0x91ab23cd, budget()));
    auto r = c->read_register(0x100, budget()); CHECK(r && r.value() == 0x91ab23cd);
    std::array<std::byte, 336> data{}, output{};
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = std::byte((i * 17 + 9) & 255);
    CHECK(c->write_memory(0x200, data, budget())); CHECK(c->read_memory(0x200, output, budget())); CHECK(output == data);
    CHECK(!c->write_register(0xa00, 0, budget())); CHECK(!c->write_memory(0x9fc, data, budget()));
    CHECK(c->shutdown() == gv::CloseOutcome::released); retired(*c, d);
    auto events = d.snapshot(); CHECK(events.size() == 11);
    for (std::size_t i = 0; i < events.size(); ++i) {
        CHECK(events[i].id == i + 1); CHECK(events[i].port == events[0].port && events[i].ip == events[0].ip);
    }
    CHECK(d.claims == 1 && d.releases == 1 && d.writes == 2 && d.refreshes == 1);
    std::cout << "success: 11 UDP commands, IDs 1..11; acquire/heartbeat/register/memory/release same endpoint PASS\n";
}
void competition() {
    Device d; auto a = owner(d), b = owner(d);
    CHECK(a->acquire(budget())); CHECK(!b->acquire(budget()));
    CHECK(!b->write_register(0x100, 8, budget()));
    auto rogue = gv::Client::create("127.0.0.1", d.port); CHECK(rogue);
    auto r = rogue.value()->write_register(0x100, 9, budget()); CHECK(!r && r.error().device_status == 0x8006);
    CHECK(d.value(0x100).empty()); CHECK(b->shutdown() == gv::CloseOutcome::not_owned);
    CHECK(a->shutdown() == gv::CloseOutcome::released);
    CHECK(d.snapshot().size() == 8 && d.refused == 1 && d.claims == 1 && d.releases == 1);
    // Simultaneous candidates: atomic server-side claim, not a read-then-write lock assumption.
    Device race; auto x = owner(race), y = owner(race);
    std::atomic<int> wins{};
    std::jthread one([&] { if (x->acquire(budget())) ++wins; });
    std::jthread two([&] { if (y->acquire(budget())) ++wins; });
    one.join(); two.join(); CHECK(wins == 1); x->shutdown(); y->shutdown(); CHECK(race.claims == 1);
    std::cout << "competing candidates and independent second socket write refusal PASS\n";
}
void transitions() {
    for (bool release : {false, true}) {
        Device d({{release ? 4U : 2U, Fault::drop}}); auto c = owner(d);
        if (release) CHECK(c->acquire(budget()));
        else {
            auto r = c->acquire(budget()); CHECK(!r && r.error().transmitted);
            CHECK(c->state() == gv::OwnerState::OutcomeUnresolved);
        }
        CHECK(c->shutdown() == gv::CloseOutcome::unresolved); retired(*c, d);
        CHECK(d.snapshot().size() == (release ? 5U : 3U)); CHECK(d.claims == 1);
        CHECK(d.releases == (release ? 1U : 0U));
    }
    Device d; auto c = owner(d); std::stop_source stop; stop.request_stop();
    CHECK(!c->acquire(budget(1s, stop.get_token()))); CHECK(d.snapshot().empty());
    CHECK(c->state() == gv::OwnerState::NotStarted); CHECK(c->shutdown() == gv::CloseOutcome::not_owned);
    CHECK(!gv::ControlOwner::create("192.0.2.1", d.port));
    CHECK(!gv::ControlOwner::create("127.0.0.1", 3956));
    CHECK(!gv::ControlOwner::create("127.0.0.1", d.port, {50ms, 50ms, 10ms, 10ms}));
    Device pending({{2, Fault::pending}}); auto candidate = owner(pending);
    std::optional<gv::Status> acquisition;
    std::jthread acquiring([&] { acquisition.emplace(candidate->acquire(budget())); });
    pending.wait(3); const auto start = gv::Clock::now();
    CHECK(candidate->shutdown() == gv::CloseOutcome::unresolved); acquiring.join();
    CHECK(acquisition && !*acquisition && acquisition->error().transmitted);
    CHECK(gv::Clock::now() - start < 300ms); CHECK(pending.snapshot().size() == 3); retired(*candidate, pending);
    std::cout << "lost acquire/release ACKs: one transition, no retry, unresolved retained; pre-send cancel zero packets PASS\n";
}
void heartbeat_faults() {
    for (auto fault : {Fault::drop, Fault::pending, Fault::stale}) {
        Device d({{4, fault}}); auto c = owner(d); CHECK(c->acquire(budget()));
        auto start = gv::Clock::now(); CHECK(!c->heartbeat(budget()));
        CHECK(gv::Clock::now() - start < 300ms); CHECK(c->state() == gv::OwnerState::OutcomeUnresolved);
        CHECK(!c->write_register(0x100, 9, budget()));
        CHECK(c->shutdown() == gv::CloseOutcome::unresolved); CHECK(d.snapshot().size() == 5);
    }
    Device revoked; auto c = owner(revoked); CHECK(c->acquire(budget())); revoked.revoke();
    CHECK(!c->heartbeat(budget())); CHECK(c->state() == gv::OwnerState::ControlLost);
    CHECK(!c->write_register(0x100, 3, budget())); CHECK(c->shutdown() == gv::CloseOutcome::control_lost);
    CHECK(revoked.snapshot().size() == 6 && revoked.releases == 0);
    std::cout << "heartbeat drop/pending/stale fail closed; identity revoked despite privilege=2; no release to new controller PASS\n";
}
void scheduling() {
    Device d({}, 180ms);
    auto c = owner(d, {180ms, 35ms, 150ms, 30ms}); CHECK(c->acquire(budget()));
    std::this_thread::sleep_for(15ms);
    d.set_fault(4, Fault::pending);
    auto start = gv::Clock::now(); auto r = c->write_register(0x100, 11, budget());
    CHECK(!r && r.error().write_completion == gv::WriteCompletion::uncertain);
    CHECK(gv::Clock::now() - start >= 130ms);
    CHECK(gv::Clock::now() - start < 350ms);
    std::this_thread::sleep_for(40ms);
    CHECK(c->shutdown() == gv::CloseOutcome::unresolved);
    CHECK(d.snapshot().size() == 5 && d.value(0x100) == word(11)); CHECK(d.losses == 1);
    // Scheduler keeps an otherwise idle session alive through several device leases.
    Device idle({}, 180ms); auto a = owner(idle, {180ms, 30ms, 25ms, 25ms});
    CHECK(a->acquire(budget())); std::this_thread::sleep_for(430ms);
    CHECK(a->state() == gv::OwnerState::Owned); CHECK(a->shutdown() == gv::CloseOutcome::released);
    CHECK(idle.refreshes >= 3 && idle.losses == 0);
    std::cout << "ordinary pending write consumed heartbeat budget: lease expired, side effect retained; idle scheduler continuity PASS\n";
}
void concurrency() {
    Device d({{4, Fault::pending}}); auto c = owner(d); CHECK(c->acquire(budget()));
    std::stop_source stop;
    std::optional<gv::Status> result;
    std::jthread writer([&] { result.emplace(c->write_register(0x100, 42, budget(1s, stop.get_token()))); });
    d.wait(5);
    // A waiting call's original deadline expires without receiving a new budget.
    const auto admission = gv::Clock::now();
    auto waiting = c->write_register(0x104, 99, {admission + 1ms, {}});
    CHECK(!waiting && !waiting.error().transmitted && gv::Clock::now() - admission < 100ms);
    std::atomic<int> busy{}; std::vector<std::jthread> callers;
    for (int i = 0; i < 12; ++i) callers.emplace_back([&] {
        auto r = c->write_register(0x104, 99, budget(10ms));
        if (!r && r.error().code == gv::ErrorCode::busy && !r.error().transmitted) ++busy;
    });
    callers.clear(); CHECK(busy == 12); stop.request_stop(); writer.join();
    CHECK(result && !*result && result->error().transmitted && result->error().code == gv::ErrorCode::cancelled);
    CHECK(d.value(0x100) == word(42)); CHECK(c->shutdown() == gv::CloseOutcome::unresolved);
    CHECK(d.snapshot().size() == 5); retired(*c, d);
    // Close cancels both an ordinary command and an automatically scheduled heartbeat.
    for (bool heartbeat : {false, true}) {
        Device server({{4, Fault::pending}}); auto a = owner(server); CHECK(a->acquire(budget()));
        std::optional<gv::Status> write;
        std::jthread in_flight;
        if (!heartbeat) in_flight = std::jthread([&] { write.emplace(a->write_register(0x100, 7, budget())); });
        server.wait(5); const auto start = gv::Clock::now();
        gv::CloseOutcome first{}, second{};
        std::jthread closer([&] { first = a->shutdown(); }); second = a->shutdown(); closer.join();
        if (in_flight.joinable()) in_flight.join();
        CHECK(gv::Clock::now() - start < 300ms);
        CHECK(first == gv::CloseOutcome::unresolved && second == first);
        CHECK(server.snapshot().size() == 5); retired(*a, server);
    }
    std::cout << "12 concurrent busy/no-send; cancel after send retains write; concurrent/repeated shutdown and pending heartbeat bounded PASS\n";
}
void termination_and_hostile() {
    Device d; auto c = owner(d); CHECK(c->acquire(budget())); d.stop();
    const auto start = gv::Clock::now(); CHECK(!c->heartbeat(budget()));
    CHECK(gv::Clock::now() - start < 300ms); CHECK(c->shutdown() == gv::CloseOutcome::unresolved);
    Device hostile({{4, Fault::hostile_then_good}}); auto a = owner(hostile); CHECK(a->acquire(budget()));
    CHECK(a->write_register(0x100, 5, budget())); CHECK(a->shutdown() == gv::CloseOutcome::released);
    CHECK(hostile.snapshot().size() == 6);
    Device delayed({{4, Fault::delay}}); auto b = owner(delayed); CHECK(b->acquire(budget()));
    CHECK(!b->write_register(0x100, 8, budget())); CHECK(b->shutdown() == gv::CloseOutcome::unresolved);
    std::this_thread::sleep_for(210ms); retired(*b, delayed); CHECK(delayed.snapshot().size() == 5);
    std::cout << "unexpected server termination bounded; unknown/stale ignored; delayed ACK cannot revive retired session PASS\n";
}
std::size_t entries(const char* p) {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(p), std::filesystem::directory_iterator{}));
}
void resources_and_exhaustion() {
    const auto fds = entries("/proc/self/fd"), threads = entries("/proc/self/task");
    for (int i = 0; i < 12; ++i) {
        Device d; const auto before = entries("/proc/self/fd");
        auto c = owner(d); CHECK(entries("/proc/self/fd") == before + 1);
        CHECK(c->acquire(budget())); CHECK(c->shutdown() == gv::CloseOutcome::released);
        CHECK(entries("/proc/self/fd") == before); retired(*c, d);
    }
    CHECK(entries("/proc/self/fd") == fds); CHECK(entries("/proc/self/task") == threads);
    Device d({}, 5s); auto c = owner(d, {5s, 1s, 500ms, 100ms}); CHECK(c->acquire(budget()));
    // Exhaust the actual client's allocator over real UDP, including any scheduler IDs.
    bool exhausted = false;
    const auto end = gv::Clock::now() + 45s;
    while (gv::Clock::now() < end) {
        auto r = c->read_register(0x100, budget());
        if (!r && r.error().code == gv::ErrorCode::busy) { std::this_thread::yield(); continue; }
        if (!r) { CHECK(r.error().code == gv::ErrorCode::id_exhausted || c->state() == gv::OwnerState::OutcomeUnresolved); exhausted = true; break; }
    }
    CHECK(exhausted); CHECK(c->shutdown() == gv::CloseOutcome::unresolved);
    auto events = d.snapshot(); CHECK(events.size() == 65535);
    for (std::size_t i = 0; i < events.size(); ++i) {
        CHECK(events[i].id == i + 1 && events[i].port == events[0].port);
    }
    retired(*c, d);
    std::cout << "12 socket/worker lifetimes: fd/thread delta=0; real owned session 65535 commands IDs 1..65535, no reset/socket recreation PASS\n";
}
int main() {
    try {
        success(); competition(); transitions(); heartbeat_faults(); scheduling(); concurrency();
        termination_and_hostile(); resources_and_exhaustion(); return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}