#pragma once
// Qualification-only kernel checks, before dlopen. No SDK dependencies.
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/nsfs.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace wire_preflight {
struct Fd { int value; ~Fd() { if (value >= 0) ::close(value); } };
inline void require(bool condition, const char* diagnostic) {
    if (!condition) throw std::runtime_error(diagnostic);
}
inline bool same(int a, int b) {
    struct stat x{}, y{};
    require(::fstat(a, &x) == 0 && ::fstat(b, &y) == 0, "namespace stat unavailable");
    return x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
inline void check(int origin) {
    require(::getppid() == 1 && ::getpid() != 1 && ::geteuid() == 0,
            "probe must be a mapped-root direct child of namespace init");
    Fd net{::open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC)};
    Fd user{::open("/proc/self/ns/user", O_RDONLY | O_CLOEXEC)};
    Fd init_net{::open("/proc/1/ns/net", O_RDONLY | O_CLOEXEC)};
    Fd init_user{::open("/proc/1/ns/user", O_RDONLY | O_CLOEXEC)};
    require(::ioctl(origin, NS_GET_NSTYPE) == CLONE_NEWNET &&
            ::ioctl(net.value, NS_GET_NSTYPE) == CLONE_NEWNET &&
            ::ioctl(user.value, NS_GET_NSTYPE) == CLONE_NEWUSER,
            "namespace inspection/outer namespace descriptor unavailable");
    require(!same(origin, net.value) && same(net.value, init_net.value) && same(user.value, init_user.value),
            "outer network namespace or incorrect namespace init");
    Fd net_owner{::ioctl(net.value, NS_GET_USERNS)};
    require(same(net_owner.value, user.value), "network not owned by qualification user namespace");
    Fd pid{::open("/proc/self/ns/pid", O_RDONLY | O_CLOEXEC)};
    require(::ioctl(pid.value, NS_GET_NSTYPE) == CLONE_NEWPID, "PID namespace inspection unavailable");
    Fd pid_owner{::ioctl(pid.value, NS_GET_USERNS)};
    require(same(pid_owner.value, user.value), "PID namespace not owned by qualification user namespace");
    std::ifstream mapping("/proc/self/uid_map");
    unsigned long inside{}, outside{}, count{};
    require(static_cast<bool>(mapping >> inside >> outside >> count) && inside == 0 && count == 1 &&
            !(mapping >> inside), "not a single-user mapped qualification namespace");
    struct if_nameindex* names = ::if_nameindex();
    require(names != nullptr, "interface inspection unavailable");
    const bool approved = names[0].if_index != 0 && std::string(names[0].if_name) == "lo" && names[1].if_index == 0;
    ::if_freenameindex(names);
    require(approved, "only lo is permitted");
    ifaddrs* addresses = nullptr;
    require(::getifaddrs(&addresses) == 0, "address inspection unavailable");
    bool endpoint = false, valid = true;
    for (auto* p = addresses; p; p = p->ifa_next) {
        if (std::string(p->ifa_name) != "lo" || !(p->ifa_flags & IFF_UP) || !(p->ifa_flags & IFF_LOOPBACK)) valid = false;
        if (!p->ifa_addr) continue;
        if (p->ifa_addr->sa_family == AF_INET) {
            sockaddr_in address{}, mask{};
            std::memcpy(&address, p->ifa_addr, sizeof address);
            if (!p->ifa_netmask) { valid = false; continue; }
            std::memcpy(&mask, p->ifa_netmask, sizeof mask);
            if (ntohl(address.sin_addr.s_addr) != 0x7f000001 || ntohl(mask.sin_addr.s_addr) != 0xff000000) valid = false;
            else endpoint = true;
        } else if (p->ifa_addr->sa_family == AF_INET6) {
            sockaddr_in6 address{}, mask{};
            std::memcpy(&address, p->ifa_addr, sizeof address);
            if (!p->ifa_netmask) { valid = false; continue; }
            std::memcpy(&mask, p->ifa_netmask, sizeof mask);
            if (!IN6_IS_ADDR_LOOPBACK(&address.sin6_addr)) valid = false;
            for (auto byte : mask.sin6_addr.s6_addr) if (byte != 255) valid = false;
        }
    }
    ::freeifaddrs(addresses);
    require(valid && endpoint, "unexpected address or inactive/missing 127.0.0.1/8 endpoint");
    // Dump all route tables, including local routes; /proc/net/route alone omits tables.
    Fd routes{::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)};
    require(routes.value >= 0, "route inspection unavailable");
    timeval timeout{2, 0};
    require(::setsockopt(routes.value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == 0, "route timeout unavailable");
    struct { nlmsghdr header; rtmsg body; } request{};
    request.header.nlmsg_len = sizeof request;
    request.header.nlmsg_type = RTM_GETROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = 1;
    request.body.rtm_family = AF_UNSPEC;
    require(::send(routes.value, &request, sizeof request, 0) == sizeof request, "route dump unavailable");
    for (unsigned batch = 0; batch < 128; ++batch) {
        std::array<unsigned char, 32768> bytes{};
        const auto received = ::recv(routes.value, bytes.data(), bytes.size(), MSG_TRUNC);
        require(received > 0 && received <= static_cast<ssize_t>(bytes.size()), "incomplete route dump");
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(received)) {
            nlmsghdr h{};
            require(static_cast<std::size_t>(received) - offset >= sizeof h, "truncated route header");
            std::memcpy(&h, bytes.data() + offset, sizeof h);
            require(h.nlmsg_len >= sizeof h && h.nlmsg_len <= static_cast<std::size_t>(received) - offset &&
                    h.nlmsg_seq == 1 && !(h.nlmsg_flags & NLM_F_DUMP_INTR), "invalid route dump");
            if (h.nlmsg_type == NLMSG_DONE) {
                int error = -1;
                require(h.nlmsg_len == NLMSG_LENGTH(sizeof error), "incomplete route completion");
                std::memcpy(&error, bytes.data() + offset + sizeof h, sizeof error);
                require(error == 0, "failed route completion");
                return;
            }
            require(h.nlmsg_type == RTM_NEWROUTE && h.nlmsg_len >= NLMSG_LENGTH(sizeof(rtmsg)), "route error");
            rtmsg route{};
            std::memcpy(&route, bytes.data() + offset + sizeof h, sizeof route);
            require(route.rtm_family == AF_INET || route.rtm_family == AF_INET6, "unapproved route family");
            unsigned device = 0;
            std::array<unsigned char, 16> destination{};
            for (std::size_t at = NLMSG_LENGTH(sizeof route); at < h.nlmsg_len;) {
                rtattr attribute{};
                require(h.nlmsg_len - at >= sizeof attribute, "truncated route attribute");
                std::memcpy(&attribute, bytes.data() + offset + at, sizeof attribute);
                require(attribute.rta_len >= sizeof attribute && attribute.rta_len <= h.nlmsg_len - at, "invalid route attribute");
                const auto length = attribute.rta_len - sizeof attribute;
                const auto* data = bytes.data() + offset + at + sizeof attribute;
                if (attribute.rta_type == RTA_OIF) {
                    require(length == sizeof device, "invalid route device");
                    std::memcpy(&device, data, sizeof device);
                } else if (attribute.rta_type == RTA_DST) {
                    require(length == (route.rtm_family == AF_INET ? 4U : 16U), "invalid route address");
                    std::memcpy(destination.data(), data, length);
                } else require(attribute.rta_type != RTA_GATEWAY && attribute.rta_type != RTA_MULTIPATH &&
                               attribute.rta_type != RTA_VIA, "external route");
                at += RTA_ALIGN(attribute.rta_len);
            }
            require(device == ::if_nametoindex("lo"), "route outside lo");
            if (route.rtm_family == AF_INET) {
                require((route.rtm_type == RTN_LOCAL || route.rtm_type == RTN_BROADCAST) &&
                        destination[0] == 127 && route.rtm_dst_len >= 8, "unapproved IPv4/default route");
            } else {
                const std::array<unsigned char, 16> loopback{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
                require(route.rtm_type == RTN_LOCAL && route.rtm_dst_len == 128 && destination == loopback,
                        "unapproved IPv6/default route");
            }
            offset += NLMSG_ALIGN(h.nlmsg_len);
        }
    }
    throw std::runtime_error("route dump bound exceeded");
}
} // namespace wire_preflight