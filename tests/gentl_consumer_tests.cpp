// GIGE-MEL-GENTL-001 offline contract for the GenTL consumer foundation.
// Drives the REAL dlopen/dlsym consumer against a controlled fixture producer
// built from the verified EMVA header. Fixture success qualifies ownership and
// error handling only; it is not evidence for any real producer or camera.
#include "gentl/gentl_consumer.hpp"

#include <dlfcn.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace gentl = gige_mel::gentl;
namespace reg = gige_mel::backend::device_registers;
using gige_mel::backend::ErrorCode;
using namespace std::chrono_literals;

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__ ":") + std::to_string(__LINE__) + ": " #x); } while (false)

namespace {

constexpr std::int32_t gc_io = -1010, gc_resource_in_use = -1004, gc_access_denied = -1005;

// Per-test environment, call log and loader observation.
class Fixture {
public:
    explicit Fixture(std::vector<std::pair<std::string, std::string>> env = {}) {
        char name[] = "/tmp/gentl-mock-log-XXXXXX";
        const int fd = mkstemp(name);
        CHECK(fd >= 0);
        close(fd);
        log_ = name;
        setenv("GENTL_MOCK_LOG", log_.c_str(), 1);
        for (const char* key : {"GENTL_MOCK_FAIL", "GENTL_MOCK_PORT_ENDIAN", "GENTL_MOCK_DEFINES_PAYLOAD",
                                "GENTL_MOCK_STALL"})
            unsetenv(key);
        for (const auto& [key, value] : env) setenv(key.c_str(), value.c_str(), 1);
    }
    ~Fixture() { std::remove(log_.c_str()); }
    std::vector<std::string> calls() const {
        std::ifstream file(log_);
        std::vector<std::string> lines;
        for (std::string line; std::getline(file, line);) lines.push_back(line);
        return lines;
    }
    bool called(std::string_view prefix) const {
        for (const auto& line : calls()) if (line.rfind(prefix, 0) == 0) return true;
        return false;
    }
    std::size_t count(std::string_view prefix) const {
        std::size_t n = 0;
        for (const auto& line : calls()) if (line.rfind(prefix, 0) == 0) ++n;
        return n;
    }
    // Index of the first line with prefix, after `from`; npos when absent.
    std::size_t index(std::string_view prefix, std::size_t from = 0) const {
        const auto lines = calls();
        for (std::size_t i = from; i < lines.size(); ++i) if (lines[i].rfind(prefix, 0) == 0) return i;
        return std::string::npos;
    }
private:
    std::string log_;
};

// True only while the producer is actually mapped in this process.
bool mapped(const char* path) {
    void* handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return false;
    dlclose(handle);
    return true;
}

struct Opened {
    std::shared_ptr<gentl::Producer> producer;
    std::shared_ptr<gentl::System> system;
    std::shared_ptr<gentl::Interface> interface;
    std::shared_ptr<gentl::Device> device;
};

Opened open_all(const char* path = GENTL_MOCK, const char* device = "mock-dev-A") {
    Opened o;
    auto producer = gentl::Producer::load(path);
    if (!producer) throw std::runtime_error(producer.error().message());
    o.producer = producer.value();
    auto system = o.producer->open_system();
    CHECK(system);
    o.system = system.value();
    auto interface = o.system->open_interface("mock-if-0");
    CHECK(interface);
    o.interface = interface.value();
    CHECK(o.interface->update_devices(100ms));
    auto opened = o.interface->open_device(device, gentl::Access::read_only);
    if (!opened) throw std::runtime_error(opened.error().message());
    o.device = opened.value();
    return o;
}

void close_all(Opened& o) {
    CHECK(o.device->close());
    CHECK(o.interface->close());
    CHECK(o.system->close());
    CHECK(o.producer->close());
}
void invalid_paths() {
    for (const std::string path : {"", "relative/producer.cti", "/nonexistent/gige-mel/producer.cti", "/tmp"}) {
        auto loaded = gentl::Producer::load(path);
        CHECK(!loaded);
        CHECK(loaded.error().native == 0);
        CHECK(loaded.error().code == (path.empty() || path[0] != '/' ? ErrorCode::invalid_argument : ErrorCode::not_found));
    }
    // A readable file that is not a shared object: the dlopen diagnostic is kept.
    char name[] = "/tmp/gentl-not-elf-XXXXXX";
    const int fd = mkstemp(name);
    CHECK(fd >= 0 && write(fd, "not a producer", 14) == 14);
    close(fd);
    auto loaded = gentl::Producer::load(name);
    std::remove(name);
    CHECK(!loaded && loaded.error().function == "dlopen" && !loaded.error().detail.empty());
    CHECK(gentl::retained_producers() == 0);
}

void missing_mandatory_symbol() {
    Fixture fixture;
    auto loaded = gentl::Producer::load(GENTL_MOCK_MISSING);
    CHECK(!loaded);
    CHECK(loaded.error().code == ErrorCode::unsupported);
    CHECK(loaded.error().detail.find("TLOpen") != std::string::npos);
    CHECK(fixture.called("loaded"));
    CHECK(!fixture.called("GCInitLib")); // validated before ANY producer call
    CHECK(fixture.called("unloaded"));
    CHECK(!mapped(GENTL_MOCK_MISSING));
}

void initialization_failure() {
    Fixture fixture({{"GENTL_MOCK_FAIL", "GCInitLib=-1005"}});
    auto loaded = gentl::Producer::load(GENTL_MOCK);
    CHECK(!loaded);
    CHECK(loaded.error().native == gc_access_denied);
    CHECK(loaded.error().function == "GCInitLib");
    CHECK(loaded.error().code == ErrorCode::busy);
    CHECK(!fixture.called("GCCloseLib")); // never initialized: never closed
    CHECK(fixture.called("unloaded"));
    CHECK(!mapped(GENTL_MOCK));
}

void lifecycle_order_and_repetition() {
    Fixture fixture;
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto o = open_all();
        CHECK(mapped(GENTL_MOCK));
        const auto info = o.producer->info();
        const auto* id = gentl::find(info, "TL_INFO_ID");
        CHECK(id && id->text && id->text.value() == "gige-mel-gentl-mock");
        const auto* name = gentl::find(info, "TL_INFO_NAME");
        CHECK(name && !name->text && name->text.error().native == -1003); // failure kept, never guessed
        close_all(o);
        CHECK(o.producer->close()); // idempotent double close: no second native call
        CHECK(o.system->close() && o.interface->close() && o.device->close());
        CHECK(mapped(GENTL_MOCK)); // still owned by `o`
    }
    CHECK(!mapped(GENTL_MOCK));
    CHECK(fixture.count("GCInitLib") == 5 && fixture.count("GCCloseLib") == 5);
    CHECK(fixture.count("loaded") == 5 && fixture.count("unloaded") == 5);
    // Order per cycle: device, interface, system, library, unload.
    CHECK(fixture.index("DevClose mock-dev-A") < fixture.index("IFClose"));
    CHECK(fixture.index("IFClose") < fixture.index("TLClose"));
    CHECK(fixture.index("TLClose") < fixture.index("GCCloseLib"));
    CHECK(fixture.index("GCCloseLib") < fixture.index("unloaded"));
    CHECK(gentl::retained_producers() == 0);
}

void parent_close_refused_while_children_open() {
    Fixture fixture;
    auto o = open_all();
    auto refused = o.producer->close();
    CHECK(!refused && refused.error().native == 0 && refused.error().code == ErrorCode::busy);
    CHECK(!o.interface->close() && !o.system->close());
    CHECK(!fixture.called("GCCloseLib") && !fixture.called("IFClose") && !fixture.called("TLClose"));
    close_all(o);
}

void destruction_order_without_explicit_close() {
    Fixture fixture;
    {
        auto o = open_all();
        // Drop the caller's parent references first: children keep them alive.
        o.producer.reset();
        o.system.reset();
        o.interface.reset();
        CHECK(mapped(GENTL_MOCK));
        CHECK(!fixture.called("GCCloseLib"));
    }
    CHECK(fixture.index("DevClose") != std::string::npos);
    CHECK(fixture.index("DevClose") < fixture.index("IFClose"));
    CHECK(fixture.index("IFClose") < fixture.index("TLClose"));
    CHECK(fixture.index("TLClose") < fixture.index("GCCloseLib"));
    CHECK(fixture.index("GCCloseLib") < fixture.index("unloaded"));
    CHECK(!mapped(GENTL_MOCK));
}
void exact_selection() {
    Fixture fixture;
    auto producer = gentl::Producer::load(GENTL_MOCK);
    CHECK(producer);
    auto system = producer.value()->open_system();
    CHECK(system);
    CHECK(!system.value()->open_interface("mock-if"));   // prefix is not selection
    CHECK(!system.value()->open_interface("MOCK-IF-0")); // no case folding
    auto interface = system.value()->open_interface("mock-if-0");
    CHECK(interface);
    // Before explicit discovery the list is empty: no implicit IFUpdateDeviceList.
    auto unlisted = interface.value()->open_device("mock-dev-A", gentl::Access::read_only);
    CHECK(!unlisted && unlisted.error().code == ErrorCode::not_found && unlisted.error().native == 0);
    CHECK(!fixture.called("IFUpdateDeviceList") && !fixture.called("IFOpenDevice"));
    CHECK(interface.value()->update_devices(100ms));
    auto devices = interface.value()->devices();
    CHECK(devices && devices.value().size() == 2);
    const auto* serial = gentl::find(devices.value()[1].properties, "DEVICE_INFO_SERIAL_NUMBER");
    CHECK(serial && serial->text && serial->text.value() == "SN-B");
    for (const auto* wrong : {"mock-dev", "mock-dev-a", "SN-B", "mock-dev-B "}) {
        auto refused = interface.value()->open_device(wrong, gentl::Access::read_only);
        CHECK(!refused && refused.error().code == ErrorCode::not_found);
    }
    CHECK(!fixture.called("IFOpenDevice"));
    auto b = interface.value()->open_device("mock-dev-B", gentl::Access::read_only);
    CHECK(b && b.value()->id() == "mock-dev-B");
    CHECK(fixture.called("IFOpenDevice mock-dev-B access=2")); // DEVICE_ACCESS_READONLY
    CHECK(!fixture.called("IFOpenDevice mock-dev-A"));
    CHECK(b.value()->close() && interface.value()->close() && system.value()->close() && producer.value()->close());
}

void partial_open_failure_cleans_up() {
    Fixture fixture({{"GENTL_MOCK_FAIL", "DevGetPort=-1010"}});
    auto producer = gentl::Producer::load(GENTL_MOCK);
    CHECK(producer);
    auto system = producer.value()->open_system();
    auto interface = system.value()->open_interface("mock-if-0");
    CHECK(interface && interface.value()->update_devices(100ms));
    auto device = interface.value()->open_device("mock-dev-A", gentl::Access::read_only);
    CHECK(!device && device.error().function == "DevGetPort" && device.error().native == gc_io);
    CHECK(device.error().detail.find("injected failure in DevGetPort") != std::string::npos); // GCGetLastError
    CHECK(fixture.called("DevClose mock-dev-A")); // the opened device is closed again
    // The interface counts no device, so a normal close succeeds.
    CHECK(interface.value()->close() && system.value()->close() && producer.value()->close());
}

void native_error_propagation() {
    Fixture fixture({{"GENTL_MOCK_FAIL", "TLOpenInterface=-1004"}});
    auto producer = gentl::Producer::load(GENTL_MOCK);
    auto system = producer.value()->open_system();
    auto interface = system.value()->open_interface("mock-if-0");
    CHECK(!interface && interface.error().native == gc_resource_in_use && interface.error().code == ErrorCode::busy);
    CHECK(interface.error().message().find("GC_ERR_RESOURCE_IN_USE (-1004)") != std::string::npos);
    CHECK(interface.error().message().find("mock: injected failure in TLOpenInterface") != std::string::npos);
    CHECK(gentl::gc_error_name(-1023) == "GC_ERR_AMBIGUOUS" && gentl::gc_error_name(-10001) == "GC_ERR_CUSTOM");
    CHECK(system.value()->close() && producer.value()->close());
}

void one_instance_per_producer_file() {
    Fixture fixture;
    auto first = gentl::Producer::load(GENTL_MOCK_MINIMAL_CTI);
    CHECK(first);
    auto second = gentl::Producer::load(GENTL_MOCK_MINIMAL_CTI);
    CHECK(!second && second.error().code == ErrorCode::busy && second.error().native == 0);
    CHECK(fixture.count("GCInitLib") == 1);
    CHECK(first.value()->close());
    first.value().reset();
    auto third = gentl::Producer::load(GENTL_MOCK_MINIMAL_CTI);
    CHECK(third);
    CHECK(third.value()->close());
}

void optional_entry_points_are_all_or_nothing() {
    Fixture fixture;
    auto producer = gentl::Producer::load(GENTL_MOCK_MINIMAL_CTI);
    CHECK(producer);
    CHECK(!producer.value()->has_port_write() && !producer.value()->has_acquisition());
    const auto& missing = producer.value()->missing_optional();
    CHECK(std::find(missing.begin(), missing.end(), "GCWritePort") != missing.end());
    CHECK(std::find(missing.begin(), missing.end(), "DSStartAcquisition") != missing.end());
    auto system = producer.value()->open_system();
    auto interface = system.value()->open_interface("mock-if-0");
    CHECK(interface.value()->update_devices(100ms));
    auto device = interface.value()->open_device("mock-dev-A", gentl::Access::read_only);
    CHECK(device);
    CHECK(!device.value()->stream_ids());
    auto written = device.value()->with_registers([](reg::IDeviceRegisters& r) { return r.write_register(0x100, 1); });
    CHECK(!written && written.error().diagnostic.code == ErrorCode::unsupported);
    CHECK(written.error().diagnostic.message.find("no native write was made") != std::string::npos);
    CHECK(device.value()->close() && interface.value()->close() && system.value()->close() && producer.value()->close());
}
void port_inspection_and_xml_location() {
    Fixture fixture;
    auto o = open_all();
    auto order = o.device->port_byte_order();
    CHECK(order && order.value() == gentl::PortByteOrder::big_endian);
    auto locations = o.device->xml_locations();
    CHECK(locations && locations.value().size() == 1);
    const auto* url = gentl::find(locations.value()[0], "URL_INFO_URL");
    CHECK(url && url->text);
    const auto local = gentl::parse_local_url(url->text.value());
    CHECK(local && local->filename == "mock.xml" && local->address == 0x1000);
    std::vector<std::byte> xml(static_cast<std::size_t>(local->length));
    CHECK(o.device->read_port(local->address, xml));
    const std::string text(reinterpret_cast<const char*>(xml.data()), xml.size());
    CHECK(text.rfind("<?xml", 0) == 0 && text.find("<IntReg Name=\"Width\"") != std::string::npos);
    // Out-of-range and empty reads: native error preserved / refused before the call.
    std::array<std::byte, 8> tail{};
    auto beyond = o.device->read_port(0x1ffc, tail);
    CHECK(!beyond && beyond.error().native == -1015); // GC_ERR_INVALID_ADDRESS
    CHECK(!o.device->read_port(0, std::span<std::byte>{}));
    CHECK(gentl::parse_local_url("Local:///a.zip;0x10;20") && !gentl::parse_local_url("File:///x.xml") &&
          !gentl::parse_local_url("Local:x;1") && !gentl::parse_local_url("Local:x;1;0"));
    close_all(o);
    // Read-only inspection issued no port write.
    CHECK(!fixture.called("GCWritePort"));
}

void register_transport_semantics() {
    for (const std::string endian : {"big", "little"}) {
        Fixture fixture({{"GENTL_MOCK_PORT_ENDIAN", endian}});
        auto o = open_all();
        std::uint32_t read = 0;
        auto status = o.device->with_registers([&](reg::IDeviceRegisters& r) -> reg::Status {
            auto value = r.read_register(0x100); // fixture stores 8 big-endian at 0x100
            if (!value) return value.error();
            read = value.value();
            if (auto s = r.write_register(0x200, 0x11223344u); !s) return s;
            std::array<std::byte, 4> raw{};
            if (auto s = r.read_memory(0x200, raw); !s) return s;
            const bool be = endian == "big";
            if (raw[0] != std::byte{be ? std::uint8_t{0x11} : std::uint8_t{0x44}})
                return gige_mel::backend::Error{ErrorCode::io_error, "word byte order"};
            return {};
        });
        CHECK(status);
        CHECK(read == (endian == "big" ? 8u : 0x08000000u));
        // Shared validation still applies BEFORE any native call.
        const auto before = fixture.count("GCReadPort");
        auto misaligned = o.device->with_registers([](reg::IDeviceRegisters& r) -> reg::Status {
            auto value = r.read_register(0x102);
            if (!value) return value.error();
            return {};
        });
        CHECK(!misaligned && misaligned.error().write_completion == reg::WriteCompletion::not_attempted);
        CHECK(fixture.count("GCReadPort") == before);
        close_all(o);
    }
    for (const std::string endian : {"none", "both"}) {
        Fixture fixture({{"GENTL_MOCK_PORT_ENDIAN", endian}});
        auto o = open_all();
        const auto before = fixture.count("GCReadPort");
        auto refused = o.device->with_registers([](reg::IDeviceRegisters&) -> reg::Status { return {}; });
        CHECK(!refused && refused.error().diagnostic.code == ErrorCode::unsupported); // never assumed
        CHECK(fixture.count("GCReadPort") == before);
        close_all(o);
    }
    // A failure after native write entry is uncertain, and the fixture really
    // applied the bytes before failing: the consumer does not claim rejection.
    Fixture fixture({{"GENTL_MOCK_FAIL", "GCWritePort=-1011"}});
    auto o = open_all();
    auto uncertain = o.device->with_registers([](reg::IDeviceRegisters& r) { return r.write_register(0x300, 0xa5a5a5a5u); });
    CHECK(!uncertain && uncertain.error().write_completion == reg::WriteCompletion::uncertain);
    CHECK(uncertain.error().diagnostic.code == ErrorCode::timeout);
    CHECK(uncertain.error().diagnostic.message.find("GC_ERR_TIMEOUT") != std::string::npos);
    std::array<std::byte, 4> applied{};
    CHECK(o.device->read_port(0x300, applied) && applied[0] == std::byte{0xa5});
    CHECK(o.device->close());
    auto closed = o.device->with_registers([](reg::IDeviceRegisters&) -> reg::Status { return {}; });
    CHECK(!closed && closed.error().diagnostic.code == ErrorCode::stopped);
    CHECK(o.interface->close() && o.system->close() && o.producer->close());
}
std::shared_ptr<gentl::Stream> open_stream(Opened& o, gentl::Device::StreamOptions options = {}) {
    auto ids = o.device->stream_ids();
    CHECK(ids && ids.value().size() == 1);
    auto stream = o.device->open_stream(ids.value()[0], options);
    CHECK(stream);
    return stream.value();
}

void stream_lifecycle_and_leases() {
    Fixture fixture;
    auto o = open_all();
    auto stream = open_stream(o, {2, 1024, 64 * 1024});
    CHECK(!o.device->close()); // a stream is open
    std::vector<gentl::Frame> held;
    for (int restart = 0; restart < 3; ++restart) {
        CHECK(stream->start());
        for (int i = 0; i < 2; ++i) {
            auto frame = stream->wait(1000ms);
            CHECK(frame);
            const auto& f = frame.value().facts();
            CHECK(f.mono8() && f.width == 8u && f.height == 4u && f.incomplete == false);
            CHECK(frame.value().bytes().size() == 32 && f.capacity == 32);
            CHECK(f.timestamp_ticks && !f.timestamp_ns); // raw value only, domain unknown
            const auto id = *f.frame_id;
            CHECK(frame.value().bytes()[1] == std::byte(static_cast<std::uint8_t>((1 + id) & 0xffu)));
            held.push_back(std::move(frame).value());
        }
        // Both leases held: the producer keeps delivering, but held bytes are never reused.
        const std::vector<std::byte> snapshot(held.back().bytes().begin(), held.back().bytes().end());
        auto starved = stream->wait(1000ms);
        CHECK(!starved && starved.error().code == ErrorCode::resource_exhausted);
        CHECK(std::equal(snapshot.begin(), snapshot.end(), held.back().bytes().begin()));
        CHECK(stream->stop());
        // Leases survive stop; restart is refused until they are released.
        auto refused = stream->start();
        CHECK(!refused && refused.error().code == ErrorCode::resource_exhausted);
        held.clear();
    }
    const auto counters = stream->counters();
    CHECK(counters.delivered == 6 && counters.starvation == 3 && counters.incomplete == 0);
    // Teardown order: stop engine, flush, revoke every buffer, unregister event.
    const auto stop = fixture.index("DSStopAcquisition");
    CHECK(stop < fixture.index("DSFlushQueue", stop));
    CHECK(fixture.index("DSFlushQueue", stop) < fixture.index("DSRevokeBuffer", stop));
    CHECK(fixture.index("DSRevokeBuffer", stop) < fixture.index("GCUnregisterEvent", stop));
    CHECK(fixture.count("DSAnnounceBuffer") == 6 && fixture.count("DSRevokeBuffer") == 6);
    CHECK(stream->close() && stream->close()); // idempotent
    close_all(o);
}

void leases_outlive_producer_unload() {
    Fixture fixture;
    std::optional<gentl::Frame> kept;
    {
        auto o = open_all();
        auto stream = open_stream(o);
        CHECK(stream->start());
        auto frame = stream->wait(1000ms);
        CHECK(frame);
        kept.emplace(std::move(frame).value());
        CHECK(stream->close());
        close_all(o);
    }
    CHECK(!mapped(GENTL_MOCK) && fixture.called("unloaded"));
    // Consumer-owned copy: still readable after the library is gone.
    CHECK(kept->bytes().size() == 32);
    CHECK(kept->bytes()[0] == std::byte(static_cast<std::uint8_t>(*kept->facts().frame_id)));
}

void stop_wakes_blocked_wait() {
    Fixture fixture({{"GENTL_MOCK_STALL", "1"}});
    auto o = open_all();
    auto stream = open_stream(o);
    CHECK(stream->start());
    auto timed_out = stream->wait(20ms);
    CHECK(!timed_out && timed_out.error().code == ErrorCode::timeout && timed_out.error().native == -1011);
    std::atomic<bool> returned{false};
    gentl::Failure observed;
    std::thread receiver([&] {
        auto frame = stream->wait(std::chrono::hours(1));
        if (!frame) observed = frame.error();
        returned = true;
    });
    std::this_thread::sleep_for(50ms);
    CHECK(!returned);
    stream->request_stop();
    receiver.join(); // EventKill must release the blocked EventGetData
    // GC_ERR_ABORT from the killed EventGetData (or, if the stop won the race
    // before native entry, a consumer-side stop). Never a timeout or a frame.
    CHECK(returned && observed.code == ErrorCode::stopped && (observed.native == -1012 || observed.native == 0));
    CHECK(fixture.called("EventKill"));
    CHECK(stream->close());
    close_all(o);
}
void stream_start_failures_clean_up() {
    for (const std::string failing : {"DSAnnounceBuffer", "DSQueueBuffer", "GCRegisterEvent", "DSStartAcquisition"}) {
        Fixture fixture({{"GENTL_MOCK_FAIL", failing}});
        auto o = open_all();
        auto stream = open_stream(o);
        auto started = stream->start();
        CHECK(!started && started.error().function.rfind(failing, 0) == 0 && started.error().native == gc_io);
        // The strict fixture DSClose refuses announced buffers, events or a running engine.
        CHECK(stream->close());
        close_all(o);
    }
    {
        Fixture fixture({{"GENTL_MOCK_DEFINES_PAYLOAD", "0"}});
        auto o = open_all();
        auto stream = open_stream(o);
        auto refused = stream->start();
        CHECK(!refused && refused.error().code == ErrorCode::unsupported);
        CHECK(!fixture.called("DSAnnounceBuffer"));
        CHECK(stream->close());
        close_all(o);
    }
    Fixture fixture;
    auto o = open_all();
    auto small = open_stream(o, {8, 1024, 256}); // 8 x 32 x 2 bytes > 256: refused before allocation
    auto refused = small->start();
    CHECK(!refused && refused.error().code == ErrorCode::resource_exhausted);
    CHECK(!fixture.called("DSAnnounceBuffer"));
    CHECK(small->close());
    CHECK(!o.device->open_stream("mock-stream-0", {0, 1024, 1024})); // zero buffers refused
    close_all(o);
}

// A producer whose DevClose is unimplemented (as Aravis 0.9.2 returns
// GC_ERR_NOT_IMPLEMENTED) may still own the device: even though GCCloseLib then
// succeeds, the library must stay mapped rather than be unloaded under it.
// Uses the minimal fixture file: retention is per file and permanent.
void failed_destructor_close_taints_producer() {
    Fixture fixture({{"GENTL_MOCK_FAIL", "DevClose=-1003"}});
    const auto before = gentl::retained_producers();
    {
        auto o = open_all(GENTL_MOCK_MINIMAL_CTI);
        auto refused = o.device->close();
        CHECK(!refused && refused.error().code == ErrorCode::unsupported);
    } // ~Device: DevClose fails again (taint); the strict fixture then refuses
      // IFClose, TLClose and GCCloseLib because its device is still open.
    CHECK(fixture.count("DevClose") == 0 && fixture.count("GCCloseLib") == 0);
    CHECK(gentl::retained_producers() == before + 1);
    CHECK(mapped(GENTL_MOCK_MINIMAL_CTI) && !fixture.called("unloaded"));
}

void revoke_failure_quarantines_memory() {
    Fixture fixture({{"GENTL_MOCK_FAIL", "DSRevokeBuffer,DSClose,DevClose,GCCloseLib"}});
    {
        auto o = open_all();
        auto stream = open_stream(o);
        CHECK(stream->start());
        auto stopped = stream->stop();
        CHECK(!stopped && stopped.error().function == "DSRevokeBuffer");
        CHECK(o.producer->quarantined() == 4); // never freed while the producer may use it
        CHECK(!stream->close());
    }
    // Unknown producer state: the library and quarantined memory stay mapped.
    CHECK(gentl::retained_producers() == 2);
    CHECK(mapped(GENTL_MOCK) && !fixture.called("unloaded"));
    auto again = gentl::Producer::load(GENTL_MOCK);
    CHECK(!again && again.error().code == ErrorCode::busy);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"invalid_paths", invalid_paths},
        {"missing_mandatory_symbol", missing_mandatory_symbol},
        {"initialization_failure", initialization_failure},
        {"lifecycle_order_and_repetition", lifecycle_order_and_repetition},
        {"parent_close_refused_while_children_open", parent_close_refused_while_children_open},
        {"destruction_order_without_explicit_close", destruction_order_without_explicit_close},
        {"exact_selection", exact_selection},
        {"partial_open_failure_cleans_up", partial_open_failure_cleans_up},
        {"native_error_propagation", native_error_propagation},
        {"one_instance_per_producer_file", one_instance_per_producer_file},
        {"optional_entry_points_are_all_or_nothing", optional_entry_points_are_all_or_nothing},
        {"port_inspection_and_xml_location", port_inspection_and_xml_location},
        {"register_transport_semantics", register_transport_semantics},
        {"stream_lifecycle_and_leases", stream_lifecycle_and_leases},
        {"leases_outlive_producer_unload", leases_outlive_producer_unload},
        {"stop_wakes_blocked_wait", stop_wakes_blocked_wait},
        {"stream_start_failures_clean_up", stream_start_failures_clean_up},
        // Last: each deliberately leaves one fixture file retained (never unloaded).
        {"failed_destructor_close_taints_producer", failed_destructor_close_taints_producer},
        {"revoke_failure_quarantines_memory", revoke_failure_quarantines_memory},
    };
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << tests.size() << " GenTL consumer fixture contracts passed (fixture only; no real producer or camera)\n";
    return 0;
}
