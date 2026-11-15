#pragma once

// Experimental, uninstalled, SDK-independent; not IRegisterDevice or profile ABI.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace gige_mel::controlled_gvcp {
using Clock = std::chrono::steady_clock;
struct TransactionOptions {
    Clock::time_point deadline;
    std::stop_token stop{};
};
enum class ErrorCode { invalid_argument, busy, timeout, cancelled, io, device, id_exhausted };
enum class WriteCompletion { not_attempted, acknowledged, uncertain };
struct Failure {
    ErrorCode code;
    bool transmitted{}; // Successful UDP send, NOT proof of device receipt.
    WriteCompletion write_completion{WriteCompletion::not_attempted};
    std::uint16_t device_status{}; // Exact known negative GVCP status, otherwise zero.
};
template<class T> class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Failure error) : data_(error) {}
    explicit operator bool() const { return std::holds_alternative<T>(data_); }
    T& value() { return std::get<T>(data_); }
    const T& value() const { return std::get<T>(data_); }
    const Failure& error() const { return std::get<Failure>(data_); }
private:
    std::variant<T, Failure> data_;
};
struct Receipt {
    bool transmitted{true};
    WriteCompletion write_completion{WriteCompletion::not_attempted};
};
using Status = Result<Receipt>;
inline constexpr std::size_t maximum_transfer = 508;

namespace detail {
class RequestIds {
public:
    std::optional<std::uint16_t> take() {
        if (next_ > UINT16_MAX) return {};
        return static_cast<std::uint16_t>(next_++);
    }
private:
    std::uint32_t next_{1};
};
}

class IControlledRegisters {
public:
    virtual ~IControlledRegisters() = default;
    virtual Result<std::uint32_t> read_register(std::uint32_t, TransactionOptions) = 0;
    virtual Status write_register(std::uint32_t, std::uint32_t, TransactionOptions) = 0;
    virtual Status read_memory(std::uint32_t, std::span<std::byte>, TransactionOptions) = 0;
    virtual Status write_memory(std::uint32_t, std::span<const std::byte>, TransactionOptions) = 0;
};

class Client final : public IControlledRegisters {
public:
    // Numeric IPv4 only. Rejects non-127/8, port zero, and the old simulator's
    // fixed control port 3956. No DNS, discovery, control or heartbeat operations.
    static Result<std::unique_ptr<Client>> create(const std::string& ipv4, std::uint16_t port);
    ~Client() override;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Result<std::uint32_t> read_register(std::uint32_t, TransactionOptions) override;
    Status write_register(std::uint32_t, std::uint32_t, TransactionOptions) override;
    Status read_memory(std::uint32_t, std::span<std::byte>, TransactionOptions) override;
    Status write_memory(std::uint32_t, std::span<const std::byte>, TransactionOptions) override;
private:
    Client(int fd, std::uint32_t ip, std::uint16_t port) : fd_(fd), ip_(ip), port_(port) {}
    Result<std::vector<std::byte>> transact(std::uint16_t command, std::uint32_t address,
        std::size_t size, std::span<const std::byte> source, TransactionOptions);
    int fd_;
    std::uint32_t ip_; // network byte order
    std::uint16_t port_; // host byte order
    std::mutex mutex_;
    detail::RequestIds ids_; // Exhausts, never wraps. Join callers before destruction.
};
} // namespace gige_mel::controlled_gvcp
