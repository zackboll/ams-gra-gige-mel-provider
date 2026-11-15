#pragma once

// Internal C++20 transport, never installed and not part of profile module v1.
#include <gige_mel/backend.hpp>

#include <algorithm>
#include <array>
#include <functional>

namespace gige_mel::backend::device_registers {

enum class WriteCompletion { not_attempted, uncertain };
struct Failure {
    Error diagnostic;
    WriteCompletion write_completion{WriteCompletion::not_attempted};
};

template <class T> class Result {
public:
    Result(T value) : value_(std::in_place_index<0>, std::move(value)) {}
    Result(Failure error) : value_(std::in_place_index<1>, std::move(error)) {}
    Result(Error error) : Result(Failure{std::move(error)}) {}
    explicit operator bool() const noexcept { return value_.index() == 0; }
    T& value() { return std::get<0>(value_); }
    const T& value() const { return std::get<0>(value_); }
    const Failure& error() const { return std::get<1>(value_); }
private:
    std::variant<T, Failure> value_;
};

template <> class Result<void> {
public:
    Result() = default;
    Result(Failure error) : error_(std::move(error)) {}
    Result(Error error) : Result(Failure{std::move(error)}) {}
    explicit operator bool() const noexcept { return !error_; }
    const Failure& error() const { return error_.value(); }
private:
    std::optional<Failure> error_;
};
using Status = Result<void>;
inline constexpr std::size_t maximum_transfer = 4096;
inline constexpr std::uint64_t maximum_address = UINT32_MAX;

class IDeviceRegisters {
public:
    virtual ~IDeviceRegisters() = default;
    virtual Result<std::uint32_t> read_register(std::uint64_t address) = 0;
    virtual Status write_register(std::uint64_t address, std::uint32_t value) = 0;
    virtual Status read_memory(std::uint64_t address, std::span<std::byte> destination) = 0;
    virtual Status write_memory(std::uint64_t address, std::span<const std::byte> source) = 0;
};

// Discover with dynamic_cast on an explicitly opened device. The callback is
// synchronous, non-reentrant and holds the existing device operation mutex for
// its ENTIRE duration. Borrowed service references must not escape the callback.
// Keep the owning ICameraDevice alive through return. No implicit I/O on discovery.
class IRegisterDevice {
public:
    virtual ~IRegisterDevice() = default;
    virtual Status with_registers(const std::function<Status(IDeviceRegisters&)>&) = 0;
};

// Adapter calls this only under its device lock. connected() is checked afresh
// before EVERY native operation, not just at transaction admission. No retries,
// rollback, byte swapping, feature cache access or acquisition changes here.
class CheckedRegisters : public IDeviceRegisters {
public:
    Result<std::uint32_t> read_register(std::uint64_t address) final {
        if (auto s = validate(address, 4); !s) return s.error();
        if (auto s = connected(); !s) return s.error();
        return native_read_register(address);
    }
    Status write_register(std::uint64_t address, std::uint32_t value) final {
        if (auto s = validate(address, 4); !s) return s;
        if (auto s = connected(); !s) return s;
        return attempted(native_write_register(address, value));
    }
    Status read_memory(std::uint64_t address, std::span<std::byte> destination) final {
        if (auto s = validate(address, destination.size()); !s) return s;
        if (!destination.data()) return invalid("null destination storage");
        if (auto s = connected(); !s) return s;
        std::array<std::byte, maximum_transfer> temporary{};
        auto bytes = std::span(temporary).first(destination.size());
        if (auto s = native_read_memory(address, bytes); !s) return s;
        std::copy(bytes.begin(), bytes.end(), destination.begin());
        return {};
    }
    Status write_memory(std::uint64_t address, std::span<const std::byte> source) final {
        if (auto s = validate(address, source.size()); !s) return s;
        if (!source.data()) return invalid("null source storage");
        if (auto s = connected(); !s) return s;
        return attempted(native_write_memory(address, source));
    }
protected:
    virtual Status connected() const = 0;
    virtual Result<std::uint32_t> native_read_register(std::uint64_t) = 0;
    virtual Status native_write_register(std::uint64_t, std::uint32_t) = 0;
    virtual Status native_read_memory(std::uint64_t, std::span<std::byte>) = 0;
    virtual Status native_write_memory(std::uint64_t, std::span<const std::byte>) = 0;
private:
    static Status invalid(std::string message) {
        return Error{ErrorCode::invalid_argument, "register transport: " + std::move(message)};
    }
    static Status validate(std::uint64_t address, std::size_t size) {
        if (size == 0 || size > maximum_transfer) return invalid("length must be 1..4096 bytes");
        if (address > UINT64_MAX - (size - 1)) return invalid("address arithmetic overflow");
        if (address > maximum_address || size - 1 > maximum_address - address)
            return invalid("transfer exceeds the qualified 32-bit GigE Vision address window");
        if (address % 4 != 0 || size % 4 != 0) return invalid("address and length must be multiples of four");
        return {};
    }
    static Status attempted(Status result) {
        if (result) return result;
        return Failure{result.error().diagnostic, WriteCompletion::uncertain};
    }
};

} // namespace gige_mel::backend::device_registers
