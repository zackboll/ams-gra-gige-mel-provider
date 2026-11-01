#pragma once

// TEST-ONLY loopback GigE Vision simulator fixture (GIGE-MEL-011). Never
// installed, never linked into the adapter library, the provider or the
// diagnostic, and not reachable from any production factory or setting.
//
// It runs the pinned Aravis 0.8.36 network camera simulator ArvGvFakeCamera
// (src/arvgvfakecamera.c, the same object the upstream tests/fakegv.c uses)
// IN THIS PROCESS, bound to 127.0.0.1. The simulator answers real GVCP on UDP
// 127.0.0.1:3956 and transmits real GVSP over UDP to the stream destination
// a client programs. It is NOT ArvFakeInterface/ArvFakeStream: the client is
// the unchanged production aravis::make_backend(), i.e. Aravis's GigE Vision
// interface, ArvGvDevice and ArvGvStream.
//
// The only process-wide SDK setting it changes is the GigE discovery
// interface name (arv_gv_interface_set_discovery_interface_name, public API
// since 0.8.34). It restricts the CLIENT's discovery to the IPv4 loopback
// interface: the client's discovery sockets are bound to 127.0.0.1, and no
// physical camera can be discovered. The previous value is restored by
// finish(). The production adapter's discovery policy is unchanged.
// arv_shutdown() is never called.
//
// NOT LOOPBACK-CONFINED (GIGE-MEL-011-R1). The client setting does not
// restrict the SIMULATOR. The pinned arv_gv_fake_camera_start()
// (src/arvgvfakecamera.c:522-526) also binds a "Global discovery" UDP socket
// to 255.255.255.255:3956 with SO_REUSEADDR/SO_REUSEPORT and no
// SO_BINDTODEVICE. On Linux, ip(7): INADDR_BROADCAST "has the same effect on
// bind(2) as INADDR_ANY", so that listener is not restricted to `lo`. It
// exists from simulator construction until finish(). The socket audit below
// identifies it explicitly as a documented test-only exception; it is not
// counted as loopback and nothing removes the exposure.
//
// This is software-simulated GigE transport. It qualifies no physical
// camera, NIC, switch, exposure timing or timestamp.

#include <gige_mel/backend.hpp>

#include <arv.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace aravis_gv_loopback {

namespace backend = gige_mel::backend;

inline constexpr std::string_view loopback_address = "127.0.0.1";
inline constexpr std::uint16_t gvcp_port = 3956; // ARV_GVCP_PORT (pinned src/arvgvcpprivate.h:36)
inline constexpr std::string_view serial_number = "GVTest";
// "<vendor>-<model>-<serial>" as built by the pinned src/arvgvinterface.c:281
// from the simulator's discovery data; asserted against actual discovery,
// never assumed from the serial alone.
inline constexpr std::string_view expected_device_id = "Aravis-Fake-GVTest";
inline constexpr std::string_view expected_vendor = "Aravis";
inline constexpr std::string_view expected_model = "Fake";

struct Options {
    // nullopt: the exact upstream constructor arv_gv_fake_camera_new(
    // "127.0.0.1", "GVTest") (property default 0.0, i.e. no simulated loss).
    // A value: g_object_new() with the same interface/serial properties plus
    // the pinned GObject property "gvsp-lost-ratio" (0.0 .. 1.0). It is a
    // construct property, so it is set before the simulator thread exists and
    // never races with GVSP transmission. The property is write-only in the
    // pinned SDK, so it cannot be read back; its effect is what is tested.
    std::optional<double> gvsp_lost_ratio;
};

// Binds a UDP socket to 127.0.0.1:port and closes it: 0 on success, else errno.
int probe_udp_bind(std::uint16_t port);
// The same for any IPv4 address, without SO_REUSEADDR/SO_REUSEPORT. For
// 255.255.255.255:3956 this fails with EADDRINUSE while ANY socket in this
// network namespace holds 255.255.255.255:3956 or 0.0.0.0:3956 (observed on
// Linux 6.12; the kernel does not treat it as conflicting with 127.0.0.1).
// The probe socket exists only between bind() and close().
int probe_udp_bind(std::string_view address, std::uint16_t port);

// The kernel-reported local address of the pinned simulator's global
// discovery listener. Linux stores the bound address as the receive address
// (net/ipv4/af_inet.c inet_bind) and reports it unchanged in /proc/net/udp
// and getsockname(), so it is 255.255.255.255, NOT 0.0.0.0. A 0.0.0.0 socket
// is never this listener.
inline constexpr std::string_view global_discovery_address = "255.255.255.255";

// A UDP socket owned by THIS process: /proc/self/fd (socket inode -> fd)
// joined with /proc/net/udp and /proc/net/udp6 by inode. Options are read
// from the same fd after fstat() confirms it still has that inode.
struct UdpSocket {
    bool ipv6{false};             // from /proc/net/udp6
    std::string address;          // kernel-reported local address
    std::uint16_t port{0};
    std::uint64_t inode{0};
    int fd{-1};
    bool options_read{false};     // false if the fd closed or changed meanwhile
    bool datagram_udp{false};     // SO_TYPE == SOCK_DGRAM && SO_PROTOCOL == IPPROTO_UDP
    bool reuse_address{false};    // SO_REUSEADDR
    bool reuse_port{false};       // SO_REUSEPORT
    std::string bound_device;     // SO_BINDTODEVICE; empty = not bound to an interface
    std::string text() const;     // "255.255.255.255:3956 (inode N, fd F, ...)"
};
std::vector<UdpSocket> owned_udp_sockets();

// Explicit three-way classification (GIGE-MEL-011-R1).
enum class SocketClass {
    loopback,                    // IPv4, bound to 127.0.0.1 (any port)
    simulator_global_discovery,  // the documented exception, see below
    unexpected,                  // everything else, including any other wildcard
};
// `listener` is the global discovery socket recorded by a LIVE Simulator at
// its start (nullptr when none is alive). A socket is the documented
// exception only if ALL hold: IPv4 UDP datagram; address exactly
// 255.255.255.255; port exactly 3956; SO_REUSEADDR and SO_REUSEPORT set (the
// pinned allow_reuse=TRUE through g_socket_bind); no SO_BINDTODEVICE; and the
// SAME inode and fd as the listener. Any other 0.0.0.0/255.255.255.255, IPv6
// or non-loopback socket is unexpected. Pure: no I/O.
SocketClass classify_udp_socket(const UdpSocket& socket, const UdpSocket* listener);
const char* to_string(SocketClass value);

struct SocketAudit {
    std::string where;
    std::vector<UdpSocket> loopback;
    std::vector<UdpSocket> discovery;  // matched the documented exception
    std::vector<UdpSocket> unexpected;
    std::size_t expected_discovery{0}; // 1 while a simulator is alive, else 0
    bool exception_observed() const noexcept { return !discovery.empty(); }
    bool passed() const noexcept { return unexpected.empty() && discovery.size() == expected_discovery; }
    // Counts, every bound address:port, unexpected count, and whether the
    // documented exception was observed (with its upstream source).
    std::string summary() const;
};
// Classifies every UDP socket this process owns. Never throws on findings
// (only if /proc cannot be read): the result says what was observed.
SocketAudit audit_udp_sockets(std::string_view where, const UdpSocket* listener);
// The same, but throws with summary() unless passed(). With listener ==
// nullptr this requires that NO wildcard or non-loopback UDP socket exists.
SocketAudit require_socket_audit(std::string_view where, const UdpSocket* listener);

// Path of the libaravis actually mapped into this process (dladdr), and of
// the object that contains the production aravis::make_backend() symbol.
std::string loaded_sdk_library();
std::string adapter_symbol_object();

// The UDP sockets one pinned ArvGvFakeCamera creates, identified at start as
// the sockets (by inode) that appeared across its construction.
struct SimulatorSockets {
    UdpSocket gvsp_output;      // 127.0.0.1:<ephemeral>       src/arvgvfakecamera.c:516-517
    UdpSocket gvcp;             // 127.0.0.1:3956, no reuse     src/arvgvfakecamera.c:518-520
    UdpSocket global_discovery; // 255.255.255.255:3956, reuse  src/arvgvfakecamera.c:522-526
    // "Subnet discovery" (src/arvgvfakecamera.c:530-536) is created only if
    // the interface broadcast address differs from the GVCP address; for
    // Linux `lo` getifaddrs reports 127.0.0.1, so it is required to be absent.
    std::string loopback_broadcast; // as reported by getifaddrs for `lo`
};

class Simulator {
public:
    // Throws std::runtime_error with an explicit diagnostic (never skips):
    // no IPv4 loopback interface; GVCP port already bound; a wildcard or
    // non-loopback UDP socket already owned before start; simulator not
    // running or not holding its GVCP socket; the new sockets are not
    // exactly the three pinned ones above; identity/endpoint mismatch.
    explicit Simulator(Options options = {});
    ~Simulator(); // best-effort backstop of finish(); never throws
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    // The simulator's own ArvFakeCamera (transfer none; owned by the simulator).
    ArvFakeCamera* camera() const;
    // Name of the loopback interface discovery is restricted to (e.g. "lo").
    const std::string& discovery_interface() const noexcept { return interface_name_; }
    std::optional<double> gvsp_lost_ratio() const noexcept { return options_.gvsp_lost_ratio; }
    // The simulator's own sockets as identified at start.
    const SimulatorSockets& sockets() const noexcept { return sockets_; }
    // Audit with this simulator's listener as the only permitted exception
    // (non-throwing), and the checked form, which also requires the three
    // recorded simulator sockets to still be owned with the same identity.
    SocketAudit socket_audit(std::string_view where) const;
    SocketAudit require_socket_audit(std::string_view where) const;

    // SERVER-SIDE observations of the simulator's own register memory,
    // independent of frame arrival and of the client.
    std::uint32_t read_register(std::uint32_t address) const; // any 32-bit big-endian register
    std::uint32_t acquisition_status() const; // AcquisitionStart/Stop register (0x124)
    std::uint32_t control_privilege() const;  // GVBS CCP (0x0A00): nonzero while a client controls it
    std::uint32_t stream_port() const;        // GVBS SCP0 host port (0x0D00, low 16 bits)
    std::string stream_destination() const;   // GVBS SCDA0 (0x0D18) as dotted IPv4
    std::uint64_t tick_frequency_hz() const;  // GVBS 0x093C/0x0940
    std::size_t payload_bytes() const;        // pinned arv_fake_camera_get_payload()
    bool free_running() const;                // pinned arv_fake_camera_is_in_free_running_mode()
    std::size_t genicam_xml_bytes() const;
    std::string genicam_xml_sha256() const;   // of the XML the simulator serves
    void write_test_register(std::uint32_t value); // TestRegister (0x1F0), server memory

    // Bounded discovery through the given backend's ordinary enumerate()
    // (the production GigE path). Retries until expected_device_id appears or
    // `bound` expires; expiry throws with the discovery interface, the
    // devices actually found and the GVCP port state.
    std::vector<backend::DeviceInfo> discover(backend::ICameraBackend& camera,
                                              std::chrono::milliseconds bound = std::chrono::seconds{10}) const;

    // Checked teardown, in order: no client may still hold GVCP control or an
    // acquisition (the client must be closed first); drop the simulator (the
    // pinned finalizer joins its thread and closes its sockets); prove it was
    // finalized; audit with NO permitted exception, so the global discovery
    // listener and the other recorded simulator sockets must be gone; prove
    // UDP 127.0.0.1:3956 is free again; restore and re-read the previous
    // discovery interface name. Throws on any violation. These checks show
    // the release, not that the listener was never reachable meanwhile.
    void finish();
    // The audit finish() performed after destruction (empty before).
    const std::optional<SocketAudit>& release_audit() const noexcept { return release_audit_; }

private:
    void release_noexcept() noexcept;
    // Identifies the sockets that appeared across construction (inodes not
    // in `preexisting`) as exactly the pinned GVSP, GVCP and global-discovery
    // sockets, and the subnet-discovery socket as absent; throws otherwise.
    void identify_sockets(const std::set<std::uint64_t>& preexisting);

    Options options_;
    SimulatorSockets sockets_;
    std::optional<SocketAudit> release_audit_;
    std::string interface_name_;
    std::optional<std::string> previous_interface_; // nullopt => NULL (all interfaces)
    bool interface_overridden_{false};
    ArvGvFakeCamera* simulator_{nullptr};
    std::atomic<int> finalized_{0};
    bool finished_{false};
};

} // namespace aravis_gv_loopback
