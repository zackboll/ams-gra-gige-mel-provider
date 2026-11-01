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
// since 0.8.34), restricted to the IPv4 loopback interface so that discovery
// broadcasts never leave loopback and no physical camera can be discovered.
// The previous value is restored by finish(). The production adapter's
// discovery policy is unchanged. arv_shutdown() is never called.
//
// This is software-simulated GigE transport. It qualifies no physical
// camera, NIC, switch, exposure timing or timestamp.

#include <gige_mel/backend.hpp>

#include <arv.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
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

// IPv4 UDP sockets owned by THIS process (/proc/self/fd joined with
// /proc/net/udp by socket inode): local address and port.
struct UdpSocket {
    std::string address;
    std::uint16_t port{0};
};
std::vector<UdpSocket> owned_udp_sockets();
// Throws unless every UDP socket this process owns is bound to 127.0.0.1 or
// to the limited broadcast 255.255.255.255 (the pinned simulator's global
// discovery socket). `where` labels the diagnostic.
void require_loopback_only_sockets(std::string_view where);

// Path of the libaravis actually mapped into this process (dladdr), and of
// the object that contains the production aravis::make_backend() symbol.
std::string loaded_sdk_library();
std::string adapter_symbol_object();

class Simulator {
public:
    // Throws std::runtime_error with an explicit diagnostic (never skips):
    // no IPv4 loopback interface; GVCP port already bound; simulator not
    // running or not holding its GVCP socket; identity/endpoint mismatch.
    explicit Simulator(Options options = {});
    ~Simulator(); // best-effort backstop of finish(); never throws
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    // The simulator's own ArvFakeCamera (transfer none; owned by the simulator).
    ArvFakeCamera* camera() const;
    // Name of the loopback interface discovery is restricted to (e.g. "lo").
    const std::string& discovery_interface() const noexcept { return interface_name_; }
    std::optional<double> gvsp_lost_ratio() const noexcept { return options_.gvsp_lost_ratio; }

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
    // finalized and that UDP 127.0.0.1:3956 is free again; restore the
    // previous discovery interface name. Throws on any violation.
    void finish();

private:
    void release_noexcept() noexcept;

    Options options_;
    std::string interface_name_;
    std::optional<std::string> previous_interface_; // nullopt => NULL (all interfaces)
    bool interface_overridden_{false};
    ArvGvFakeCamera* simulator_{nullptr};
    std::atomic<int> finalized_{0};
    bool finished_{false};
};

} // namespace aravis_gv_loopback
