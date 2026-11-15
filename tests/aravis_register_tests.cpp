// Actual pinned Fake raw APIs, not recording replacements or camera hardware.
#include "aravis_fake_support.hpp"
#include "device_registers.hpp"

#include <future>
#include <iostream>
#include <stdexcept>

namespace b = gige_mel::backend;
namespace r = b::device_registers;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

int main() {
    try {
        auto backend = aravis_fake::make_backend();
        auto opened = backend->open(aravis_fake::device_id);
        CHECK(opened);
        auto device = opened.value();
        auto* service = dynamic_cast<r::IRegisterDevice*>(device.get());
        CHECK(service);
        CHECK(!service->with_registers({}));
        bool unwound = false;
        try {
            (void)service->with_registers([](auto&) -> r::Status { throw std::runtime_error("callback unwind"); });
        } catch (const std::runtime_error&) { unwound = true; }
        CHECK(unwound); // following transaction proves the native lock released
        // Unused Fake RAM, not a sensor-specific production register layout.
        constexpr std::uint64_t address = 0x800;
        CHECK(service->with_registers([&](r::IDeviceRegisters& registers) -> r::Status {
            for (auto value : {0U, 0x812345efU, UINT32_MAX, 0x12345678U}) {
                CHECK(registers.write_register(address, value));
                auto read = registers.read_register(address);
                CHECK(read && read.value() == value);
            }
            const std::array bytes{std::byte{0}, std::byte{0x81}, std::byte{0x23}, std::byte{0xef},
                                   std::byte{0xfe}, std::byte{0x45}, std::byte{0x67}, std::byte{0x89}};
            CHECK(registers.write_memory(address, bytes));
            std::array<std::byte, 8> output{};
            CHECK(registers.read_memory(address, output));
            CHECK(bytes == output);
            CHECK(!registers.write_memory(address + 1, bytes));
            CHECK(!registers.write_memory(address, std::span(bytes).first(3)));
            CHECK(!registers.write_memory(UINT64_MAX - 3, bytes));
            CHECK(!registers.read_register(0x100000000ULL));
            CHECK(!registers.read_memory(address, {}));
            std::array<std::byte, 4100> large{};
            CHECK(!registers.read_memory(address, large));
            // Native Fake read-only XML region: actual raw write failure. Fake
            // returns FALSE without GError; adapter must not fabricate success.
            auto failed = registers.write_memory(0x10000, bytes);
            CHECK(!failed);
            CHECK(failed.error().write_completion == r::WriteCompletion::uncertain);
            CHECK(failed.error().diagnostic.message.find("Aravis write_memory") != std::string::npos);
            CHECK(registers.read_memory(address, output)); // device survives failure
            CHECK(output == bytes);
            return {};
        }));
        std::promise<void> entered, release, closing;
        auto gate = release.get_future();
        auto io = std::async(std::launch::async, [&] {
            return service->with_registers([&](r::IDeviceRegisters& registers) -> r::Status {
                entered.set_value();
                gate.wait();
                CHECK(registers.write_register(address, 0xa12300ef));
                auto read = registers.read_register(address);
                CHECK(read && read.value() == 0xa12300ef);
                return {};
            });
        });
        entered.get_future().wait();
        auto close = std::async(std::launch::async, [&] { closing.set_value(); return device->close(); });
        closing.get_future().wait();
        const auto waited = close.wait_for(20ms);
        release.set_value();
        CHECK(io.get());
        CHECK(close.get());
        CHECK(waited == std::future_status::timeout);
        CHECK(!service->with_registers([](auto&) -> r::Status { throw std::runtime_error("closed callback"); }));
        // Independent device: confirmed asynchronous control loss gates service.
        device.reset();
        opened.value().reset();
        auto lost = backend->open(aravis_fake::device_id);
        CHECK(lost);
        CHECK(b::aravis::with_native_device_for_testing(*lost.value(), [](ArvDevice* native) {
            g_signal_emit_by_name(native, "control-lost");
        }));
        auto* lost_service = dynamic_cast<r::IRegisterDevice*>(lost.value().get());
        CHECK(lost_service);
        CHECK(!lost_service->with_registers([](auto&) -> r::Status { throw std::runtime_error("lost callback"); }));
        std::cout << "native Aravis register contracts passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
