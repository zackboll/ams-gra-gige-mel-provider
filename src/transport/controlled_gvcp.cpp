#include "controlled_gvcp.hpp"
#include "gvcp_wire.hpp"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gige_mel::controlled_gvcp {
namespace {
bool valid(std::uint32_t address, std::size_t size) {
    return size != 0 && size <= maximum_transfer && size % 4 == 0 && address % 4 == 0 &&
           size - 1 <= UINT32_MAX - address;
}
std::optional<ErrorCode> interrupted(TransactionOptions options) {
    if (options.stop.stop_requested()) return ErrorCode::cancelled;
    if (Clock::now() >= options.deadline) return ErrorCode::timeout;
    return {};
}
}
Result<std::unique_ptr<Client>> Client::create(const std::string& ipv4, std::uint16_t port) {
    return bind_loopback(ipv4, port, 0);
}
Result<std::unique_ptr<Client>> Client::bind_loopback(const std::string& ipv4, std::uint16_t port,
                                                      std::uint16_t local_port) {
    in_addr ip{};
    if (inet_pton(AF_INET, ipv4.c_str(), &ip) != 1 || (ntohl(ip.s_addr) >> 24) != 127 ||
        port == 0 || port == 3956) return Failure{ErrorCode::invalid_argument};
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return Failure{ErrorCode::io};
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(local_port);
    if (bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        close(fd); return Failure{ErrorCode::io};
    }
    try { return std::unique_ptr<Client>(new Client(fd, ip.s_addr, port)); }
    catch (...) { close(fd); throw; }
}
Client::~Client() { close(fd_); }

Result<std::vector<std::byte>> Client::transact(std::uint16_t command, std::uint32_t address,
        std::size_t size, std::span<const std::byte> source, TransactionOptions options) {
    const bool writing = command == wire::write_memory || command == wire::write_register;
    bool transmitted = false;
    auto fail = [&](ErrorCode code, std::uint16_t status = 0) -> Result<std::vector<std::byte>> {
        return Failure{code, transmitted, writing && transmitted ? WriteCompletion::uncertain :
                        WriteCompletion::not_attempted, status};
    };
    if (options.deadline == Clock::time_point::max() || !valid(address, size))
        return fail(ErrorCode::invalid_argument);
    if (auto error = interrupted(options)) return fail(*error);
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return fail(ErrorCode::busy);
    if (auto error = interrupted(options)) return fail(*error);
    const auto allocated_id = ids_.take();
    if (!allocated_id) return fail(ErrorCode::id_exhausted);
    const auto id = *allocated_id;
    const auto payload = command == wire::read_register ? 4 :
                         command == wire::write_memory ? 4 + size : 8;
    std::vector<std::byte> packet(8 + payload);
    // Allocate all result storage before send: an allocation exception must not
    // conceal an already-transmitted write or partially publish a memory read.
    std::vector<std::byte> response(command == wire::read_memory ? size : 4);
    packet[0] = std::byte{0x42}; packet[1] = std::byte{1};
    wire::put16(packet, 2, command);
    wire::put16(packet, 4, static_cast<std::uint16_t>(payload));
    wire::put16(packet, 6, id); wire::put32(packet, 8, address);
    if (writing) std::copy(source.begin(), source.end(), packet.begin() + 12);
    else if (command == wire::read_memory) wire::put32(packet, 12, static_cast<std::uint32_t>(size));
    sockaddr_in remote{};
    remote.sin_family = AF_INET; remote.sin_addr.s_addr = ip_; remote.sin_port = htons(port_);
    if (auto error = interrupted(options)) return fail(*error);
    // Nonblocking UDP: exactly ONE send syscall. EAGAIN/EINTR are errors, not retries.
    const auto sent = sendto(fd_, packet.data(), packet.size(), MSG_DONTWAIT,
                            reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
    if (sent < 0) return fail(ErrorCode::io);
    transmitted = true;
    if (static_cast<std::size_t>(sent) != packet.size()) return fail(ErrorCode::io);
    for (;;) {
        if (auto error = interrupted(options)) return fail(*error);
        // At most 2 ms user-space polling quantum; round remaining time upward
        // only to the next millisecond, then recheck BEFORE accepting any ACK.
        const auto remaining = options.deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) return fail(ErrorCode::timeout);
        const auto wait = std::min(std::chrono::ceil<std::chrono::milliseconds>(remaining),
                                   std::chrono::milliseconds{2});
        pollfd event{fd_, POLLIN, 0};
        const int ready = poll(&event, 1, static_cast<int>(wait.count()));
        if (ready < 0) { if (errno == EINTR) continue; return fail(ErrorCode::io); }
        if (auto error = interrupted(options)) return fail(*error);
        if (ready == 0) continue;
        if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) return fail(ErrorCode::io);
        std::array<std::byte, 520> incoming{};
        sockaddr_in sender{};
        socklen_t length = sizeof(sender);
        const auto received = recvfrom(fd_, incoming.data(), incoming.size(), MSG_DONTWAIT | MSG_TRUNC,
                                      reinterpret_cast<sockaddr*>(&sender), &length);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return fail(ErrorCode::io);
        }
        if (auto error = interrupted(options)) return fail(*error);
        if (length != sizeof(sender) || sender.sin_family != AF_INET ||
            sender.sin_addr.s_addr != ip_ || sender.sin_port != htons(port_) ||
            static_cast<std::size_t>(received) > incoming.size()) continue;
        const auto bytes = std::span(incoming).first(static_cast<std::size_t>(received));
        const auto parsed = wire::parse(bytes, id, command, address, size);
        if (auto error = interrupted(options)) return fail(*error);
        if (parsed.kind == wire::Kind::device_error) return fail(ErrorCode::device, parsed.status);
        if (parsed.kind != wire::Kind::success) continue;
        std::copy(bytes.begin() + (command == wire::read_memory ? 12 : 8), bytes.end(), response.begin());
        return Result<std::vector<std::byte>>(std::move(response));
    }
}
Result<std::uint32_t> Client::read_register(std::uint32_t address, TransactionOptions options) {
    auto result = transact(wire::read_register, address, 4, {}, options);
    if (!result) return result.error();
    return wire::u32(result.value(), 0);
}
Status Client::write_register(std::uint32_t address, std::uint32_t value, TransactionOptions options) {
    std::array<std::byte, 4> bytes{}; wire::put32(bytes, 0, value);
    auto result = transact(wire::write_register, address, 4, bytes, options);
    if (!result) return result.error();
    return Receipt{true, WriteCompletion::acknowledged};
}
Status Client::read_memory(std::uint32_t address, std::span<std::byte> bytes, TransactionOptions options) {
    if (!bytes.data()) return Failure{ErrorCode::invalid_argument};
    auto result = transact(wire::read_memory, address, bytes.size(), {}, options);
    if (!result) return result.error();
    std::copy(result.value().begin(), result.value().end(), bytes.begin());
    return Receipt{};
}
Status Client::write_memory(std::uint32_t address, std::span<const std::byte> bytes, TransactionOptions options) {
    if (!bytes.data()) return Failure{ErrorCode::invalid_argument};
    auto result = transact(wire::write_memory, address, bytes.size(), bytes, options);
    if (!result) return result.error();
    return Receipt{true, WriteCompletion::acknowledged};
}
} // namespace gige_mel::controlled_gvcp
