// TEST-ONLY loopback ArvGvFakeCamera fixture (GIGE-MEL-011). See the header.
#include "aravis_gv_loopback.hpp"

#include "aravis_backend.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace aravis_gv_loopback {
namespace {

// Pinned register addresses (src/arvgvcpprivate.h, src/arvfakecamera.h).
constexpr guint32 gvbs_current_ip = 0x0024;
constexpr guint32 gvbs_manufacturer_name = 0x0048; // 32 bytes
constexpr guint32 gvbs_model_name = 0x0068;        // 32 bytes
constexpr guint32 gvbs_serial_number = 0x00d8;     // 16 bytes
constexpr guint32 gvbs_tick_high = 0x093c;
constexpr guint32 gvbs_tick_low = 0x0940;
constexpr guint32 gvbs_ccp = 0x0a00;
constexpr guint32 gvbs_scp0 = 0x0d00;
constexpr guint32 gvbs_scda0 = 0x0d18;
constexpr guint32 fake_acquisition = 0x124;
constexpr guint32 fake_test_register = 0x1f0;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("loopback GigE simulator fixture: " + message);
}

// /proc/net/udp prints the 32-bit s_addr as host-order hex of the stored word.
std::string proc_ipv4(const std::string& hex) {
    in_addr a{};
    a.s_addr = static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
    char text[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &a, text, sizeof text);
    return text;
}

std::string ipv4_text(guint32 host_order) {
    in_addr a{};
    a.s_addr = htonl(host_order);
    char text[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &a, text, sizeof text);
    return text;
}

// /proc/net/udp6 prints the four 32-bit words of in6_addr as host-order hex.
std::string proc_ipv6(const std::string& hex) {
    if (hex.size() != 32) fail("unexpected /proc/net/udp6 address '" + hex + "'");
    in6_addr a{};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto word = static_cast<std::uint32_t>(std::stoul(hex.substr(i * 8, 8), nullptr, 16));
        std::memcpy(&a.s6_addr[i * 4], &word, sizeof word);
    }
    char text[INET6_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET6, &a, text, sizeof text);
    return text;
}

std::string sockaddr_ipv4_text(const sockaddr* address) {
    if (!address || address->sa_family != AF_INET) return "none";
    char text[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(address)->sin_addr, text, sizeof text);
    return text;
}

struct LoopbackInterface {
    std::string name;
    // ifa_ifu.ifu_broadaddr, exactly what the pinned Linux branch of
    // arv_enumerate_network_interfaces() copies (src/arvnetwork.c:380-381)
    // and arv_gv_fake_camera_start() compares with the GVCP address.
    std::string broadcast;
};

// IPv4 loopback interface (IFF_UP | IFF_LOOPBACK carrying 127.0.0.1).
LoopbackInterface loopback_interface() {
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) fail(std::string("getifaddrs failed: ") + std::strerror(errno));
    std::unique_ptr<ifaddrs, void (*)(ifaddrs*)> owner(list, &::freeifaddrs);
    std::string seen;
    for (auto* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_name) continue;
        const auto text = sockaddr_ipv4_text(i->ifa_addr);
        seen += std::string(seen.empty() ? "" : ", ") + i->ifa_name + "=" + text +
                ((i->ifa_flags & IFF_UP) ? "" : "(down)");
        if ((i->ifa_flags & IFF_UP) && (i->ifa_flags & IFF_LOOPBACK) && loopback_address == text)
            return {i->ifa_name, sockaddr_ipv4_text(i->ifa_ifu.ifu_broadaddr)};
    }
    fail("no UP IPv4 loopback interface with address 127.0.0.1 (IPv4 interfaces: " + (seen.empty() ? "none" : seen) +
         "); loopback UDP is required and the test is NOT skipped");
}

constexpr const char* exception_source =
    "pinned Aravis 0.8.36 arv_gv_fake_camera_start(), src/arvgvfakecamera.c:522-526 \"Global discovery\"; Linux "
    "ip(7): INADDR_BROADCAST has the same effect on bind(2) as INADDR_ANY, so it is NOT loopback-confined";

std::string memory_string(ArvFakeCamera* camera, guint32 address, guint32 size) {
    std::string text(size, '\0');
    if (!arv_fake_camera_read_memory(camera, address, size, text.data())) fail("cannot read simulator memory");
    text.resize(std::strlen(text.c_str()));
    return text;
}

guint32 reg(ArvFakeCamera* camera, guint32 address) {
    guint32 value = 0;
    if (!arv_fake_camera_read_register(camera, address, &value)) fail("cannot read simulator register");
    return value;
}

void on_finalized(gpointer data, GObject*) { static_cast<std::atomic<int>*>(data)->fetch_add(1); }

std::string object_of(const void* symbol) {
    Dl_info info{};
    if (!::dladdr(symbol, &info) || !info.dli_fname) return "<unknown>";
    return info.dli_fname;
}

// Socket inode -> fd for every socket descriptor of THIS process.
std::map<std::uint64_t, int> socket_descriptors() {
    std::map<std::uint64_t, int> descriptors;
    DIR* dir = ::opendir("/proc/self/fd");
    if (!dir) fail("cannot list /proc/self/fd");
    const int listing = ::dirfd(dir);
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const int fd = std::atoi(entry->d_name);
        if (fd == listing) continue;
        char target[256] = {};
        const std::string link = std::string("/proc/self/fd/") + entry->d_name;
        const auto n = ::readlink(link.c_str(), target, sizeof target - 1);
        if (n <= 0) continue;
        const std::string text(target, static_cast<std::size_t>(n));
        if (text.rfind("socket:[", 0) == 0 && text.size() > 9 && text.back() == ']')
            descriptors.emplace(std::stoull(text.substr(8, text.size() - 9)), fd);
    }
    ::closedir(dir);
    return descriptors;
}

// Socket options read from the fd, accepted only if the fd still refers to
// the same socket inode before and after the reads (no fd-reuse confusion).
void read_options(UdpSocket& socket) {
    struct stat before{};
    if (::fstat(socket.fd, &before) != 0 || !S_ISSOCK(before.st_mode) || before.st_ino != socket.inode) return;
    int type = 0, protocol = 0, reuse_address = 0, reuse_port = 0;
    char device[IFNAMSIZ + 1] = {};
    auto get = [&](int option, void* value, socklen_t size, socklen_t& length) {
        length = size;
        return ::getsockopt(socket.fd, SOL_SOCKET, option, value, &length) == 0;
    };
    const auto int_size = static_cast<socklen_t>(sizeof(int));
    socklen_t l_type = 0, l_protocol = 0, l_reuse_address = 0, l_reuse_port = 0, l_device = 0;
    const bool ok = get(SO_TYPE, &type, int_size, l_type) && get(SO_PROTOCOL, &protocol, int_size, l_protocol) &&
                    get(SO_REUSEADDR, &reuse_address, int_size, l_reuse_address) &&
                    get(SO_REUSEPORT, &reuse_port, int_size, l_reuse_port) &&
                    get(SO_BINDTODEVICE, device, static_cast<socklen_t>(IFNAMSIZ), l_device) &&
                    l_type == int_size && l_protocol == int_size && l_reuse_address == int_size &&
                    l_reuse_port == int_size && l_device <= IFNAMSIZ;
    struct stat after{};
    if (!ok || ::fstat(socket.fd, &after) != 0 || after.st_ino != socket.inode) return;
    socket.datagram_udp = type == SOCK_DGRAM && protocol == IPPROTO_UDP;
    socket.reuse_address = reuse_address != 0;
    socket.reuse_port = reuse_port != 0;
    socket.bound_device.assign(device, ::strnlen(device, l_device));
    socket.options_read = true;
}

void read_udp_table(const char* path, bool ipv6, const std::map<std::uint64_t, int>& descriptors,
                    std::vector<UdpSocket>& sockets) {
    std::ifstream in(path);
    if (!in) {
        if (ipv6 && ::access(path, F_OK) != 0 && errno == ENOENT) return; // IPv6 disabled: no UDPv6 sockets exist
        fail(std::string("cannot read ") + path);
    }
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string slot, local, remote, state, queues, timer, retransmit, uid, timeout, inode;
        fields >> slot >> local >> remote >> state >> queues >> timer >> retransmit >> uid >> timeout >> inode;
        if (inode.empty()) continue;
        const auto owned = descriptors.find(std::stoull(inode));
        if (owned == descriptors.end()) continue; // another process's socket
        const auto colon = local.find(':');
        if (colon == std::string::npos) fail(std::string("unparsable ") + path + " line: " + line);
        UdpSocket s;
        s.ipv6 = ipv6;
        const auto address = local.substr(0, colon);
        s.address = ipv6 ? proc_ipv6(address) : proc_ipv4(address);
        s.port = static_cast<std::uint16_t>(std::stoul(local.substr(colon + 1), nullptr, 16));
        s.inode = owned->first;
        s.fd = owned->second;
        read_options(s);
        sockets.push_back(std::move(s));
    }
}

} // namespace

int probe_udp_bind(std::uint16_t port) { return probe_udp_bind(loopback_address, port); }

int probe_udp_bind(std::string_view address, std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (::inet_pton(AF_INET, std::string(address).c_str(), &a.sin_addr) != 1) return EINVAL;
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return errno;
    const int rc = ::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    const int error = rc == 0 ? 0 : errno;
    ::close(fd);
    return error;
}

std::string UdpSocket::text() const {
    std::string t = (ipv6 ? "[" + address + "]" : address) + ":" + std::to_string(port) + " (inode " +
                    std::to_string(inode) + ", fd " + std::to_string(fd);
    if (!options_read) return t + ", options unreadable)";
    return t + (datagram_udp ? ", UDP datagram" : ", NOT a UDP datagram socket") + ", SO_REUSEADDR=" +
           (reuse_address ? "1" : "0") + ", SO_REUSEPORT=" + (reuse_port ? "1" : "0") +
           ", SO_BINDTODEVICE=" + (bound_device.empty() ? "none" : bound_device) + ")";
}

std::vector<UdpSocket> owned_udp_sockets() {
    const auto descriptors = socket_descriptors();
    std::vector<UdpSocket> sockets;
    read_udp_table("/proc/net/udp", false, descriptors, sockets);
    read_udp_table("/proc/net/udp6", true, descriptors, sockets);
    return sockets;
}

SocketClass classify_udp_socket(const UdpSocket& s, const UdpSocket* listener) {
    if (!s.ipv6 && s.address == loopback_address) return SocketClass::loopback;
    // The documented exception is ONE known socket, never a wildcard class:
    // every attribute of the pinned listener plus its recorded identity.
    if (listener && !s.ipv6 && s.options_read && s.datagram_udp && s.address == global_discovery_address &&
        s.port == gvcp_port && s.reuse_address && s.reuse_port && s.bound_device.empty() &&
        s.inode == listener->inode && s.fd == listener->fd)
        return SocketClass::simulator_global_discovery;
    return SocketClass::unexpected;
}

const char* to_string(SocketClass value) {
    switch (value) {
    case SocketClass::loopback: return "loopback";
    case SocketClass::simulator_global_discovery: return "simulator global-discovery listener (documented exception)";
    case SocketClass::unexpected: return "UNEXPECTED";
    }
    return "?";
}

std::string SocketAudit::summary() const {
    auto list = [](const std::vector<UdpSocket>& sockets) {
        std::string text;
        for (const auto& s : sockets) text += (text.empty() ? "" : "; ") + s.text();
        return "[" + text + "]";
    };
    return where + ": " + std::to_string(loopback.size() + discovery.size() + unexpected.size()) +
           " UDP socket(s) owned; loopback-bound " + std::to_string(loopback.size()) + " " + list(loopback) +
           "; documented simulator global-discovery listener " + std::to_string(discovery.size()) + " (expected " +
           std::to_string(expected_discovery) + ") " + list(discovery) + "; unexpected " +
           std::to_string(unexpected.size()) + " " + list(unexpected) + "; exception observed: " +
           (exception_observed() ? std::string("yes (") + exception_source + ")" : std::string("no")) + "; " +
           (passed() ? "PASS" : "FAIL");
}

SocketAudit audit_udp_sockets(std::string_view where, const UdpSocket* listener) {
    SocketAudit audit;
    audit.where = std::string(where);
    audit.expected_discovery = listener ? 1u : 0u;
    for (auto& s : owned_udp_sockets()) {
        switch (classify_udp_socket(s, listener)) {
        case SocketClass::loopback: audit.loopback.push_back(std::move(s)); break;
        case SocketClass::simulator_global_discovery: audit.discovery.push_back(std::move(s)); break;
        case SocketClass::unexpected: audit.unexpected.push_back(std::move(s)); break;
        }
    }
    return audit;
}

SocketAudit require_socket_audit(std::string_view where, const UdpSocket* listener) {
    auto audit = audit_udp_sockets(where, listener);
    if (!audit.passed()) fail("UDP socket audit failed: " + audit.summary());
    return audit;
}

std::string loaded_sdk_library() { return object_of(reinterpret_cast<const void*>(&arv_get_major_version)); }
std::string adapter_symbol_object() {
    return object_of(reinterpret_cast<const void*>(&gige_mel::backend::aravis::make_backend));
}

Simulator::Simulator(Options options) : options_(options) {
    if (options_.gvsp_lost_ratio && !(*options_.gvsp_lost_ratio >= 0.0 && *options_.gvsp_lost_ratio <= 1.0))
        fail("gvsp-lost-ratio must be in [0, 1]");
    const auto loopback = loopback_interface();
    interface_name_ = loopback.name;
    sockets_.loopback_broadcast = loopback.broadcast;
    if (const int busy = probe_udp_bind(gvcp_port); busy != 0)
        fail("UDP 127.0.0.1:" + std::to_string(gvcp_port) + " (GVCP) cannot be bound before the simulator starts: " +
             std::strerror(busy) + "; another GigE simulator or camera tool may own it. Not skipped.");
    // Before start: no 255.255.255.255:3956 or 0.0.0.0:3956 holder anywhere in
    // this network namespace, and this process owns loopback-bound UDP
    // sockets only. No wildcard is inherited from an earlier scenario, so
    // every wildcard after start is attributable to this simulator.
    if (const int busy = probe_udp_bind(global_discovery_address, gvcp_port); busy != 0)
        fail("UDP 255.255.255.255:" + std::to_string(gvcp_port) + " cannot be bound before the simulator starts: " +
             std::strerror(busy) + "; a GigE discovery listener already exists in this network namespace. "
             "Not skipped.");
    const auto before = aravis_gv_loopback::require_socket_audit("before simulator start", nullptr);
    std::set<std::uint64_t> preexisting;
    for (const auto& s : before.loopback) preexisting.insert(s.inode);
    char* previous = arv_gv_interface_dup_discovery_interface_name();
    if (previous) previous_interface_ = std::string(previous);
    g_free(previous);
    // Restrict GigE discovery to loopback for THIS test process (public API).
    arv_gv_interface_set_discovery_interface_name(interface_name_.c_str());
    interface_overridden_ = true;
    try {
        const std::string address(loopback_address), serial(serial_number);
        if (!options_.gvsp_lost_ratio) {
            simulator_ = arv_gv_fake_camera_new(address.c_str(), serial.c_str()); // exactly as tests/fakegv.c
        } else {
            simulator_ = ARV_GV_FAKE_CAMERA(g_object_new(ARV_TYPE_GV_FAKE_CAMERA, "interface-name", address.c_str(),
                                                         "serial-number", serial.c_str(), "gvsp-lost-ratio",
                                                         *options_.gvsp_lost_ratio, nullptr));
        }
        if (!simulator_ || !ARV_IS_GV_FAKE_CAMERA(simulator_)) fail("no ArvGvFakeCamera was constructed");
        g_object_weak_ref(G_OBJECT(simulator_), &on_finalized, &finalized_);
        if (!arv_gv_fake_camera_is_running(simulator_))
            fail("arv_gv_fake_camera_is_running() is FALSE: the pinned simulator could not bind interface '" + address +
                 "' or start its thread. Not skipped.");
        ArvFakeCamera* fake = arv_gv_fake_camera_get_fake_camera(simulator_);
        if (!fake || !ARV_IS_FAKE_CAMERA(fake)) fail("the simulator's underlying device is not an ArvFakeCamera");
        // The simulator itself now owns the GVCP port (its bound input socket).
        if (const int held = probe_udp_bind(gvcp_port); held != EADDRINUSE)
            fail("after start, UDP 127.0.0.1:" + std::to_string(gvcp_port) + " is not held by the simulator (probe: " +
                 (held ? std::strerror(held) : "bind succeeded") + ")");
        identify_sockets(preexisting);
        // Identity and endpoint in the simulator's own bootstrap memory.
        const auto ip = ipv4_text(reg(fake, gvbs_current_ip));
        const auto vendor = memory_string(fake, gvbs_manufacturer_name, 32);
        const auto model = memory_string(fake, gvbs_model_name, 32);
        const auto serial_read = memory_string(fake, gvbs_serial_number, 16);
        if (ip != loopback_address || vendor != expected_vendor || model != expected_model || serial_read != serial)
            fail("unexpected simulator identity/endpoint: ip=" + ip + " vendor=" + vendor + " model=" + model +
                 " serial=" + serial_read);
        if (reg(fake, gvbs_ccp) != 0 || reg(fake, fake_acquisition) != 0)
            fail("a fresh simulator already reports a controller or an acquisition");
    } catch (...) {
        release_noexcept();
        throw;
    }
}

void Simulator::identify_sockets(const std::set<std::uint64_t>& preexisting) {
    std::vector<UdpSocket> created;
    for (auto& s : owned_udp_sockets())
        if (!preexisting.count(s.inode)) created.push_back(std::move(s));
    std::string list;
    for (const auto& s : created) list += (list.empty() ? "" : "; ") + s.text();
    auto mismatch = [&](const std::string& why) {
        fail("the simulator's UDP sockets differ from the pinned arv_gv_fake_camera_start() (src/arvgvfakecamera.c:"
             "516-536): " + why + "; created: [" + list + "]");
    };
    // Subnet discovery (src/arvgvfakecamera.c:530-536) binds the interface
    // broadcast address unless it equals the GVCP address; for `lo` the
    // pinned Linux enumeration copies ifu_broadaddr = 127.0.0.1, so it is
    // never created. Anything else would be an additional, unaudited socket.
    if (sockets_.loopback_broadcast != loopback_address)
        mismatch("interface '" + interface_name_ + "' reports broadcast address " + sockets_.loopback_broadcast +
                 ", so the pinned simulator would also bind a subnet-discovery socket; this fixture permits none");
    if (created.size() != 3) mismatch("expected exactly 3 new sockets, found " + std::to_string(created.size()));
    bool gvcp = false, gvsp = false, discovery = false;
    for (const auto& s : created) {
        if (!s.options_read || !s.datagram_udp || s.ipv6 || !s.bound_device.empty())
            mismatch("socket " + s.text() + " is not an unbound-device IPv4 UDP datagram socket");
        if (s.address == loopback_address && s.port == gvcp_port && !s.reuse_address && !s.reuse_port && !gvcp) {
            sockets_.gvcp = s; // allow_reuse FALSE
            gvcp = true;
        } else if (s.address == loopback_address && s.port != gvcp_port && !s.reuse_address && !s.reuse_port && !gvsp) {
            sockets_.gvsp_output = s; // port 0 -> kernel-assigned, allow_reuse FALSE
            gvsp = true;
        } else if (s.address == global_discovery_address && s.port == gvcp_port && s.reuse_address && s.reuse_port &&
                   !discovery) {
            sockets_.global_discovery = s; // allow_reuse TRUE -> SO_REUSEADDR + SO_REUSEPORT (GIO g_socket_bind)
            discovery = true;
        } else {
            mismatch("socket " + s.text() + " matches none of the expected GVCP, GVSP or global-discovery sockets");
        }
    }
    if (!gvcp || !gvsp || !discovery) mismatch("a pinned socket is missing");
    if (classify_udp_socket(sockets_.global_discovery, &sockets_.global_discovery) !=
        SocketClass::simulator_global_discovery)
        mismatch("the global-discovery socket does not classify as the documented exception");
}

SocketAudit Simulator::socket_audit(std::string_view where) const {
    return audit_udp_sockets(where, simulator_ ? &sockets_.global_discovery : nullptr);
}

SocketAudit Simulator::require_socket_audit(std::string_view where) const {
    if (!simulator_) fail(std::string(where) + ": no live simulator to audit");
    auto audit = aravis_gv_loopback::require_socket_audit(where, &sockets_.global_discovery);
    auto still_owned = [&](const UdpSocket& expected) {
        for (const auto& s : audit.loopback)
            if (s.inode == expected.inode && s.address == expected.address && s.port == expected.port) return true;
        return false;
    };
    if (!still_owned(sockets_.gvcp) || !still_owned(sockets_.gvsp_output))
        fail(std::string(where) + ": the simulator's loopback GVCP/GVSP sockets are no longer owned: " +
             audit.summary());
    return audit;
}

Simulator::~Simulator() { release_noexcept(); }

void Simulator::release_noexcept() noexcept {
    if (simulator_) {
        g_object_unref(simulator_); // pinned finalizer: joins its thread, closes its sockets
        simulator_ = nullptr;
    }
    if (interface_overridden_) {
        arv_gv_interface_set_discovery_interface_name(previous_interface_ ? previous_interface_->c_str() : nullptr);
        interface_overridden_ = false;
    }
}

ArvFakeCamera* Simulator::camera() const {
    if (!simulator_) fail("the simulator was already finished");
    return arv_gv_fake_camera_get_fake_camera(simulator_);
}
std::uint32_t Simulator::read_register(std::uint32_t address) const { return reg(camera(), address); }
std::uint32_t Simulator::acquisition_status() const { return arv_fake_camera_get_acquisition_status(camera()); }
std::uint32_t Simulator::control_privilege() const { return reg(camera(), gvbs_ccp); }
std::uint32_t Simulator::stream_port() const { return reg(camera(), gvbs_scp0) & 0xffffu; }
std::string Simulator::stream_destination() const { return ipv4_text(reg(camera(), gvbs_scda0)); }
std::uint64_t Simulator::tick_frequency_hz() const {
    return (std::uint64_t{reg(camera(), gvbs_tick_high)} << 32) | reg(camera(), gvbs_tick_low);
}
std::size_t Simulator::payload_bytes() const { return arv_fake_camera_get_payload(camera()); }
bool Simulator::free_running() const { return arv_fake_camera_is_in_free_running_mode(camera()); }
std::size_t Simulator::genicam_xml_bytes() const {
    std::size_t size = 0;
    (void)arv_fake_camera_get_genicam_xml(camera(), &size);
    return size;
}
std::string Simulator::genicam_xml_sha256() const {
    std::size_t size = 0;
    const char* xml = arv_fake_camera_get_genicam_xml(camera(), &size);
    if (!xml || !size) fail("the simulator serves no GenICam XML");
    std::unique_ptr<gchar, decltype(&g_free)> digest(
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(xml), size), &g_free);
    return digest ? digest.get() : "";
}
void Simulator::write_test_register(std::uint32_t value) {
    if (!arv_fake_camera_write_register(camera(), fake_test_register, value)) fail("cannot write TestRegister");
}

std::vector<backend::DeviceInfo> Simulator::discover(backend::ICameraBackend& camera,
                                                     std::chrono::milliseconds bound) const {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    std::string last;
    int attempts = 0;
    for (;;) {
        ++attempts;
        auto found = camera.enumerate(); // the production GigE discovery (1 s SDK poll per attempt)
        if (!found) fail("enumerate() failed: " + found.error().message);
        last.clear();
        for (const auto& d : found.value()) {
            last += (last.empty() ? "" : "; ") + d.device_id + " [" + d.display_name + "]";
            if (d.device_id == expected_device_id) return found.value();
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    const int bind_state = probe_udp_bind(gvcp_port);
    fail("GigE discovery on interface '" + interface_name_ + "' did not find '" + std::string(expected_device_id) +
         "' within " + std::to_string(bound.count()) + " ms (" + std::to_string(attempts) + " attempts); found: " +
         (last.empty() ? "nothing" : last) + "; simulator running=" +
         (simulator_ && arv_gv_fake_camera_is_running(simulator_) ? "yes" : "no") + "; GVCP port probe: " +
         (bind_state ? std::strerror(bind_state) : "FREE (simulator socket missing)") + ". Not skipped.");
}

void Simulator::finish() {
    if (finished_) return;
    if (!simulator_) fail("finish() without a simulator");
    // The client must be closed first: GVCP control released and no acquisition.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while ((control_privilege() != 0 || acquisition_status() != 0) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    if (control_privilege() != 0 || acquisition_status() != 0)
        fail("finish(): a client still holds GVCP control (CCP=" + std::to_string(control_privilege()) +
             ") or acquisition (" + std::to_string(acquisition_status()) + "); close the client device first");
    g_object_unref(simulator_);
    simulator_ = nullptr;
    if (finalized_.load() != 1) fail("finish(): the simulator was not finalized (an extra reference is alive)");
    // No exception is permitted any more: the global-discovery listener, and
    // every other wildcard, must be gone; so must every recorded inode.
    auto released = aravis_gv_loopback::require_socket_audit("after simulator destruction", nullptr);
    for (const auto* own : {&sockets_.gvcp, &sockets_.gvsp_output, &sockets_.global_discovery})
        for (const auto& s : released.loopback)
            if (s.inode == own->inode)
                fail("finish(): simulator socket " + own->text() + " is still owned after destruction");
    if (const int state = probe_udp_bind(gvcp_port); state != 0)
        fail(std::string("finish(): UDP 127.0.0.1:3956 is still bound after the simulator was destroyed: ") +
             std::strerror(state));
    // Namespace-wide: no 255.255.255.255:3956 (or 0.0.0.0:3956) listener
    // remains in ANY process of this network namespace.
    if (const int state = probe_udp_bind(global_discovery_address, gvcp_port); state != 0)
        fail(std::string("finish(): UDP 255.255.255.255:3956 is still bound after the simulator was destroyed: ") +
             std::strerror(state));
    release_audit_ = std::move(released);
    arv_gv_interface_set_discovery_interface_name(previous_interface_ ? previous_interface_->c_str() : nullptr);
    interface_overridden_ = false;
    char* restored = arv_gv_interface_dup_discovery_interface_name();
    const bool same = previous_interface_ ? (restored && *previous_interface_ == restored) : restored == nullptr;
    g_free(restored);
    if (!same) fail("finish(): the previous GigE discovery interface name was not restored");
    finished_ = true;
}

} // namespace aravis_gv_loopback
