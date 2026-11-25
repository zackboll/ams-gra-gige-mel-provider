// GIGE-MEL-GENTL-001 diagnostic: explicit, uninstalled GenTL producer probe.
// Nothing here runs from static initialization. Every step that can reach a
// device is a separate command-line request; configuration writes, register
// writes and remote-device AcquisitionStart are not implemented at all.
// No image is published through MEL. Exit: 0 ok, 1 failure, 2 usage.
#include "transport/gentl/gentl_consumer.hpp"
#include "gentl_wire_preflight.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace gentl = gige_mel::gentl;
using namespace std::chrono_literals;

namespace {

constexpr const char* usage_text =
    "usage: gige_mel_gentl_probe PRODUCER.cti COMMAND [ARGS]\n"
    "  PRODUCER.cti must be an explicit absolute path. Commands:\n"
    "  load                        dlopen, validate symbols, GCInitLib, GCGetInfo, close, unload\n"
    "  interfaces                  + TLOpen and list host interfaces (no device discovery)\n"
    "  wire-readonly IFACE QUIET_MS isolated simulator qualification only; exact singleton\n"
    "                                discovery, READONLY open, reads, checked close, quiet wait\n"
    "                                requires supervisor-inherited --origin-netns-fd FD and kernel preflight\n"
    "  discover IFACE              + IFUpdateDeviceList on exactly IFACE (network discovery)\n"
    "  inspect IFACE DEVICE [--access readonly|control]\n"
    "                              + discover, open exactly DEVICE (default readonly), read-only\n"
    "                                module/port/XML-location facts and the first XML bytes\n"
    "  acquire IFACE DEVICE FRAMES [--reopen N] --allow-host-acquisition\n"
    "                              + host engine only (DSStartAcquisition). Never executes the\n"
    "                                remote AcquisitionStart or writes features; a device that is\n"
    "                                not already streaming delivers nothing (reported as timeouts).\n"
    "  --repeat N (before COMMAND) repeats the whole load..unload cycle N times\n"
    "  --offline-qualification (before COMMAND) restricts to load/interfaces, fails on interface\n"
    "                                update errors, exits without destructor retry after close failure\n"
    "  --no-interface-info (before COMMAND) skips TLGetInterfaceInfo, which faults in some producers\n";

void print(std::string_view indent, const std::vector<gentl::Property>& properties) {
    for (const auto& property : properties)
        std::cout << indent << property.name << " = "
                  << (property.text ? property.text.value() : "<" + property.text.error().message() + ">") << '\n';
}

int fail(const gentl::Failure& failure) {
    std::cout << "FAILED: " << failure.message() << '\n';
    return 1;
}

bool parse(std::string_view text, unsigned& value) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

struct Request {
    std::string producer, command, interface_id, device_id;
    unsigned frames{0}, reopen{1}, repeat{1};
    int origin_fd{-1};
    bool interface_info{true};
    bool offline_qualification{false};
    gentl::Access access{gentl::Access::read_only};
    bool allow_acquisition{false};
};

int acquire(gentl::Device& device, const Request& request) {
    auto ids = device.stream_ids();
    if (!ids) return fail(ids.error());
    if (ids.value().empty()) { std::cout << "FAILED: device exposes no data stream\n"; return 1; }
    for (unsigned cycle = 1; cycle <= request.reopen; ++cycle) {
        auto stream = device.open_stream(ids.value().front(), {});
        if (!stream) return fail(stream.error());
        std::cout << "stream " << ids.value().front() << " cycle " << cycle << ":\n";
        print("  ", stream.value()->info());
        if (auto s = stream.value()->start(); !s) {
            std::cout << "  start: " << s.error().message() << '\n';
            auto closed = stream.value()->close();
            if (!closed) return fail(closed.error());
            return 1;
        }
        unsigned received = 0, mono8 = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned attempt = 0; received < request.frames && attempt < request.frames + 5; ++attempt) {
            auto frame = stream.value()->wait(2000ms);
            if (!frame) { std::cout << "  wait: " << frame.error().message() << '\n'; continue; }
            const auto& f = frame.value().facts();
            ++received;
            if (f.mono8()) ++mono8;
            std::cout << "  frame id=" << (f.frame_id ? std::to_string(*f.frame_id) : "?")
                      << " incomplete=" << (f.incomplete ? (*f.incomplete ? "yes" : "no") : "?")
                      << " bytes=" << frame.value().bytes().size() << " capacity=" << f.capacity
                      << " geometry=" << (f.width ? std::to_string(*f.width) : "?") << 'x'
                      << (f.height ? std::to_string(*f.height) : "?") << " mono8=" << (f.mono8() ? "yes" : "no")
                      << " producer_timestamp=" << (f.timestamp_ns ? std::to_string(*f.timestamp_ns) + "ns" :
                                                    f.timestamp_ticks ? std::to_string(*f.timestamp_ticks) + "ticks" : "?")
                      << " (clock domain UNKNOWN; not host, UTC or integration-start time)\n";
            for (const auto& note : f.notes) std::cout << "    note: " << note << '\n';
        }
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        const auto counters = stream.value()->counters();
        std::cout << "  received=" << received << " mono8=" << mono8 << " delivered=" << counters.delivered
                  << " incomplete=" << counters.incomplete << " timeouts=" << counters.timeouts
                  << " starvation=" << counters.starvation << " elapsed_host_s=" << elapsed << '\n';
        if (auto s = stream.value()->close(); !s) return fail(s.error());
        if (received < request.frames) return 1;
    }
    return 0;
}
void print_xml_head(gentl::Device& device, const std::vector<std::vector<gentl::Property>>& locations) {
    for (const auto& location : locations) {
        const auto* url = gentl::find(location, "URL_INFO_URL");
        if (!url || !url->text) continue;
        const auto local = gentl::parse_local_url(url->text.value());
        if (!local) { std::cout << "  xml: not a Local: register URL; not read\n"; continue; }
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(local->length, 1u << 20));
        std::vector<std::byte> bytes(length);
        if (auto s = device.read_port(local->address, bytes); !s) {
            std::cout << "  xml read: " << s.error().message() << '\n';
            continue;
        }
        std::string head;
        for (std::size_t i = 0; i < std::min<std::size_t>(bytes.size(), 96); ++i) {
            const auto c = std::to_integer<unsigned char>(bytes[i]);
            head += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.';
        }
        const bool zip = bytes.size() >= 2 && bytes[0] == std::byte{'P'} && bytes[1] == std::byte{'K'};
        std::cout << "  xml file=" << local->filename << " address=0x" << std::hex << local->address << std::dec
                  << " length=" << local->length << " read=" << bytes.size() << (zip ? " format=zip" : "")
                  << "\n  xml head: " << head << '\n';
        return; // first readable location only
    }
}

int inspect(gentl::Interface& interface, const Request& request) {
    auto device = interface.open_device(request.device_id, request.access);
    if (!device) return fail(device.error());
    std::cout << "  opened " << request.device_id << " (requested access "
              << (request.access == gentl::Access::read_only ? "READONLY" : "CONTROL") << ")\n";
    print("    module ", device.value()->info());
    print("    port ", device.value()->port_info());
    auto order = device.value()->port_byte_order();
    std::cout << "    declared port byte order: "
              << (order ? (order.value() == gentl::PortByteOrder::big_endian ? "big" : "little")
                        : order.error().message()) << '\n';
    auto locations = device.value()->xml_locations();
    if (!locations) std::cout << "    xml locations: " << locations.error().message() << '\n';
    else {
        for (std::size_t i = 0; i < locations.value().size(); ++i) {
            std::cout << "    xml location " << i << '\n';
            print("      ", locations.value()[i]);
        }
        print_xml_head(*device.value(), locations.value());
    }
    const int status = request.command == "acquire" ? acquire(*device.value(), request) : 0;
    if (auto s = device.value()->close(); !s) return fail(s.error());
    return status;
}

int session(const Request& request) {
    std::cout << "BEGIN Producer::load (dlopen, function table, GCInitLib)\n";
    auto producer = gentl::Producer::load(request.producer);
    if (!producer) return fail(producer.error());
    std::cout << "producer " << request.producer << " loaded and initialized\n";
    print("  ", producer.value()->info());
    for (const auto& missing : producer.value()->missing_optional())
        std::cout << "  optional symbol absent: " << missing << '\n';
    std::cout << "  port write available=" << producer.value()->has_port_write()
              << " acquisition available=" << producer.value()->has_acquisition() << '\n';
    int status = 0;
    if (request.command != "load") {
        std::cout << "BEGIN TLOpen\n";
        auto system = producer.value()->open_system();
        if (!system) return fail(system.error());
        std::cout << "END TLOpen; BEGIN TLGetInfo\n";
        print("  system ", system.value()->info());
        std::cout << "BEGIN TLUpdateInterfaceList\n";
        if (auto s = system.value()->update_interfaces(1000ms); !s) {
            std::cout << "  " << s.error().message() << '\n';
            if (request.offline_qualification) status = 1;
        }
        std::cout << "END TLUpdateInterfaceList\n";
        if (status) {
            std::cout << "BEGIN TLClose after update failure\n";
            if (auto s = system.value()->close(); !s) std::_Exit(fail(s.error()));
            std::cout << "BEGIN GCCloseLib after update failure\n";
            if (auto s = producer.value()->close(); !s) std::_Exit(fail(s.error()));
            return status;
        }
        std::cout << "BEGIN TLGetNumInterfaces/TLGetInterfaceID/TLGetInterfaceInfo\n";
        auto interfaces = system.value()->interfaces(request.interface_info);
        if (!interfaces) {
            const int failed = fail(interfaces.error());
            if (request.offline_qualification) {
                std::cout << "BEGIN TLClose after enumeration failure\n";
                if (auto s = system.value()->close(); !s) std::_Exit(fail(s.error()));
                std::cout << "BEGIN GCCloseLib after enumeration failure\n";
                if (auto s = producer.value()->close(); !s) std::_Exit(fail(s.error()));
            }
            return failed;
        }
        for (const auto& description : interfaces.value()) {
            std::cout << "  interface " << description.id << '\n';
            print("    ", description.properties);
        }
        if (request.command != "interfaces") {
            auto interface = system.value()->open_interface(request.interface_id);
            if (!interface) return fail(interface.error());
            if (auto s = interface.value()->update_devices(2000ms); !s) return fail(s.error());
            auto devices = interface.value()->devices();
            if (!devices) return fail(devices.error());
            std::cout << "  " << devices.value().size() << " device(s) on " << request.interface_id << '\n';
            for (const auto& description : devices.value()) {
                std::cout << "  device " << description.id << '\n';
                print("    ", description.properties);
            }
            if (request.command == "inspect" || request.command == "acquire") status = inspect(*interface.value(), request);
            if (auto s = interface.value()->close(); !s) return fail(s.error());
        }
        std::cout << "BEGIN TLClose\n";
        if (auto s = system.value()->close(); !s) {
            const int failed = fail(s.error());
            if (request.offline_qualification) std::_Exit(failed);
            return failed;
        }
        std::cout << "END TLClose\n";
    }
    std::cout << "BEGIN GCCloseLib\n";
    if (auto s = producer.value()->close(); !s) {
        const int failed = fail(s.error());
        if (request.offline_qualification) std::_Exit(failed);
        return failed;
    }
    std::cout << "END GCCloseLib; native library unmap follows session destruction\n";
    std::cout << "closed in order: device, interface, system, library\n";
    return status;
}

// Subprocess-only qualification path. Never unwind an uncertain native operation:
// the supervisor records failure and the OS retires this process, not native cleanup.
void wire_marker(std::string_view stage) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::cout << "WIRE " << ns << ' ' << stage << '\n';
}
template<class T> T wire_take(gentl::Result<T> result) {
    if (!result) std::_Exit(fail(result.error()));
    return std::move(result).value();
}
void wire_check(gentl::Status status) {
    if (!status) std::_Exit(fail(status.error()));
}
int wire_session(const Request& request) {
    try { wire_preflight::check(request.origin_fd); }
    catch (const std::exception& error) {
        std::cout << "ISOLATION_REFUSED: " << error.what() << '\n';
        return 1;
    }
    ::close(request.origin_fd);
    wire_marker("isolation_verified");
    wire_marker("load");
    auto producer = wire_take(gentl::Producer::load(request.producer));
    print("producer ", producer->info());
    wire_marker("system");
    auto system = wire_take(producer->open_system());
    wire_check(system->update_interfaces(1000ms));
    auto interfaces = wire_take(system->interfaces());
    bool listed = false;
    for (const auto& item : interfaces) if (item.id == request.interface_id) listed = true;
    if (!listed) { std::cout << "FAILED: exact interface not listed\n"; std::_Exit(1); }
    wire_marker("discovery");
    auto interface = wire_take(system->open_interface(request.interface_id));
    wire_check(interface->update_devices(2000ms));
    auto devices = wire_take(interface->devices());
    if (devices.size() != 1) { std::cout << "FAILED: expected exactly one simulated device\n"; std::_Exit(1); }
    std::cout << "exact discovered device " << devices.front().id << '\n';
    wire_marker("open_readonly");
    auto device = wire_take(interface->open_device(devices.front().id, gentl::Access::read_only));
    wire_marker("inspect");
    print("module ", device->info());
    print("port ", device->port_info());
    auto locations = device->xml_locations();
    if (locations) print_xml_head(*device, locations.value());
    else std::cout << "XML unsupported: " << locations.error().message() << '\n';
    // Selected simulator feature: TestRegister in the pinned fake-camera XML.
    // No GenApi conversion or feature semantics are introduced by this diagnostic.
    std::vector<std::byte> value(4);
    wire_marker("required_read_attempt");
    auto read = device->read_port(0x1f0, value);
    std::cout << "TestRegister raw read: " << (read ? "success" : read.error().message()) << '\n';
    if (!read) {
        std::cout << "REQUIRED_READ_ERROR native=" << read.error().native << " function=" << read.error().function
                  << " diagnostic=" << read.error().detail << '\n';
    }
    wire_marker(read ? "required_read_success" : "required_read_failed");
    wire_marker("close_device");
    wire_check(device->close());
    wire_marker("device_closed");
    device.reset();
    wire_marker("close_interface");
    wire_check(interface->close());
    wire_marker("interface_closed");
    interface.reset();
    wire_marker("close_system");
    wire_check(system->close());
    wire_marker("system_closed");
    system.reset();
    wire_marker("close_producer");
    wire_check(producer->close());
    producer.reset(); // dlclose is inside the bounded subprocess, before quietness
    if (gentl::retained_producers()) { std::cout << "FAILED: retained producer\n"; std::_Exit(1); }
    wire_marker("native_closed");
    std::this_thread::sleep_for(std::chrono::milliseconds(request.frames));
    wire_marker("quiet_complete");
    return read ? 0 : 1;
}
int parse_request(const std::vector<std::string_view>& args, Request& request) {
    if (args.size() < 2) return 2;
    request.producer = std::string(args[0]);
    std::size_t at = 1;
    if (args[at] == "--repeat") {
        if (args.size() < at + 3 || !parse(args[at + 1], request.repeat) || request.repeat == 0 || request.repeat > 100)
            return 2;
        at += 2;
    }
    if (at < args.size() && args[at] == "--no-interface-info") {
        request.interface_info = false;
        ++at;
    }
    if (at < args.size() && args[at] == "--offline-qualification") {
        request.offline_qualification = true;
        ++at;
    }
    if (at >= args.size()) return 2;
    request.command = std::string(args[at++]);
    if (request.offline_qualification && request.command != "load" && request.command != "interfaces") return 2;
    const auto take = [&](std::string& out) {
        if (at >= args.size()) return false;
        out = std::string(args[at++]);
        return true;
    };
    if (request.command == "wire-readonly") {
        if (!take(request.interface_id) || at >= args.size() || !parse(args[at++], request.frames) ||
            request.frames < 100 || request.frames > 10000 || request.repeat != 1 ||
            !request.interface_info) return 2;
        if (at < args.size()) {
            unsigned fd{};
            if (args[at++] != "--origin-netns-fd" || at >= args.size() || !parse(args[at++], fd) || fd > 1024 ||
                at != args.size()) return 2;
            request.origin_fd = static_cast<int>(fd);
        }
        return 0;
    }
    if (request.command == "load" || request.command == "interfaces") return args.size() == at ? 0 : 2;
    if (request.command != "discover" && request.command != "inspect" && request.command != "acquire") return 2;
    if (!take(request.interface_id)) return 2;
    if (request.command != "discover" && !take(request.device_id)) return 2;
    if (request.command == "acquire") {
        if (at >= args.size() || !parse(args[at], request.frames) || request.frames == 0 || request.frames > 10000)
            return 2;
        ++at;
    }
    while (at < args.size()) {
        const auto option = args[at++];
        if (option == "--allow-host-acquisition" && request.command == "acquire") {
            request.allow_acquisition = true;
        } else if (option == "--reopen" && request.command == "acquire" && at < args.size() &&
                   parse(args[at], request.reopen) && request.reopen >= 1 && request.reopen <= 100) {
            ++at;
        } else if (option == "--access" && request.command == "inspect" && at < args.size()) {
            const auto value = args[at++];
            if (value == "readonly") request.access = gentl::Access::read_only;
            else if (value == "control") request.access = gentl::Access::control;
            else return 2;
        } else {
            return 2;
        }
    }
    if (request.command == "acquire") {
        if (!request.allow_acquisition) {
            std::cerr << "acquire requires the explicit --allow-host-acquisition flag\n";
            return 2;
        }
        request.access = gentl::Access::control; // host stream channel setup needs control
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Keep the last operation marker even if a proprietary producer crashes.
    std::cout << std::unitbuf;
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() == 1 && args[0] == "--help") { std::cout << usage_text; return 0; }
    Request request;
    if (parse_request(args, request)) { std::cerr << usage_text; return 2; }
    if (request.command == "wire-readonly") return wire_session(request);
    int result = 0;
    for (unsigned cycle = 1; cycle <= request.repeat; ++cycle) {
        if (request.repeat > 1) std::cout << "=== cycle " << cycle << " of " << request.repeat << " ===\n";
        result = session(request);
        if (result) break;
    }
    if (const auto kept = gentl::retained_producers(); kept)
        std::cout << "WARNING: " << kept << " producer(s) retained mapped (a native close failed; producer state unknown)\n";
    std::cout << "exit status " << result << '\n';
    return result;
}
