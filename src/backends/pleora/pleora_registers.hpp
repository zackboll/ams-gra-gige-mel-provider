#pragma once

// Private production-call boundary, compiled against installed eBUS declarations.
// Template permits argument/result qualification without connecting a camera.
#include "device_registers.hpp"
#include "pleora_features.hpp"

namespace gige_mel::backend::pleora {

template <class NativeDevice> class Registers final : public device_registers::CheckedRegisters {
public:
    Registers(NativeDevice& device, std::function<Status()> check)
        : device_(device), check_(std::move(check)) {}
private:
    device_registers::Status connected() const override {
        if (auto s = check_(); !s) return s.error();
        return {};
    }
    static device_registers::Status outcome(const PvResult& result, std::string_view operation) {
        if (!result.IsOK()) return native_error(result, operation);
        return {};
    }
    device_registers::Result<std::uint32_t> native_read_register(std::uint64_t address) override {
        std::uint32_t value = 0;
        auto result = device_.ReadRegister(static_cast<std::int64_t>(address), value);
        if (auto s = outcome(result, "ReadRegister"); !s) return s.error();
        return value;
    }
    device_registers::Status native_write_register(std::uint64_t address, std::uint32_t value) override {
        return outcome(device_.WriteRegister(static_cast<std::int64_t>(address), value, true), "WriteRegister");
    }
    device_registers::Status native_read_memory(std::uint64_t address, std::span<std::byte> bytes) override {
        return outcome(device_.ReadMemory(static_cast<std::int64_t>(address),
            reinterpret_cast<unsigned char*>(bytes.data()), static_cast<std::int64_t>(bytes.size())), "ReadMemory");
    }
    device_registers::Status native_write_memory(std::uint64_t address, std::span<const std::byte> bytes) override {
        return outcome(device_.WriteMemory(static_cast<std::int64_t>(address),
            reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<std::int64_t>(bytes.size())), "WriteMemory");
    }
    NativeDevice& device_;
    const std::function<Status()> check_;
};

} // namespace gige_mel::backend::pleora
