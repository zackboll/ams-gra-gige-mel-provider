// Production-call boundary with native PvResult and actual disconnected device.
// Recording calls prove argument construction, NOT GVCP/camera acceptance.
#include "backends/pleora/pleora_registers.hpp"
#include <PvDeviceGEV.h>

#include <iostream>
#include <stdexcept>

namespace b = gige_mel::backend;
namespace p = b::pleora;
namespace r = b::device_registers;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

struct Calls {
    std::vector<std::string> calls;
    std::int64_t address{}, length{};
    std::uint32_t word{};
    std::vector<unsigned char> bytes;
    bool acknowledge{false};
    PvResult result{PvResult::Code::OK};
    PvResult ReadRegister(std::int64_t a, std::uint32_t& value) {
        calls.push_back("rr"); address = a; value = word; return result;
    }
    PvResult WriteRegister(std::int64_t a, std::uint32_t value, bool ack) {
        calls.push_back("wr"); address = a; word = value; acknowledge = ack; return result;
    }
    PvResult ReadMemory(std::int64_t a, unsigned char* destination, std::int64_t size) {
        calls.push_back("rm"); address = a; length = size;
        destination[0] = 0xff;
        if (result.IsOK()) {
            CHECK(size == static_cast<std::int64_t>(bytes.size()));
            std::copy(bytes.begin(), bytes.end(), destination);
        }
        return result;
    }
    PvResult WriteMemory(std::int64_t a, const unsigned char* source, std::int64_t size) {
        calls.push_back("wm"); address = a; length = size;
        bytes.assign(source, source + size); return result;
    }
};

int main() {
    try {
        Calls native;
        bool closed = false;
        {
            p::Registers registers(native, [&]() -> b::Status {
                if (closed) return b::Error{b::ErrorCode::disconnected, "closed"};
                return {};
            });
            CHECK(native.calls.empty());
            for (const auto value : {0U, 0x812345efU, UINT32_MAX, 0x12345678U}) {
                CHECK(registers.write_register(0xfffffffc, value));
                CHECK(native.address == 0xfffffffc && native.word == value && native.acknowledge);
                auto read = registers.read_register(0xfffffffc);
                CHECK(read && read.value() == value);
            }
            const std::array bytes{std::byte{0}, std::byte{0x81}, std::byte{0x23}, std::byte{0xef},
                                   std::byte{0xfe}, std::byte{0x45}, std::byte{0x67}, std::byte{0x89}};
            CHECK(registers.write_memory(0xfffffff8, bytes));
            CHECK(native.address == 0xfffffff8 && native.length == 8);
            CHECK(native.bytes == std::vector<unsigned char>({0, 0x81, 0x23, 0xef, 0xfe, 0x45, 0x67, 0x89}));
            std::array<std::byte, 8> output{};
            CHECK(registers.read_memory(0xfffffff8, output));
            CHECK(output == bytes);
            auto count = native.calls.size();
            CHECK(!registers.write_register(UINT64_MAX, 1));
            CHECK(!registers.write_register(0x8000000000000000ULL, 1));
            CHECK(!registers.write_register(0x100000000ULL, 1));
            CHECK(!registers.write_memory(0, {}));
            CHECK(!registers.write_memory(1, bytes));
            CHECK(!registers.write_memory(0, std::span(bytes).first(7)));
            CHECK(native.calls.size() == count);
            for (const auto code : {PvResult::Code::TIMEOUT, PvResult::Code::NETWORK_ERROR,
                                    PvResult::Code::NOT_SUPPORTED, PvResult::Code::NOT_CONNECTED}) {
                native.result = PvResult(code, "original native diagnostic");
                const auto expected = p::native_error(native.result, "WriteMemory");
                auto failed = registers.write_memory(0, bytes);
                CHECK(!failed && failed.error().diagnostic.message == expected.message);
                CHECK(failed.error().diagnostic.code == expected.code);
                CHECK(failed.error().write_completion == r::WriteCompletion::uncertain);
                CHECK(native.calls.size() == ++count);
                output.fill(std::byte{0x5a});
                const auto before = output;
                CHECK(!registers.read_memory(0, output));
                CHECK(output == before && native.calls.size() == ++count);
                auto word = registers.read_register(0);
                CHECK(!word && native.calls.size() == ++count);
                auto write = registers.write_register(0, 0xa123ef00);
                CHECK(!write && native.calls.size() == ++count);
                CHECK(native.word == 0xa123ef00); // no rollback
            }
            CHECK(native.calls[count - 4] == "wm" && native.calls[count - 3] == "rm" &&
                  native.calls[count - 2] == "rr" && native.calls[count - 1] == "wr");
            closed = true;
            auto refused = registers.write_register(0, 1);
            CHECK(!refused && refused.error().write_completion == r::WriteCompletion::not_attempted);
            CHECK(native.calls.size() == count);
        }
        CHECK(native.calls.back() == "wr"); // service destruction adds no I/O

        // Genuine installed native API: disconnected device, no network/open.
        PvDeviceGEV device;
        p::Registers registers(device, []() -> b::Status { return {}; });
        CHECK(!registers.read_register(0));
        CHECK(!registers.write_register(0, 0x812345ef));
        std::array<std::byte, 8> output{};
        auto failed = registers.read_memory(0, output);
        CHECK(!failed && failed.error().diagnostic.code == b::ErrorCode::disconnected);
        CHECK(failed.error().diagnostic.message.find("NOT_CONNECTED") != std::string::npos);
        failed = registers.write_memory(0, output);
        CHECK(!failed && failed.error().write_completion == r::WriteCompletion::uncertain);
        std::cout << "native Pleora call-boundary contracts passed (no camera transport)\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
