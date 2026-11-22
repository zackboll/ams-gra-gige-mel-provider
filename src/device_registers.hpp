#pragma once

// Internal C++20 transport, never installed and not part of profile module v1.
#include <gige_mel/backend.hpp>

#include <algorithm>
#include <array>
#include <functional>

namespace gige_mel::backend::device_registers {

/// Write classification on failure only, for this operation (not the callback).
enum class WriteCompletion {
    not_attempted, ///< Refused before the adapter write path, or not a write.
    uncertain      ///< Adapter write path failed; remote effect is not established.
};

/// Preserve native diagnostic provenance separately from write completion.
struct Failure {
    Error diagnostic;
    WriteCompletion write_completion{WriteCompletion::not_attempted};
};

/// Register-local value or Failure; test success before selecting the alternative.
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

/// Register-local success without a value, or Failure.
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
/// Completion of a service operation, not sensor processing/acceptance.
using Status = Result<void>;
inline constexpr std::size_t maximum_transfer = 4096;
inline constexpr std::uint64_t maximum_address = UINT32_MAX;

/// @brief Borrowed raw register/memory service, internal and outside module v1.
/// The whole transfer must fit the 32-bit address window. Register and memory
/// addresses and memory lengths must be multiples of four; memory length is
/// nonzero and at most 4096 bytes. Each call is an application-level operation,
/// not a promise about SDK transaction splitting, physical transmissions, SDK retry
/// limits or finite in-flight execution deadlines. Raw writes can change device
/// state outside GenApi policy, including settings the SDK has cached.
class IDeviceRegisters {
public:
    virtual ~IDeviceRegisters() = default;
    /// Read one numeric 32-bit register, or return its access diagnostic.
    virtual Result<std::uint32_t> read_register(std::uint64_t address) = 0;
    /// Set one numeric 32-bit register. Failure after entering the adapter write
    /// path is conservatively classified as uncertain.
    virtual Status write_register(std::uint64_t address, std::uint32_t value) = 0;
    /// Read exact bytes; update the destination only on success. Caller storage
    /// must remain valid and writable for the call.
    virtual Status read_memory(std::uint64_t address, std::span<std::byte> destination) = 0;
    /// @brief Write the supplied bytes as-is, without padding or byte swapping.
    /// @param[in] address Four-byte-aligned start of a range in the 32-bit window.
    /// @param[in] source Live readable span, 4–4096 bytes in multiples of four.
    /// Host storage need not be word-aligned.
    /// @return Service success, or diagnostic with not_attempted before the
    /// adapter write path and uncertain if that path fails. No rollback or readback.
    /// @warning A failed write can still have changed the remote device.
    virtual Status write_memory(std::uint64_t address, std::span<const std::byte> source) = 0;
};

/// Optional internal device capability, discovered by dynamic_cast after
/// explicit open. Discovery itself performs no I/O.
class IRegisterDevice {
public:
    virtual ~IRegisterDevice() = default;
    /// @brief Invoke a callback synchronously under the device operation mutex.
    /// Supply a nonempty callable. The borrowed register reference cannot escape.
    /// @pre Caller keeps the owning device alive through return. Do not reenter
    /// provider/device operations or wait for work that needs those locks.
    /// @return Admission failure (e.g. closed/disconnected), or the callback's
    /// status. Propagate individual operation failures in the callback; this
    /// is serialization, not atomicity. Callback exceptions may propagate.
    /// @note The mutex covers the whole invocation. The checked service tests
    /// current connection before every native operation, not just at admission.
    virtual Status with_registers(const std::function<Status(IDeviceRegisters&)>&) = 0;
};

/// Shared validation and error staging for adapter-owned register services.
/// Use only under the adapter operation lock. Validate arguments, then check
/// connection afresh before each native entry; memory reads stage output.
/// Does not retry, roll back, swap bytes, touch the feature cache or change
/// acquisition itself. Native failures retain the original diagnostic; even a
/// refusal within the adapter write hook is conservatively marked uncertain.
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
