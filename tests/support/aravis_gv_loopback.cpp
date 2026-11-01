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
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
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

// IPv4 loopback interface name (IFF_UP | IFF_LOOPBACK carrying 127.0.0.1).
std::string loopback_interface() {
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) fail(std::string("getifaddrs failed: ") + std::strerror(errno));
    std::unique_ptr<ifaddrs, void (*)(ifaddrs*)> owner(list, &::freeifaddrs);
    std::string seen;
    for (auto* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_name) continue;
        char text[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr, text, sizeof text);
        seen += std::string(seen.empty() ? "" : ", ") + i->ifa_name + "=" + text +
                ((i->ifa_flags & IFF_UP) ? "" : "(down)");
        if ((i->ifa_flags & IFF_UP) && (i->ifa_flags & IFF_LOOPBACK) && loopback_address == text) return i->ifa_name;
    }
    fail("no UP IPv4 loopback interface with address 127.0.0.1 (IPv4 interfaces: " + (seen.empty() ? "none" : seen) +
         "); loopback UDP is required and the test is NOT skipped");
}

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

} // namespace

int probe_udp_bind(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return errno;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int rc = ::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    const int error = rc == 0 ? 0 : errno;
    ::close(fd);
    return error;
}

std::vector<UdpSocket> owned_udp_sockets() {
    std::set<std::string> inodes;
    DIR* dir = ::opendir("/proc/self/fd");
    if (!dir) fail("cannot list /proc/self/fd");
    while (const dirent* entry = ::readdir(dir)) {
        char target[256] = {};
        const std::string link = std::string("/proc/self/fd/") + entry->d_name;
        const auto n = ::readlink(link.c_str(), target, sizeof target - 1);
        if (n <= 0) continue;
        const std::string text(target, static_cast<std::size_t>(n));
        if (text.rfind("socket:[", 0) == 0 && text.size() > 9) inodes.insert(text.substr(8, text.size() - 9));
    }
    ::closedir(dir);
    std::ifstream in("/proc/net/udp");
    if (!in) fail("cannot read /proc/net/udp");
    std::vector<UdpSocket> sockets;
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string slot, local, remote, state, queues, timer, retransmit, uid, timeout, inode;
        fields >> slot >> local >> remote >> state >> queues >> timer >> retransmit >> uid >> timeout >> inode;
        if (!inodes.count(inode)) continue;
        const auto colon = local.find(':');
        if (colon == std::string::npos) continue;
        sockets.push_back({proc_ipv4(local.substr(0, colon)),
                           static_cast<std::uint16_t>(std::stoul(local.substr(colon + 1), nullptr, 16))});
    }
    return sockets;
}

void require_loopback_only_sockets(std::string_view where) {
    std::string list;
    bool ok = true;
    for (const auto& s : owned_udp_sockets()) {
        list += (list.empty() ? "" : ", ") + s.address + ":" + std::to_string(s.port);
        if (s.address != loopback_address && s.address != "255.255.255.255") ok = false;
    }
    if (!ok) fail(std::string(where) + ": this process owns a UDP socket outside 127.0.0.1/255.255.255.255: " + list);
}

std::string loaded_sdk_library() { return object_of(reinterpret_cast<const void*>(&arv_get_major_version)); }
std::string adapter_symbol_object() {
    return object_of(reinterpret_cast<const void*>(&gige_mel::backend::aravis::make_backend));
}

Simulator::Simulator(Options options) : options_(options) {
    if (options_.gvsp_lost_ratio && !(*options_.gvsp_lost_ratio >= 0.0 && *options_.gvsp_lost_ratio <= 1.0))
        fail("gvsp-lost-ratio must be in [0, 1]");
    interface_name_ = loopback_interface();
    if (const int busy = probe_udp_bind(gvcp_port); busy != 0)
        fail("UDP 127.0.0.1:" + std::to_string(gvcp_port) + " (GVCP) cannot be bound before the simulator starts: " +
             std::strerror(busy) + "; another GigE simulator or camera tool may own it. Not skipped.");
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
    if (const int state = probe_udp_bind(gvcp_port); state != 0)
        fail(std::string("finish(): UDP 127.0.0.1:3956 is still bound after the simulator was destroyed: ") +
             std::strerror(state));
    arv_gv_interface_set_discovery_interface_name(previous_interface_ ? previous_interface_->c_str() : nullptr);
    interface_overridden_ = false;
    char* restored = arv_gv_interface_dup_discovery_interface_name();
    const bool same = previous_interface_ ? (restored && *previous_interface_ == restored) : restored == nullptr;
    g_free(restored);
    if (!same) fail("finish(): the previous GigE discovery interface name was not restored");
    finished_ = true;
}

} // namespace aravis_gv_loopback
