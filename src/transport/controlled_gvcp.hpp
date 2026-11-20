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
/// Per-call deadline and cooperative cancellation, not a hard real-time bound.
struct TransactionOptions {
    Clock::time_point deadline; ///< Absolute steady-clock limit; max() is not admitted.
    std::stop_token stop{};     ///< Cancellation cannot recall a transmitted request.
};
enum class ErrorCode { invalid_argument, busy, timeout, cancelled, io, device, id_exhausted };
/// Evidence about a write, not detector-level processing or durable storage.
enum class WriteCompletion { not_attempted, acknowledged, uncertain };
/// Error with send evidence. A failed transmitted write is uncertain even on
/// a correlated negative ACK; pre-send refusal does not enter the remote path.
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
/// Successful memory read (no write) or positively acknowledged write.
struct Receipt {
    bool transmitted{true};
    WriteCompletion write_completion{WriteCompletion::not_attempted};
};
using Status = Result<Receipt>;
/// Experiment's memory data limit, distinct from the native 4096-byte service.
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

/// @brief Experimental loopback register/memory operations, not SDK access.
/// The entire range must fit the 32-bit window; addresses and lengths are
/// multiples of four. Memory spans must contain 4–508 live bytes. Refuse invalid
/// operations without transmission; no padding, splitting or automatic replay.
/// Numeric register words are converted to/from network order; memory bytes
/// are not converted. Each operation takes an absolute deadline and a stop token.
class IControlledRegisters {
public:
    virtual ~IControlledRegisters() = default;
    /// Return a decoded numeric register from a matching successful ACK.
    virtual Result<std::uint32_t> read_register(std::uint32_t, TransactionOptions) = 0;
    /// Return an acknowledged receipt or failure with write completion evidence.
    virtual Status write_register(std::uint32_t, std::uint32_t, TransactionOptions) = 0;
    /// Copy exact bytes to the caller's destination only after complete success.
    virtual Status read_memory(std::uint32_t, std::span<std::byte>, TransactionOptions) = 0;
    /// Write exact source bytes; an ACK is not proof of application acceptance.
    virtual Status write_memory(std::uint32_t, std::span<const std::byte>, TransactionOptions) = 0;
};

/// @brief One-socket, one-in-flight controlled transport for loopback tests.
/// A competing transaction returns busy without queuing. The socket allocates
/// request IDs from 1 through 65535, then refuses further sends without wrapping
/// or reusing IDs. At most one send per admitted command; no retry or deadline extension
/// for a pending ACK. Correlate sender, ID, expected ACK kind and exact shape,
/// including echoed address for successful memory ACKs (register ACKs have no
/// address). Discard malformed, stale and unmatched replies. This is not
/// authentication or cross-socket stale-reply fencing. Join callers before
/// destruction. Not an acquisition transport or qualified for SDK coexistence.
class Client final : public IControlledRegisters {
public:
    /// @brief Create a local UDP client without sending or acquiring control.
    /// @param[in] ipv4 Numeric IPv4 in 127/8 only; no DNS or discovery.
    /// @param[in] port Explicit destination port, excluding zero and the old
    /// simulator's fixed control port 3956.
    /// @return Owned client with an ephemeral loopback socket, or invalid_argument
    /// or socket I/O failure. Allocation can throw; the socket is then closed.
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
