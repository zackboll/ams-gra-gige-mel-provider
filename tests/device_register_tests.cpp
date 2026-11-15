#include "device_registers.hpp"

#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace b = gige_mel::backend;
namespace r = b::device_registers;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

// Deliberately not invariant under word or double-word swapping.
constexpr std::array payload{std::byte{0x00}, std::byte{0x81}, std::byte{0x23}, std::byte{0xef},
                             std::byte{0xfe}, std::byte{0x45}, std::byte{0x67}, std::byte{0x89},
                             std::byte{0x12}, std::byte{0x34}, std::byte{0xab}, std::byte{0xcd},
                             std::byte{0x55}, std::byte{0xaa}, std::byte{0x00}, std::byte{0xff}};

class Recording final : public r::CheckedRegisters {
public:
    bool closed{false}, lost{false}, fail{false};
    std::uint32_t word{};
    std::vector<std::byte> memory;
    std::vector<std::string> calls;
    std::size_t* destruction_count{nullptr};
    ~Recording() override { if (destruction_count) *destruction_count = calls.size(); }
    b::Error diagnostic{b::ErrorCode::timeout, "native domain=recording code=73: lost ACK"};
private:
    r::Status connected() const override {
        if (closed || lost) return b::Error{b::ErrorCode::disconnected, "closed or lost"};
        return {};
    }
    r::Result<std::uint32_t> native_read_register(std::uint64_t address) override {
        calls.push_back("rr:" + std::to_string(address));
        if (fail) return diagnostic;
        return word;
    }
    r::Status native_write_register(std::uint64_t address, std::uint32_t value) override {
        calls.push_back("wr:" + std::to_string(address));
        word = value; // side effect BEFORE an uncertain result
        if (fail) return diagnostic;
        return {};
    }
    r::Status native_read_memory(std::uint64_t address, std::span<std::byte> bytes) override {
        calls.push_back("rm:" + std::to_string(address));
        bytes.front() = std::byte{0x99}; // failed partial output must never escape
        if (fail) return diagnostic;
        CHECK(bytes.size() == memory.size());
        std::copy(memory.begin(), memory.end(), bytes.begin());
        return {};
    }
    r::Status native_write_memory(std::uint64_t address, std::span<const std::byte> bytes) override {
        calls.push_back("wm:" + std::to_string(address));
        memory.assign(bytes.begin(), bytes.end());
        if (fail) return diagnostic;
        return {};
    }
};

void semantics() {
    std::size_t final_calls = 99;
    {
        Recording passive;
        passive.destruction_count = &final_calls;
    }
    CHECK(final_calls == 0);
    Recording registers;
    CHECK(registers.calls.empty()); // construction/selection/destruction are passive
    for (const auto value : {0U, 0x812345efU, 0xffffffffU, 0x12345678U}) {
        CHECK(registers.write_register(0xfffffffc, value));
        auto read = registers.read_register(0xfffffffc);
        CHECK(read && read.value() == value);
    }
    CHECK(registers.write_memory(0x100, payload));
    std::array<std::byte, payload.size()> output{};
    CHECK(registers.read_memory(0x100, output));
    CHECK(output == payload);
    auto count = registers.calls.size();
    const auto refused = [&](r::Status status) {
        CHECK(!status);
        CHECK(status.error().diagnostic.code == b::ErrorCode::invalid_argument);
        CHECK(status.error().write_completion == r::WriteCompletion::not_attempted);
        CHECK(registers.calls.size() == count);
    };
    refused(registers.write_memory(0, {}));
    refused(registers.read_memory(0, {}));
    std::array<std::byte, 4100> large{};
    refused(registers.write_memory(0, large));
    refused(registers.read_memory(0, large));
    refused(registers.write_memory(UINT64_MAX - 3, payload));
    refused(registers.read_memory(UINT64_MAX - 3, output));
    refused(registers.write_register(UINT64_MAX, 1));
    refused(registers.write_register(0x100000000ULL, 1));
    refused(registers.write_memory(0xfffffff8, payload));
    refused(registers.write_memory(1, payload));
    refused(registers.write_memory(0, std::span(payload).first(7)));
    refused(registers.read_memory(2, output));
    refused(registers.read_memory(0, std::span(output).first(3)));
    refused(registers.write_register(2, 3));
    CHECK(!registers.read_register(1));
    CHECK(registers.calls.size() == count);
    std::array<std::byte, 4096> maximum{};
    maximum[4095] = std::byte{0xe7};
    CHECK(registers.write_memory(0xfffff000, maximum));
    std::array<std::byte, 4096> back{};
    CHECK(registers.read_memory(0xfffff000, back));
    CHECK(back == maximum);

    registers.fail = true;
    count = registers.calls.size();
    auto failed = registers.write_memory(0, payload);
    CHECK(!failed && failed.error().write_completion == r::WriteCompletion::uncertain);
    CHECK(failed.error().diagnostic.code == b::ErrorCode::timeout);
    CHECK(failed.error().diagnostic.message == registers.diagnostic.message);
    CHECK(registers.calls.size() == ++count); // no retry or rollback
    CHECK(std::equal(payload.begin(), payload.end(), registers.memory.begin()));
    failed = registers.write_register(0, 0x91abcdef);
    CHECK(!failed && registers.word == 0x91abcdef);
    CHECK(registers.calls.size() == ++count);
    CHECK(failed.error().write_completion == r::WriteCompletion::uncertain);
    auto failed_word = registers.read_register(0);
    CHECK(!failed_word && failed_word.error().diagnostic.message == registers.diagnostic.message);
    CHECK(registers.calls.size() == ++count);
    output.fill(std::byte{0x5a});
    const auto before = output;
    CHECK(!registers.read_memory(0, output));
    CHECK(output == before && registers.calls.size() == ++count);
    registers.closed = true;
    failed = registers.write_memory(0, payload);
    CHECK(!failed && failed.error().write_completion == r::WriteCompletion::not_attempted);
    CHECK(!registers.read_register(0));
    CHECK(!registers.read_memory(0, output));
    CHECK(!registers.write_register(0, 1));
    CHECK(registers.calls.size() == count);
    registers.closed = false;
    registers.lost = true;
    CHECK(!registers.write_register(0, 1));
    CHECK(registers.calls.size() == count);
}

// Recording transaction owner implements the same lock/borrow pattern as both
// adapters. Actual native lifetime/close is separately tested with Aravis Fake.
void concurrency() {
    struct Owner final : r::IRegisterDevice {
        std::mutex mutex;
        Recording registers;
        r::Status with_registers(const std::function<r::Status(r::IDeviceRegisters&)>& fn) override {
            std::lock_guard lock(mutex);
            if (registers.closed) return b::Error{b::ErrorCode::disconnected, "closed"};
            return fn(registers);
        }
        void close() { std::lock_guard lock(mutex); registers.closed = true; }
    } owner;
    std::promise<void> entered, release, closing;
    auto gate = release.get_future();
    auto io = std::async(std::launch::async, [&] {
        return owner.with_registers([&](r::IDeviceRegisters& service) -> r::Status {
            CHECK(service.write_register(0, 0x812345ef));
            entered.set_value();
            gate.wait();
            auto word = service.read_register(0);
            CHECK(word && word.value() == 0x812345ef);
            return {};
        });
    });
    entered.get_future().wait();
    auto close = std::async(std::launch::async, [&] { closing.set_value(); owner.close(); });
    closing.get_future().wait();
    const auto waited = close.wait_for(20ms);
    release.set_value();
    CHECK(io.get());
    close.get();
    CHECK(waited == std::future_status::timeout);
    CHECK(!owner.with_registers([](auto&) -> r::Status { throw std::runtime_error("called after close"); }));
    CHECK(owner.registers.calls == std::vector<std::string>({"wr:0", "rr:0"}));
}

int main() {
    try { semantics(); concurrency(); std::cout << "register service contracts passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}