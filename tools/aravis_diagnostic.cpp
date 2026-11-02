// Optional, uninstalled Aravis diagnostic (GIGE-MEL-008/009/010). Uses Aravis's
// GigE Vision interface only (no Fake fallback). `inspect` never writes a
// camera feature or starts acquisition; `apply`, `capture` and `time-probe`
// are separate explicit commands. `time-probe` only observes raw SDK
// timestamps; it never qualifies them.
//
// GIGE-MEL-013: the optional, default-off `--discovery-interface IFNAME`
// restricts THIS process's GigE Vision discovery to one network interface
// through the pinned public arv_gv_interface_set_discovery_interface_name().
// When omitted, nothing is set and the SDK default (every UP IPv4 interface)
// is unchanged. The production backend (aravis::make_backend()) is unchanged.
#include "aravis_backend.hpp"

#include <arv.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace backend = gige_mel::backend;
namespace aravis = gige_mel::backend::aravis;

namespace {

constexpr const char* usage =
    "Usage:\n"
    "  gige_mel_aravis_diagnostic --help\n"
    "  gige_mel_aravis_diagnostic [--discovery-interface IFNAME] COMMAND [ARGUMENTS]\n"
    "\n"
    "Option (before COMMAND, at most once; default off):\n"
    "  --discovery-interface IFNAME\n"
    "      Restrict this process's GigE Vision discovery to the existing, UP network\n"
    "      interface IFNAME, which must have an IPv4 address (e.g. lo, or the camera NIC).\n"
    "      Checked before any device I/O. No fallback: a device not found through IFNAME\n"
    "      is a failure; no other interface is tried. When omitted, discovery uses the\n"
    "      SDK default: every UP IPv4 interface. The exact DEVICE_ID is still required.\n"
    "\n"
    "Commands:\n"
    "  gige_mel_aravis_diagnostic list\n"
    "      Discover GigE Vision devices (network broadcast) and print stable device IDs.\n"
    "  gige_mel_aravis_diagnostic inspect DEVICE_ID\n"
    "      Open exactly DEVICE_ID read-only: SDK version, identity, native settings, and\n"
    "      the profile decision. Writes no camera feature; starts no acquisition.\n"
    "  gige_mel_aravis_diagnostic apply DEVICE_ID WIDTH HEIGHT OFFSET_X OFFSET_Y RATE_HZ EXPOSURE_NS\n"
    "      Explicitly apply one complete configuration and print the actual readback.\n"
    "      Requires the documented manual/free-running Mono8 profile; changes nothing else.\n"
    "  gige_mel_aravis_diagnostic capture DEVICE_ID OUTPUT.pgm\n"
    "      Open exactly DEVICE_ID, use its current supported configuration (no feature write),\n"
    "      acquire ONE validated Mono8 frame within 5 s, write a tightly packed binary PGM,\n"
    "      then stop and close. The frame timestamp is unqualified (not a camera or MEL time).\n"
    "  gige_mel_aravis_diagnostic time-probe DEVICE_ID COUNT OUTPUT.jsonl\n"
    "      Read-only native timestamp OBSERVATION. Open exactly DEVICE_ID, use its current\n"
    "      supported configuration (no feature write; no trigger/PTP/network change), receive\n"
    "      COUNT (1-32) validated Mono8 frames within ONE overall 10 s deadline, stop, close,\n"
    "      then write one JSON header line, one line per frame (raw SDK buffer/system\n"
    "      timestamps) and one summary line. OUTPUT.jsonl must not exist; it is removed on any\n"
    "      failure. Every record says timestamp_qualified=false: this is not a timing\n"
    "      qualification and establishes no integration start, epoch, UTC or PTP fact.\n"
    "\n"
    "Internal tool for the experimental Aravis adapter; not a supported MEL image profile.\n"
    "Exit status: 0 success, 1 failure/unsupported/readback differs/cleanup failure or an\n"
    "unusable discovery interface, 2 usage.\n";

const char* code_name(backend::ErrorCode code) {
    using C = backend::ErrorCode;
    switch (code) {
    case C::invalid_argument: return "invalid_argument";
    case C::invalid_state: return "invalid_state";
    case C::not_found: return "not_found";
    case C::busy: return "busy";
    case C::unsupported: return "unsupported";
    case C::resource_exhausted: return "resource_exhausted";
    case C::timeout: return "timeout";
    case C::stopped: return "stopped";
    case C::disconnected: return "disconnected";
    case C::io_error: return "io_error";
    }
    return "unknown";
}

int report(const backend::Error& error) {
    std::cerr << "error (" << code_name(error.code) << "): " << error.message << '\n';
    return 1;
}

void print(const backend::CameraConfiguration& c) {
    std::cout << "  width=" << c.width << " height=" << c.height << " offset_x=" << c.offset_x
              << " offset_y=" << c.offset_y << " frame_rate_hz=" << c.frame_rate_hz
              << " exposure_ns=" << c.exposure.count() << '\n';
}

void versions() {
    std::cout << "Aravis runtime " << aravis::runtime_version() << ", compiled against "
              << aravis::compiled_version() << '\n';
}

template <class T> bool parse(std::string_view text, T& value) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size();
}

int inspect(backend::ICameraBackend& camera, std::string_view id) {
    versions();
    auto inspection = aravis::inspect(camera, id);
    if (!inspection) return report(inspection.error());
    const auto& i = inspection.value();
    std::cout << "Device " << i.device_id << " via " << i.interface_protocol << " (read-only inspection)\n";
    for (const auto& [name, value] : i.native) std::cout << "  " << name << ": " << value << '\n';
    if (i.capabilities) {
        const auto& c = *i.capabilities;
        std::cout << "Profile supported. Envelope: sensor " << c.sensor_width << 'x' << c.sensor_height
                  << ", increments W" << c.width_increment << " H" << c.height_increment << " X"
                  << c.offset_x_increment << " Y" << c.offset_y_increment << ", rate " << c.minimum_frame_rate_hz
                  << '-' << c.maximum_frame_rate_hz << " Hz, minimum exposure " << c.minimum_exposure.count() << " ns\n";
    }
    if (i.configuration) {
        std::cout << "Current configuration (truthful readback):\n";
        print(*i.configuration);
    }
    for (const auto& problem : i.problems) std::cout << "PROBLEM: " << problem << '\n';
    return i.problems.empty() ? 0 : 1;
}

// Argument validation for `apply`, before any device I/O.
std::optional<backend::CameraConfiguration> parse_apply(const std::vector<std::string_view>& args) {
    backend::CameraConfiguration requested;
    std::int64_t exposure_ns = 0;
    if (!parse(args[2], requested.width) || !parse(args[3], requested.height) || !parse(args[4], requested.offset_x) ||
        !parse(args[5], requested.offset_y) || !parse(args[6], requested.frame_rate_hz) || !parse(args[7], exposure_ns)) {
        std::cerr << "apply: every value must be a decimal integer in range\n" << usage;
        return std::nullopt;
    }
    requested.exposure = std::chrono::nanoseconds{exposure_ns};
    return requested;
}

int apply(backend::ICameraBackend& camera, std::string_view id, const backend::CameraConfiguration& requested) {
    versions();
    auto opened = camera.open(id);
    if (!opened) return report(opened.error());
    auto device = std::move(opened).value();
    auto actual = device->apply(requested);
    int status = 0;
    if (!actual) status = report(actual.error());
    else {
        const bool same = actual.value() == requested;
        std::cout << "Applied; actual readback" << (same ? "" : " DIFFERS from the request") << ":\n";
        print(actual.value());
        status = same ? 0 : 1;
    }
    (void)device->close();
    return status;
}

int capture(backend::ICameraBackend& camera, std::string_view id, const std::string& output) {
    versions();
    const auto outcome = aravis::capture_one(camera, id, output, aravis::default_capture_deadline);
    int status = 0;
    if (outcome.report) {
        const auto& r = *outcome.report;
        std::cout << "Captured device " << r.device_id << ": " << r.width << 'x' << r.height << " Mono8, source row stride "
                  << r.source_row_stride << " bytes, native frame ID " << r.native_frame_id << ", stream generation "
                  << r.stream_generation << "\n  timestamp: unqualified (FrameTimestamp invalid; no camera, UTC/PTP or "
                  << "integration-start claim)\n  configuration:\n";
        print(r.configuration);
        std::cout << "  statistics: delivered " << r.statistics.delivered_frames << ", incomplete "
                  << r.statistics.incomplete_frames << ", timeouts " << r.statistics.timeout_count << ", pool starvation "
                  << r.statistics.pool_starvation_events << "\n  wrote " << output << " (" << r.pgm_bytes
                  << " bytes, tightly packed P5)\n";
    }
    if (outcome.error) status = report(*outcome.error);
    if (outcome.cleanup_error) {
        std::cerr << "cleanup ";
        status = report(*outcome.cleanup_error);
    }
    return outcome.ok() ? 0 : (status ? status : 1);
}

int time_probe(backend::ICameraBackend& camera, std::string_view id, std::uint32_t count, const std::string& output) {
    versions();
    const auto outcome = aravis::time_probe(camera, id, count, output, aravis::time_probe_max_deadline);
    if (outcome.report) {
        const auto& r = *outcome.report;
        std::cout << "Observed " << r.frames.size() << " frame(s) from " << r.device_id << " via "
                  << r.context.native_device_type << "; wrote " << output << " (" << r.output_lines << " JSONL lines, "
                  << r.output_bytes << " bytes)\n  timing origin: " << aravis::timing_origin(r.context.native_interface)
                  << "\n  timestamp_qualified=false: " << aravis::timestamp_qualification_reason() << '\n';
    }
    int status = 0;
    if (outcome.error) status = report(*outcome.error);
    if (outcome.cleanup_error) {
        std::cerr << "cleanup ";
        status = report(*outcome.cleanup_error);
    }
    return outcome.ok() ? 0 : (status ? status : 1);
}

// ---- --discovery-interface (GIGE-MEL-013) ---------------------------------

constexpr std::string_view discovery_option = "--discovery-interface";

// Syntax only (no system or SDK call): a Linux interface name is 1..15 bytes
// (IFNAMSIZ - 1) without '/', ':' (alias syntax), whitespace or control
// characters, and may not begin with '-' (an option, never a name here).
bool well_formed_interface_name(std::string_view name) {
    if (name.empty() || name.size() >= IFNAMSIZ || name.front() == '-' || name == "." || name == "..") return false;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7f || c == '/' || c == ':') return false;
    }
    return true;
}

// The interface must exist (if_nametoindex), be UP and carry an IPv4
// address: exactly the interfaces the pinned SDK discovery enumerates
// (src/arvnetwork.c arv_enumerate_network_interfaces: IFF_UP and AF_INET).
// Read-only host queries; no socket is opened and no SDK function is called.
// Returns the IPv4 address(es) found, or an actionable error message.
bool usable_discovery_interface(const std::string& name, std::string& addresses, std::string& problem) {
    if (::if_nametoindex(name.c_str()) == 0) {
        problem = "network interface '" + name + "' does not exist (" + std::strerror(errno) +
                  "); list the host's interfaces with 'ip -br addr' and pass the one attached to the camera";
        return false;
    }
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        problem = std::string("cannot enumerate network interfaces: ") + std::strerror(errno);
        return false;
    }
    const std::unique_ptr<ifaddrs, void (*)(ifaddrs*)> owner(list, &::freeifaddrs);
    bool seen = false, up = false;
    for (const ifaddrs* entry = list; entry; entry = entry->ifa_next) {
        if (!entry->ifa_name || name != entry->ifa_name) continue;
        seen = true;
        if ((entry->ifa_flags & IFF_UP) == 0) continue;
        up = true;
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) continue;
        char text[INET_ADDRSTRLEN] = {};
        sockaddr_in address{};
        std::memcpy(&address, entry->ifa_addr, sizeof address);
        if (!::inet_ntop(AF_INET, &address.sin_addr, text, sizeof text)) continue;
        addresses += (addresses.empty() ? "" : ", ") + std::string(text);
    }
    if (!addresses.empty()) return true;
    if (!seen || !up) problem = "network interface '" + name + "' is not UP; bring it up or choose the interface attached to the camera";
    else problem = "network interface '" + name + "' has no IPv4 address; GigE Vision discovery uses IPv4 only";
    return false;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) {
        std::cout << usage; // no SDK call, no device I/O
        return 2;
    }
    // Optional, default-off discovery restriction: before COMMAND, at most once.
    std::optional<std::string> discovery_interface;
    while (!args.empty() && args[0].substr(0, discovery_option.size()) == discovery_option) {
        if (args[0] != discovery_option) {
            std::cerr << "malformed option '" << args[0] << "': use '" << discovery_option << " IFNAME'\n" << usage;
            return 2;
        }
        if (discovery_interface) {
            std::cerr << discovery_option << " was given more than once; pass exactly one interface\n" << usage;
            return 2;
        }
        if (args.size() < 2 || args[1].empty() || args[1].front() == '-') {
            std::cerr << discovery_option << " requires a network interface name (IFNAME), e.g. lo or the camera NIC\n"
                      << usage;
            return 2;
        }
        if (!well_formed_interface_name(args[1])) {
            std::cerr << discovery_option << ": '" << args[1] << "' is not a valid network interface name (1-"
                      << (IFNAMSIZ - 1) << " bytes; no '/', ':', whitespace or control characters)\n" << usage;
            return 2;
        }
        discovery_interface = std::string(args[1]);
        args.erase(args.begin(), args.begin() + 2);
    }
    if (args.empty()) {
        std::cerr << "missing COMMAND\n" << usage;
        return 2;
    }
    if (args[0] == "--help" || args[0] == "-h") {
        std::cout << usage; // no SDK call, no device I/O
        return 0;
    }
    for (const auto& arg : args) {
        if (arg.substr(0, discovery_option.size()) == discovery_option) {
            std::cerr << discovery_option << " must precede COMMAND\n" << usage;
            return 2;
        }
    }

    // Usage validation of COMMAND and its arguments, before any SDK call.
    std::optional<backend::CameraConfiguration> apply_request;
    std::uint32_t count = 0;
    const auto& command = args[0];
    if (command == "list" && args.size() == 1) {
    } else if (command == "inspect" && args.size() == 2) {
    } else if (command == "apply" && args.size() == 8) {
        apply_request = parse_apply(args);
        if (!apply_request) return 2;
    } else if (command == "capture" && args.size() == 3) {
        if (args[1].empty() || args[2].empty()) {
            std::cerr << "capture: DEVICE_ID and OUTPUT.pgm must be non-empty\n" << usage;
            return 2;
        }
    } else if (command == "time-probe" && args.size() == 4) {
        if (args[1].empty() || args[3].empty() || !parse(args[2], count) || count < 1 ||
            count > aravis::time_probe_max_frames) {
            std::cerr << "time-probe: DEVICE_ID and OUTPUT.jsonl must be non-empty and COUNT a decimal integer in [1, "
                      << aravis::time_probe_max_frames << "]\n" << usage;
            return 2;
        }
    } else {
        std::cerr << usage;
        return 2;
    }

    if (discovery_interface) {
        // Verified BEFORE the SDK is touched: the setter itself returns no
        // status, so calling it proves nothing about the interface.
        std::string addresses, problem;
        if (!usable_discovery_interface(*discovery_interface, addresses, problem)) {
            std::cerr << "error (discovery_interface): " << problem
                      << "; no device I/O was performed and no other interface is tried\n";
            return 1;
        }
        // Process-local SDK setting, applied before the first enumeration,
        // discovery or open of this process, then read back.
        arv_gv_interface_set_discovery_interface_name(discovery_interface->c_str());
        std::unique_ptr<char, decltype(&g_free)> applied(arv_gv_interface_dup_discovery_interface_name(), &g_free);
        if (!applied || *discovery_interface != applied.get()) {
            std::cerr << "error (discovery_interface): the SDK did not retain discovery interface '" << *discovery_interface
                      << "'; no device I/O was performed\n";
            return 1;
        }
        std::cout << "GigE Vision discovery restricted to interface " << *discovery_interface << " (UP, IPv4 "
                  << addresses << "); no fallback to other interfaces\n";
    }

    auto camera = aravis::make_backend(); // GigE Vision interface only; no I/O yet
    if (command == "list") {
        versions();
        auto devices = camera->enumerate();
        if (!devices) return report(devices.error());
        std::cout << devices.value().size() << " GigE Vision device(s)\n";
        for (const auto& device : devices.value()) std::cout << "  " << device.device_id << "  " << device.display_name << '\n';
        return 0;
    }
    if (command == "inspect") return inspect(*camera, args[1]);
    if (command == "apply") return apply(*camera, args[1], *apply_request);
    if (command == "capture") return capture(*camera, args[1], std::string(args[2]));
    return time_probe(*camera, args[1], count, std::string(args[3]));
}
