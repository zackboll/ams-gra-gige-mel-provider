// Controlled GenTL 1.6 producer fixture for GIGE-MEL-GENTL-001 offline tests.
// Built only with GIGE_MEL_BUILD_GENTL_PROBE=ON, against the unmodified,
// checksum-verified EMVA GenTL.h. It models no real device and qualifies no
// producer or camera: it exists to exercise consumer ownership/error paths.
//
// Environment (read at GCInitLib):
//   GENTL_MOCK_LOG=path         append one line per significant call (read at load)
//   GENTL_MOCK_FAIL=A,B=-1005   fail named entry points (default GC_ERR_IO)
//   GENTL_MOCK_PORT_ENDIAN=big|little|none|both  declared remote port order
//   GENTL_MOCK_DEFINES_PAYLOAD=0|1               STREAM_INFO_DEFINES_PAYLOADSIZE
//   GENTL_MOCK_STALL=1          queued buffers are never filled (blocked waits)
// Compile-time variants: GENTL_MOCK_OMIT_TLOPEN (missing mandatory symbol) and
// GENTL_MOCK_MINIMAL (no GCWritePort and no acquisition entry points).
#include <GenTL.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace GenTL;

namespace {

constexpr std::size_t width = 8, height = 4, payload = width * height;
constexpr std::size_t port_bytes = 0x2000, xml_address = 0x1000;
constexpr const char* interface_id = "mock-if-0";
constexpr const char* device_ids[] = {"mock-dev-A", "mock-dev-B"};
constexpr const char* stream_id = "mock-stream-0";

// Minimal GenApi 1.1 description placed in the remote port ("Local:" URL).
constexpr char xml[] = R"(<?xml version="1.0" encoding="utf-8"?>
<RegisterDescription ModelName="MockCam" VendorName="GIGE-MEL" StandardNameSpace="None"
  SchemaMajorVersion="1" SchemaMinorVersion="1" SchemaSubMinorVersion="0" MajorVersion="1"
  MinorVersion="0" SubMinorVersion="0" ProductGuid="5f8e0c52-6a4a-4f53-9d22-6b1f00000001"
  VersionGuid="5f8e0c52-6a4a-4f53-9d22-6b1f00000002" xmlns="http://www.genicam.org/GenApi/Version_1_1">
  <Category Name="Root" NameSpace="Standard"><pFeature>Width</pFeature></Category>
  <IntReg Name="Width" NameSpace="Standard"><Address>0x0100</Address><Length>4</Length>
    <AccessMode>RO</AccessMode><pPort>Device</pPort><Sign>Unsigned</Sign><Endianess>BigEndian</Endianess></IntReg>
  <Port Name="Device" NameSpace="Standard"/>
</RegisterDescription>
)";

char log_path[4096];
thread_local GC_ERROR last_code = GC_ERR_SUCCESS;
thread_local char last_text[256];

void log(const std::string& line) {
    if (!log_path[0]) return;
    if (std::FILE* file = std::fopen(log_path, "a")) {
        std::fputs((line + "\n").c_str(), file);
        std::fclose(file);
    }
}

GC_ERROR error(GC_ERROR code, const std::string& text) {
    last_code = code;
    std::snprintf(last_text, sizeof last_text, "mock: %s", text.c_str());
    return code;
}

struct Buffer {
    void* base{nullptr};
    std::size_t size{0};
    void* user{nullptr};
    std::uint64_t frame{0};
    std::size_t filled{0};
};

struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool initialized{false}, discovered{false}, acquiring{false}, killed{false}, registered{false};
    bool tl{false}, iface{false}, devices[2]{}, stream{false};
    std::vector<unsigned char> port;
    std::vector<std::unique_ptr<Buffer>> buffers;
    std::deque<Buffer*> input, output;
    std::uint64_t next_frame{0}, delivered{0};
    std::string fail, endian{"big"};
    bool defines_payload{true}, stall{false};
} state;

// Distinct addresses used as opaque handles.
char tl_handle, iface_handle, device_handle[2], port_handle[2], stream_handle, event_handle;

// Installed only by offline tests, before starting control-thread operations.
using BoundaryHook = void (*)(const char*, bool, void*);
BoundaryHook boundary_hook = nullptr;
void* boundary_context = nullptr;
bool conclusive_close = false;
struct Boundary {
    const char* name;
    explicit Boundary(const char* n) : name(n) {
        if (boundary_hook) boundary_hook(name, false, boundary_context);
    }
    ~Boundary() { if (boundary_hook) boundary_hook(name, true, boundary_context); }
};

GC_ERROR injected(const char* name) {
    const std::string list = "," + state.fail + ",";
    const std::string key = std::string(",") + name;
    const auto at = list.find(key);
    if (at == std::string::npos) return GC_ERR_SUCCESS;
    const char next = list[at + key.size()];
    if (next == ',') return error(GC_ERR_IO, std::string("injected failure in ") + name);
    if (next != '=') return GC_ERR_SUCCESS;
    const auto code = static_cast<GC_ERROR>(std::strtol(list.c_str() + at + key.size() + 1, nullptr, 10));
    return error(code, std::string("injected failure in ") + name);
}
#define MOCK_INJECT(name) do { if (auto e_ = injected(name); e_ != GC_ERR_SUCCESS) { log(std::string("fail ") + name); return e_; } } while (false)
#define MOCK_INIT do { if (!state.initialized) return error(GC_ERR_NOT_INITIALIZED, "GCInitLib not called"); } while (false)

GC_ERROR put(INFO_DATATYPE kind, const void* value, std::size_t size, INFO_DATATYPE* type, void* buffer,
             std::size_t* out) {
    if (!out) return error(GC_ERR_INVALID_PARAMETER, "null size");
    if (type) *type = kind;
    if (!buffer) { *out = size; return GC_ERR_SUCCESS; }
    if (*out < size) return error(GC_ERR_BUFFER_TOO_SMALL, "buffer too small");
    std::memcpy(buffer, value, size);
    *out = size;
    return GC_ERR_SUCCESS;
}
GC_ERROR put_string(const char* text, INFO_DATATYPE* type, void* buffer, std::size_t* size) {
    return put(INFO_DATATYPE_STRING, text, std::strlen(text) + 1, type, buffer, size);
}
template <class T> GC_ERROR put_value(INFO_DATATYPE kind, T value, INFO_DATATYPE* type, void* buffer, std::size_t* size) {
    return put(kind, &value, sizeof value, type, buffer, size);
}
GC_ERROR put_bool(bool value, INFO_DATATYPE* type, void* buffer, std::size_t* size) {
    const unsigned char byte = value ? 1 : 0;
    return put(INFO_DATATYPE_BOOL8, &byte, 1, type, buffer, size);
}
int device_index(const void* handle) {
    for (int i = 0; i < 2; ++i) if (handle == &device_handle[i] && state.devices[i]) return i;
    return -1;
}
int port_index(const void* handle) {
    for (int i = 0; i < 2; ++i) if (handle == &port_handle[i] && state.devices[i]) return i;
    return -1;
}
Buffer* buffer_of(const void* handle) {
    for (auto& buffer : state.buffers) if (buffer.get() == handle) return buffer.get();
    return nullptr;
}
bool stream_ok(const void* handle) { return handle == &stream_handle && state.stream; }

void write_be(std::size_t address, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
        state.port[address + i] = static_cast<unsigned char>((value >> (8 * (3 - i))) & 0xffu);
}

void reset() {
    state.port.assign(port_bytes, 0);
    write_be(0x100, static_cast<std::uint32_t>(width));
    write_be(0x104, static_cast<std::uint32_t>(height));
    std::memcpy(state.port.data() + xml_address, xml, sizeof xml - 1);
    state.buffers.clear();
    state.input.clear();
    state.output.clear();
    state.discovered = state.acquiring = state.killed = state.registered = false;
    state.tl = state.iface = state.stream = state.devices[0] = state.devices[1] = false;
    state.next_frame = state.delivered = 0;
    const char* fail = std::getenv("GENTL_MOCK_FAIL");
    state.fail = fail ? fail : "";
    const char* endian = std::getenv("GENTL_MOCK_PORT_ENDIAN");
    state.endian = endian ? endian : "big";
    const char* defines = std::getenv("GENTL_MOCK_DEFINES_PAYLOAD");
    state.defines_payload = !defines || std::strcmp(defines, "0") != 0;
    const char* stall = std::getenv("GENTL_MOCK_STALL");
    state.stall = stall && std::strcmp(stall, "1") == 0;
}

// Producer-side "acquisition": every queued buffer is filled immediately with
// a deterministic Mono8 pattern and moved to the output queue.
void fill_locked() {
    while (state.acquiring && !state.stall && !state.input.empty()) {
        auto* buffer = state.input.front();
        state.input.pop_front();
        buffer->frame = ++state.next_frame;
        buffer->filled = payload;
        auto* bytes = static_cast<unsigned char*>(buffer->base);
        for (std::size_t i = 0; i < payload; ++i) bytes[i] = static_cast<unsigned char>((i + buffer->frame) & 0xffu);
        state.output.push_back(buffer);
    }
    state.cv.notify_all();
}

__attribute__((constructor)) void loaded() {
    if (const char* path = std::getenv("GENTL_MOCK_LOG")) std::snprintf(log_path, sizeof log_path, "%s", path);
    log("loaded");
}
__attribute__((destructor)) void unloaded() { log("unloaded"); }

} // namespace

extern "C" {

void MockSetBoundaryHook(BoundaryHook hook, void* context) {
    boundary_hook = hook;
    boundary_context = context;
}
void MockSetFailure(const char* fail) {
    std::lock_guard lock(state.mutex);
    state.fail = fail;
}
void MockConclusiveClose() { conclusive_close = true; }
void MockRepairOpenHandles() {
    std::lock_guard lock(state.mutex);
    if (state.stream) state.stream = false;
    else if (state.devices[0]) state.devices[0] = false;
    else if (state.iface) state.iface = false;
    else state.tl = false;
}
// Simulate a late native write through both announcement pointers. Sanitizers
// must see valid storage even after all consumer objects have been destroyed.
void MockTouchOutstandingBuffers() {
    std::lock_guard lock(state.mutex);
    for (const auto& buffer : state.buffers) {
        static_cast<unsigned char*>(buffer->base)[0] = 0x5a;
        volatile auto user_byte = *static_cast<const unsigned char*>(buffer->user);
        (void)user_byte;
    }
}

GC_API GCInitLib() {
    std::lock_guard lock(state.mutex);
    if (state.initialized) return error(GC_ERR_RESOURCE_IN_USE, "already initialized");
    reset();
    MOCK_INJECT("GCInitLib");
    state.initialized = true;
    log("GCInitLib");
    return GC_ERR_SUCCESS;
}

GC_API GCCloseLib() {
    std::lock_guard lock(state.mutex);
    MOCK_INIT;
    MOCK_INJECT("GCCloseLib");
    // The producer itself requires every module to be closed first.
    if (state.tl || state.iface || state.devices[0] || state.devices[1] || state.stream || !state.buffers.empty())
        return error(GC_ERR_RESOURCE_IN_USE, "GCCloseLib with open modules or announced buffers");
    state.initialized = false;
    log("GCCloseLib");
    return GC_ERR_SUCCESS;
}

GC_API GCGetLastError(GC_ERROR* code, char* text, size_t* size) {
    if (!size) return GC_ERR_INVALID_PARAMETER;
    if (code) *code = last_code;
    const auto needed = std::strlen(last_text) + 1;
    if (!text) { *size = needed; return GC_ERR_SUCCESS; }
    if (*size < needed) return GC_ERR_BUFFER_TOO_SMALL;
    std::memcpy(text, last_text, needed);
    *size = needed;
    return GC_ERR_SUCCESS;
}

GC_API GCGetInfo(TL_INFO_CMD command, INFO_DATATYPE* type, void* buffer, size_t* size) {
    std::lock_guard lock(state.mutex);
    MOCK_INIT;
    switch (command) {
    case TL_INFO_ID: return put_string("gige-mel-gentl-mock", type, buffer, size);
    case TL_INFO_VENDOR: return put_string("GIGE-MEL test fixture", type, buffer, size);
    case TL_INFO_MODEL: return put_string("mock producer", type, buffer, size);
    case TL_INFO_VERSION: return put_string("1", type, buffer, size);
    case TL_INFO_TLTYPE: return put_string(TLTypeCustomName, type, buffer, size);
    case TL_INFO_GENTL_VER_MAJOR: return put_value<std::uint32_t>(INFO_DATATYPE_UINT32, 1, type, buffer, size);
    case TL_INFO_GENTL_VER_MINOR: return put_value<std::uint32_t>(INFO_DATATYPE_UINT32, 6, type, buffer, size);
    default: return error(GC_ERR_NOT_IMPLEMENTED, "GCGetInfo command not modeled");
    }
}

#ifndef GENTL_MOCK_OMIT_TLOPEN
GC_API TLOpen(TL_HANDLE* handle) {
    std::lock_guard lock(state.mutex);
    MOCK_INIT;
    MOCK_INJECT("TLOpen");
    if (!handle) return error(GC_ERR_INVALID_PARAMETER, "null handle");
    if (state.tl) return error(GC_ERR_RESOURCE_IN_USE, "TL already open");
    state.tl = true;
    *handle = &tl_handle;
    log("TLOpen");
    return GC_ERR_SUCCESS;
}
#endif

GC_API TLClose(TL_HANDLE handle) {
    log("attempt TLClose");
    std::lock_guard lock(state.mutex);
    MOCK_INIT;
    if (handle != &tl_handle || !state.tl) return error(GC_ERR_INVALID_HANDLE, "TLClose handle");
    MOCK_INJECT("TLClose");
    if (state.iface) return error(GC_ERR_RESOURCE_IN_USE, "TLClose with an open interface");
    state.tl = false;
    log("TLClose");
    return GC_ERR_SUCCESS;
}
GC_API TLGetInfo(TL_HANDLE handle, TL_INFO_CMD command, INFO_DATATYPE* type, void* buffer, size_t* size) {
    if (handle != &tl_handle || !state.tl) return error(GC_ERR_INVALID_HANDLE, "TLGetInfo handle");
    return GCGetInfo(command, type, buffer, size);
}

GC_API TLUpdateInterfaceList(TL_HANDLE handle, bool8_t* changed, uint64_t) {
    std::lock_guard lock(state.mutex);
    if (handle != &tl_handle || !state.tl) return error(GC_ERR_INVALID_HANDLE, "TLUpdateInterfaceList handle");
    if (changed) *changed = false;
    return GC_ERR_SUCCESS;
}

GC_API TLGetNumInterfaces(TL_HANDLE handle, uint32_t* count) {
    std::lock_guard lock(state.mutex);
    if (handle != &tl_handle || !state.tl) return error(GC_ERR_INVALID_HANDLE, "TLGetNumInterfaces handle");
    MOCK_INJECT("TLGetNumInterfaces");
    *count = 1;
    return GC_ERR_SUCCESS;
}

GC_API TLGetInterfaceID(TL_HANDLE handle, uint32_t index, char* id, size_t* size) {
    std::lock_guard lock(state.mutex);
    if (handle != &tl_handle || !state.tl) return error(GC_ERR_INVALID_HANDLE, "TLGetInterfaceID handle");
    if (index != 0) return error(GC_ERR_INVALID_INDEX, "interface index");
    return put_string(interface_id, nullptr, id, size);
}

GC_API TLGetInterfaceInfo(TL_HANDLE handle, const char* id, INTERFACE_INFO_CMD command, INFO_DATATYPE* type,
                          void* buffer, size_t* size) {
    std::lock_guard lock(state.mutex);
    if (handle != &tl_handle || !state.tl) return error(GC_ERR_INVALID_HANDLE, "TLGetInterfaceInfo handle");
    if (!id || std::strcmp(id, interface_id) != 0) return error(GC_ERR_INVALID_ID, "interface ID");
    if (command == INTERFACE_INFO_DISPLAYNAME) return put_string("Mock interface", type, buffer, size);
    if (command == INTERFACE_INFO_TLTYPE) return put_string(TLTypeCustomName, type, buffer, size);
    return error(GC_ERR_NOT_IMPLEMENTED, "interface info not modeled");
}

GC_API TLOpenInterface(TL_HANDLE handle, const char* id, IF_HANDLE* out) {
    std::lock_guard lock(state.mutex);
    if (handle != &tl_handle || !state.tl) return error(GC_ERR_INVALID_HANDLE, "TLOpenInterface handle");
    MOCK_INJECT("TLOpenInterface");
    if (!id || std::strcmp(id, interface_id) != 0) return error(GC_ERR_INVALID_ID, "interface ID");
    if (state.iface) return error(GC_ERR_RESOURCE_IN_USE, "interface already open");
    state.iface = true;
    *out = &iface_handle;
    log("TLOpenInterface");
    return GC_ERR_SUCCESS;
}

GC_API IFClose(IF_HANDLE handle) {
    log("attempt IFClose");
    std::lock_guard lock(state.mutex);
    if (handle != &iface_handle || !state.iface) return error(GC_ERR_INVALID_HANDLE, "IFClose handle");
    MOCK_INJECT("IFClose");
    if (state.devices[0] || state.devices[1]) return error(GC_ERR_RESOURCE_IN_USE, "IFClose with an open device");
    state.iface = false;
    log("IFClose");
    return GC_ERR_SUCCESS;
}

GC_API IFUpdateDeviceList(IF_HANDLE handle, bool8_t* changed, uint64_t) {
    std::lock_guard lock(state.mutex);
    if (handle != &iface_handle || !state.iface) return error(GC_ERR_INVALID_HANDLE, "IFUpdateDeviceList handle");
    MOCK_INJECT("IFUpdateDeviceList");
    if (changed) *changed = !state.discovered;
    state.discovered = true;
    log("IFUpdateDeviceList");
    return GC_ERR_SUCCESS;
}

GC_API IFGetNumDevices(IF_HANDLE handle, uint32_t* count) {
    std::lock_guard lock(state.mutex);
    if (handle != &iface_handle || !state.iface) return error(GC_ERR_INVALID_HANDLE, "IFGetNumDevices handle");
    *count = state.discovered ? 2 : 0;
    return GC_ERR_SUCCESS;
}

GC_API IFGetDeviceID(IF_HANDLE handle, uint32_t index, char* id, size_t* size) {
    std::lock_guard lock(state.mutex);
    if (handle != &iface_handle || !state.iface) return error(GC_ERR_INVALID_HANDLE, "IFGetDeviceID handle");
    if (!state.discovered || index > 1) return error(GC_ERR_INVALID_INDEX, "device index");
    return put_string(device_ids[index], nullptr, id, size);
}
GC_API IFGetDeviceInfo(IF_HANDLE handle, const char* id, DEVICE_INFO_CMD command, INFO_DATATYPE* type, void* buffer,
                       size_t* size) {
    std::lock_guard lock(state.mutex);
    if (handle != &iface_handle || !state.iface) return error(GC_ERR_INVALID_HANDLE, "IFGetDeviceInfo handle");
    int index = -1;
    for (int i = 0; i < 2; ++i) if (id && std::strcmp(id, device_ids[i]) == 0) index = i;
    if (index < 0) return error(GC_ERR_INVALID_ID, "device ID");
    switch (command) {
    case DEVICE_INFO_ID: return put_string(device_ids[index], type, buffer, size);
    case DEVICE_INFO_VENDOR: return put_string("GIGE-MEL", type, buffer, size);
    case DEVICE_INFO_MODEL: return put_string("MockCam", type, buffer, size);
    case DEVICE_INFO_SERIAL_NUMBER: return put_string(index ? "SN-B" : "SN-A", type, buffer, size);
    case DEVICE_INFO_TLTYPE: return put_string(TLTypeCustomName, type, buffer, size);
    case DEVICE_INFO_ACCESS_STATUS:
        return put_value<int32_t>(INFO_DATATYPE_INT32, state.devices[index] ? DEVICE_ACCESS_STATUS_OPEN_READWRITE
                                                                            : DEVICE_ACCESS_STATUS_READWRITE,
                                  type, buffer, size);
    default: return error(GC_ERR_NOT_IMPLEMENTED, "device info not modeled");
    }
}

GC_API IFOpenDevice(IF_HANDLE handle, const char* id, DEVICE_ACCESS_FLAGS flags, DEV_HANDLE* out) {
    std::lock_guard lock(state.mutex);
    if (handle != &iface_handle || !state.iface) return error(GC_ERR_INVALID_HANDLE, "IFOpenDevice handle");
    MOCK_INJECT("IFOpenDevice");
    int index = -1;
    for (int i = 0; i < 2; ++i) if (id && std::strcmp(id, device_ids[i]) == 0) index = i;
    if (index < 0) return error(GC_ERR_INVALID_ID, "device ID");
    if (state.devices[index]) return error(GC_ERR_RESOURCE_IN_USE, "device already open");
    state.devices[index] = true;
    *out = &device_handle[index];
    log(std::string("IFOpenDevice ") + device_ids[index] + " access=" + std::to_string(flags));
    return GC_ERR_SUCCESS;
}

GC_API DevGetPort(DEV_HANDLE handle, PORT_HANDLE* port) {
    std::lock_guard lock(state.mutex);
    const int index = device_index(handle);
    if (index < 0) return error(GC_ERR_INVALID_HANDLE, "DevGetPort handle");
    MOCK_INJECT("DevGetPort");
    *port = &port_handle[index];
    return GC_ERR_SUCCESS;
}

GC_API DevGetInfo(DEV_HANDLE handle, DEVICE_INFO_CMD command, INFO_DATATYPE* type, void* buffer, size_t* size) {
    std::lock_guard lock(state.mutex);
    const int index = device_index(handle);
    if (index < 0) return error(GC_ERR_INVALID_HANDLE, "DevGetInfo handle");
    if (command == DEVICE_INFO_ID) return put_string(device_ids[index], type, buffer, size);
    if (command == DEVICE_INFO_TIMESTAMP_FREQUENCY)
        return put_value<uint64_t>(INFO_DATATYPE_UINT64, 1000000000ull, type, buffer, size);
    return error(GC_ERR_NOT_IMPLEMENTED, "device module info not modeled");
}

GC_API DevClose(DEV_HANDLE handle) {
    log("attempt DevClose");
    std::lock_guard lock(state.mutex);
    const int index = device_index(handle);
    if (index < 0) return error(GC_ERR_INVALID_HANDLE, "DevClose handle");
    MOCK_INJECT("DevClose");
    if (state.stream) return error(GC_ERR_RESOURCE_IN_USE, "DevClose with an open stream");
    state.devices[index] = false;
    log(std::string("DevClose ") + device_ids[index]);
    return GC_ERR_SUCCESS;
}
GC_API GCReadPort(PORT_HANDLE port, uint64_t address, void* buffer, size_t* size) {
    std::lock_guard lock(state.mutex);
    MOCK_INIT;
    if (port_index(port) < 0) return error(GC_ERR_INVALID_HANDLE, "GCReadPort handle");
    if (!buffer || !size) return error(GC_ERR_INVALID_PARAMETER, "GCReadPort arguments");
    MOCK_INJECT("GCReadPort");
    if (address > port_bytes || *size > port_bytes - address) return error(GC_ERR_INVALID_ADDRESS, "outside mock port");
    std::memcpy(buffer, state.port.data() + address, *size);
    log("GCReadPort " + std::to_string(address) + " " + std::to_string(*size));
    return GC_ERR_SUCCESS;
}

#ifndef GENTL_MOCK_MINIMAL
GC_API GCWritePort(PORT_HANDLE port, uint64_t address, const void* buffer, size_t* size) {
    std::lock_guard lock(state.mutex);
    MOCK_INIT;
    if (port_index(port) < 0) return error(GC_ERR_INVALID_HANDLE, "GCWritePort handle");
    if (!buffer || !size) return error(GC_ERR_INVALID_PARAMETER, "GCWritePort arguments");
    if (address > port_bytes || *size > port_bytes - address) return error(GC_ERR_INVALID_ADDRESS, "outside mock port");
    // Applied BEFORE an injected failure: models an uncertain write outcome.
    std::memcpy(state.port.data() + address, buffer, *size);
    log("GCWritePort " + std::to_string(address) + " " + std::to_string(*size));
    MOCK_INJECT("GCWritePort");
    return GC_ERR_SUCCESS;
}
#endif

GC_API GCGetPortInfo(PORT_HANDLE port, PORT_INFO_CMD command, INFO_DATATYPE* type, void* buffer, size_t* size) {
    std::lock_guard lock(state.mutex);
    MOCK_INIT;
    const int index = port_index(port);
    if (index < 0) return error(GC_ERR_INVALID_HANDLE, "GCGetPortInfo handle");
    const bool both = state.endian == "both";
    switch (command) {
    case PORT_INFO_ID: return put_string(device_ids[index], type, buffer, size);
    case PORT_INFO_VENDOR: return put_string("GIGE-MEL", type, buffer, size);
    case PORT_INFO_MODEL: return put_string("MockCam", type, buffer, size);
    case PORT_INFO_TLTYPE: return put_string(TLTypeCustomName, type, buffer, size);
    case PORT_INFO_MODULE: return put_string(TLRemoteDeviceModuleName, type, buffer, size);
    case PORT_INFO_PORTNAME: return put_string("Device", type, buffer, size);
    case PORT_INFO_LITTLE_ENDIAN:
        if (state.endian == "none") return error(GC_ERR_NOT_IMPLEMENTED, "endianness not declared");
        return put_bool(both || state.endian == "little", type, buffer, size);
    case PORT_INFO_BIG_ENDIAN:
        if (state.endian == "none") return error(GC_ERR_NOT_IMPLEMENTED, "endianness not declared");
        return put_bool(both || state.endian == "big", type, buffer, size);
    case PORT_INFO_ACCESS_READ: return put_bool(true, type, buffer, size);
    case PORT_INFO_ACCESS_WRITE: return put_bool(true, type, buffer, size);
    case PORT_INFO_ACCESS_NA: return put_bool(false, type, buffer, size);
    case PORT_INFO_ACCESS_NI: return put_bool(false, type, buffer, size);
    default: return error(GC_ERR_NOT_IMPLEMENTED, "port info not modeled");
    }
}

GC_API GCGetPortURL(PORT_HANDLE, char*, size_t*) { return error(GC_ERR_NOT_IMPLEMENTED, "deprecated GCGetPortURL"); }

GC_API GCGetNumPortURLs(PORT_HANDLE port, uint32_t* count) {
    std::lock_guard lock(state.mutex);
    if (port_index(port) < 0) return error(GC_ERR_INVALID_HANDLE, "GCGetNumPortURLs handle");
    *count = 1;
    return GC_ERR_SUCCESS;
}

GC_API GCGetPortURLInfo(PORT_HANDLE port, uint32_t index, URL_INFO_CMD command, INFO_DATATYPE* type, void* buffer,
                        size_t* size) {
    std::lock_guard lock(state.mutex);
    if (port_index(port) < 0) return error(GC_ERR_INVALID_HANDLE, "GCGetPortURLInfo handle");
    if (index != 0) return error(GC_ERR_INVALID_INDEX, "URL index");
    char url[128];
    std::snprintf(url, sizeof url, "Local:mock.xml;%zx;%zx", xml_address, sizeof xml - 1);
    switch (command) {
    case URL_INFO_URL: return put_string(url, type, buffer, size);
    case URL_INFO_SCHEME: return put_value<int32_t>(INFO_DATATYPE_INT32, URL_SCHEME_LOCAL, type, buffer, size);
    case URL_INFO_FILE_REGISTER_ADDRESS:
        return put_value<uint64_t>(INFO_DATATYPE_UINT64, xml_address, type, buffer, size);
    case URL_INFO_FILE_SIZE: return put_value<uint64_t>(INFO_DATATYPE_UINT64, sizeof xml - 1, type, buffer, size);
    case URL_INFO_SCHEMA_VER_MAJOR: return put_value<int32_t>(INFO_DATATYPE_INT32, 1, type, buffer, size);
    case URL_INFO_SCHEMA_VER_MINOR: return put_value<int32_t>(INFO_DATATYPE_INT32, 1, type, buffer, size);
    default: return error(GC_ERR_NOT_IMPLEMENTED, "URL info not modeled");
    }
}

GC_API GCReadPortStacked(PORT_HANDLE, PORT_REGISTER_STACK_ENTRY*, size_t*) {
    return error(GC_ERR_NOT_IMPLEMENTED, "stacked read not modeled");
}
GC_API GCWritePortStacked(PORT_HANDLE, PORT_REGISTER_STACK_ENTRY*, size_t*) {
    return error(GC_ERR_NOT_IMPLEMENTED, "stacked write not modeled");
}
#ifndef GENTL_MOCK_MINIMAL
GC_API DevGetNumDataStreams(DEV_HANDLE handle, uint32_t* count) {
    std::lock_guard lock(state.mutex);
    if (device_index(handle) < 0) return error(GC_ERR_INVALID_HANDLE, "DevGetNumDataStreams handle");
    *count = 1;
    return GC_ERR_SUCCESS;
}

GC_API DevGetDataStreamID(DEV_HANDLE handle, uint32_t index, char* id, size_t* size) {
    std::lock_guard lock(state.mutex);
    if (device_index(handle) < 0) return error(GC_ERR_INVALID_HANDLE, "DevGetDataStreamID handle");
    if (index != 0) return error(GC_ERR_INVALID_INDEX, "stream index");
    return put_string(stream_id, nullptr, id, size);
}

GC_API DevOpenDataStream(DEV_HANDLE handle, const char* id, DS_HANDLE* out) {
    std::lock_guard lock(state.mutex);
    if (device_index(handle) < 0) return error(GC_ERR_INVALID_HANDLE, "DevOpenDataStream handle");
    MOCK_INJECT("DevOpenDataStream");
    if (!id || std::strcmp(id, stream_id) != 0) return error(GC_ERR_INVALID_ID, "stream ID");
    if (state.stream) return error(GC_ERR_RESOURCE_IN_USE, "stream already open");
    state.stream = true;
    *out = &stream_handle;
    log("DevOpenDataStream");
    return GC_ERR_SUCCESS;
}

GC_API DSGetInfo(DS_HANDLE handle, STREAM_INFO_CMD command, INFO_DATATYPE* type, void* buffer, size_t* size) {
    std::lock_guard lock(state.mutex);
    if (!stream_ok(handle)) return error(GC_ERR_INVALID_HANDLE, "DSGetInfo handle");
    switch (command) {
    case STREAM_INFO_ID: return put_string(stream_id, type, buffer, size);
    case STREAM_INFO_PAYLOAD_SIZE: return put_value<size_t>(INFO_DATATYPE_SIZET, payload, type, buffer, size);
    case STREAM_INFO_DEFINES_PAYLOADSIZE: return put_bool(state.defines_payload, type, buffer, size);
    case STREAM_INFO_NUM_DELIVERED: return put_value<uint64_t>(INFO_DATATYPE_UINT64, state.delivered, type, buffer, size);
    case STREAM_INFO_IS_GRABBING: return put_bool(state.acquiring, type, buffer, size);
    case STREAM_INFO_BUF_ANNOUNCE_MIN: return put_value<size_t>(INFO_DATATYPE_SIZET, 1, type, buffer, size);
    default: return error(GC_ERR_NOT_IMPLEMENTED, "stream info not modeled");
    }
}

GC_API DSAnnounceBuffer(DS_HANDLE handle, void* base, size_t size, void* user, BUFFER_HANDLE* out) {
    Boundary boundary("DSAnnounceBuffer");
    std::lock_guard lock(state.mutex);
    if (!stream_ok(handle)) return error(GC_ERR_INVALID_HANDLE, "DSAnnounceBuffer handle");
    MOCK_INJECT("DSAnnounceBuffer");
    if (!base || size < payload || !out) return error(GC_ERR_INVALID_PARAMETER, "announce arguments");
    auto buffer = std::make_unique<Buffer>();
    buffer->base = base;
    buffer->size = size;
    buffer->user = user;
    *out = buffer.get();
    state.buffers.push_back(std::move(buffer));
    log("DSAnnounceBuffer");
    return GC_ERR_SUCCESS;
}

GC_API DSAllocAndAnnounceBuffer(DS_HANDLE, size_t, void*, BUFFER_HANDLE*) {
    return error(GC_ERR_NOT_IMPLEMENTED, "producer-allocated buffers not modeled");
}

GC_API DSQueueBuffer(DS_HANDLE handle, BUFFER_HANDLE buffer_handle) {
    Boundary boundary("DSQueueBuffer");
    std::lock_guard lock(state.mutex);
    if (!stream_ok(handle)) return error(GC_ERR_INVALID_HANDLE, "DSQueueBuffer handle");
    auto* buffer = buffer_of(buffer_handle);
    if (!buffer) return error(GC_ERR_INVALID_HANDLE, "unknown buffer");
    MOCK_INJECT("DSQueueBuffer");
    for (auto* queued : state.input) if (queued == buffer) return error(GC_ERR_RESOURCE_IN_USE, "already queued");
    for (auto* queued : state.output) if (queued == buffer) return error(GC_ERR_RESOURCE_IN_USE, "awaiting delivery");
    buffer->filled = 0;
    state.input.push_back(buffer);
    fill_locked();
    return GC_ERR_SUCCESS;
}

GC_API DSRevokeBuffer(DS_HANDLE handle, BUFFER_HANDLE buffer_handle, void** base, void** user) {
    log("attempt DSRevokeBuffer");
    std::lock_guard lock(state.mutex);
    if (!stream_ok(handle)) return error(GC_ERR_INVALID_HANDLE, "DSRevokeBuffer handle");
    auto* buffer = buffer_of(buffer_handle);
    if (!buffer) return error(GC_ERR_INVALID_HANDLE, "unknown buffer");
    MOCK_INJECT("DSRevokeBuffer");
    for (auto* queued : state.input) if (queued == buffer) return error(GC_ERR_BUSY, "revoking a queued buffer");
    for (auto* queued : state.output) if (queued == buffer) return error(GC_ERR_BUSY, "revoking an undelivered buffer");
    if (base) *base = buffer->base;
    if (user) *user = buffer->user;
    for (auto it = state.buffers.begin(); it != state.buffers.end(); ++it)
        if (it->get() == buffer) { state.buffers.erase(it); break; }
    log("DSRevokeBuffer");
    return GC_ERR_SUCCESS;
}

GC_API DSFlushQueue(DS_HANDLE handle, ACQ_QUEUE_TYPE operation) {
    log("attempt DSFlushQueue");
    std::lock_guard lock(state.mutex);
    if (!stream_ok(handle)) return error(GC_ERR_INVALID_HANDLE, "DSFlushQueue handle");
    MOCK_INJECT("DSFlushQueue");
    if (operation != ACQ_QUEUE_ALL_DISCARD) return error(GC_ERR_NOT_IMPLEMENTED, "flush mode not modeled");
    state.input.clear();
    state.output.clear();
    log("DSFlushQueue");
    return GC_ERR_SUCCESS;
}

GC_API DSStartAcquisition(DS_HANDLE handle, ACQ_START_FLAGS, uint64_t) {
    Boundary boundary("DSStartAcquisition");
    std::lock_guard lock(state.mutex);
    if (!stream_ok(handle)) return error(GC_ERR_INVALID_HANDLE, "DSStartAcquisition handle");
    MOCK_INJECT("DSStartAcquisition");
    if (state.acquiring) return error(GC_ERR_RESOURCE_IN_USE, "already acquiring");
    state.acquiring = true;
    log("DSStartAcquisition");
    fill_locked();
    MOCK_INJECT("DSStartAcquisitionAfterStart");
    return GC_ERR_SUCCESS;
}

GC_API DSStopAcquisition(DS_HANDLE handle, ACQ_STOP_FLAGS) {
    log("attempt DSStopAcquisition");
    std::lock_guard lock(state.mutex);
    if (!stream_ok(handle)) return error(GC_ERR_INVALID_HANDLE, "DSStopAcquisition handle");
    MOCK_INJECT("DSStopAcquisition");
    if (!state.acquiring) return error(GC_ERR_RESOURCE_IN_USE, "not acquiring");
    state.acquiring = false;
    log("DSStopAcquisition");
    return GC_ERR_SUCCESS;
}
GC_API DSGetBufferInfo(DS_HANDLE handle, BUFFER_HANDLE buffer_handle, BUFFER_INFO_CMD command, INFO_DATATYPE* type,
                       void* out, size_t* size) {
    std::lock_guard lock(state.mutex);
    if (!stream_ok(handle)) return error(GC_ERR_INVALID_HANDLE, "DSGetBufferInfo handle");
    auto* buffer = buffer_of(buffer_handle);
    if (!buffer) return error(GC_ERR_INVALID_HANDLE, "unknown buffer");
    switch (command) {
    case BUFFER_INFO_BASE: return put_value<void*>(INFO_DATATYPE_PTR, buffer->base, type, out, size);
    case BUFFER_INFO_SIZE: return put_value<size_t>(INFO_DATATYPE_SIZET, buffer->size, type, out, size);
    case BUFFER_INFO_IS_INCOMPLETE: return put_bool(false, type, out, size);
    case BUFFER_INFO_FRAMEID: return put_value<uint64_t>(INFO_DATATYPE_UINT64, buffer->frame, type, out, size);
    case BUFFER_INFO_SIZE_FILLED: return put_value<size_t>(INFO_DATATYPE_SIZET, buffer->filled, type, out, size);
    case BUFFER_INFO_WIDTH: return put_value<size_t>(INFO_DATATYPE_SIZET, width, type, out, size);
    case BUFFER_INFO_HEIGHT: return put_value<size_t>(INFO_DATATYPE_SIZET, height, type, out, size);
    case BUFFER_INFO_PAYLOADTYPE: return put_value<size_t>(INFO_DATATYPE_SIZET, PAYLOAD_TYPE_IMAGE, type, out, size);
    case BUFFER_INFO_PIXELFORMAT: return put_value<uint64_t>(INFO_DATATYPE_UINT64, 0x01080001u, type, out, size);
    case BUFFER_INFO_PIXELFORMAT_NAMESPACE:
        return put_value<uint64_t>(INFO_DATATYPE_UINT64, PIXELFORMAT_NAMESPACE_PFNC_32BIT, type, out, size);
    case BUFFER_INFO_TIMESTAMP: // fixture counter in an undefined clock domain
        return put_value<uint64_t>(INFO_DATATYPE_UINT64, buffer->frame * 1000u, type, out, size);
    default: return error(GC_ERR_NOT_IMPLEMENTED, "buffer info not modeled");
    }
}

GC_API DSClose(DS_HANDLE handle) {
    log("attempt DSClose");
    std::lock_guard lock(state.mutex);
    if (!stream_ok(handle)) return error(GC_ERR_INVALID_HANDLE, "DSClose handle");
    MOCK_INJECT("DSClose");
    if (conclusive_close) {
        state.input.clear();
        state.output.clear();
        state.buffers.clear();
        state.registered = false;
        state.acquiring = false;
    }
    // Strict fixture: the consumer must have revoked and unregistered first.
    if (!state.buffers.empty() || state.registered || state.acquiring)
        return error(GC_ERR_RESOURCE_IN_USE, "DSClose with announced buffers, an event or a running engine");
    state.stream = false;
    log("DSClose");
    return GC_ERR_SUCCESS;
}

GC_API GCRegisterEvent(EVENTSRC_HANDLE source, EVENT_TYPE type, EVENT_HANDLE* event) {
    Boundary boundary("GCRegisterEvent");
    std::lock_guard lock(state.mutex);
    if (!stream_ok(source) || type != EVENT_NEW_BUFFER) return error(GC_ERR_NOT_IMPLEMENTED, "event not modeled");
    MOCK_INJECT("GCRegisterEvent");
    if (state.registered) return error(GC_ERR_RESOURCE_IN_USE, "event already registered");
    state.registered = true;
    state.killed = false;
    *event = &event_handle;
    log("GCRegisterEvent");
    return GC_ERR_SUCCESS;
}

GC_API GCUnregisterEvent(EVENTSRC_HANDLE source, EVENT_TYPE type) {
    log("attempt GCUnregisterEvent");
    std::lock_guard lock(state.mutex);
    if (!stream_ok(source) || type != EVENT_NEW_BUFFER || !state.registered)
        return error(GC_ERR_INVALID_HANDLE, "event not registered");
    MOCK_INJECT("GCUnregisterEvent");
    state.registered = false;
    state.cv.notify_all();
    log("GCUnregisterEvent");
    return GC_ERR_SUCCESS;
}

GC_API EventGetData(EVENT_HANDLE event, void* buffer, size_t* size, uint64_t timeout) {
    Boundary boundary("EventGetData");
    std::unique_lock lock(state.mutex);
    if (event != &event_handle || !state.registered) return error(GC_ERR_INVALID_HANDLE, "event handle");
    if (!buffer || !size || *size < sizeof(EVENT_NEW_BUFFER_DATA)) return error(GC_ERR_BUFFER_TOO_SMALL, "event buffer");
    const auto ready = [] { return state.killed || !state.output.empty() || !state.registered; };
    // Notify under the fixture mutex: EventKill cannot acquire it until the
    // native wait atomically releases it. No scheduling delay is assumed.
    if (boundary_hook) boundary_hook("EventGetDataWaiting", false, boundary_context);
    if (timeout == GENTL_INFINITE) state.cv.wait(lock, ready);
    else if (!state.cv.wait_for(lock, std::chrono::milliseconds(timeout), ready))
        return error(GC_ERR_TIMEOUT, "no buffer within timeout");
    if (state.killed) { state.killed = false; return error(GC_ERR_ABORT, "EventKill"); }
    if (!state.registered) return error(GC_ERR_ABORT, "event unregistered");
    auto* delivered = state.output.front();
    state.output.pop_front();
    ++state.delivered;
    EVENT_NEW_BUFFER_DATA data{delivered, delivered->user};
    std::memcpy(buffer, &data, sizeof data);
    *size = sizeof data;
    return GC_ERR_SUCCESS;
}

GC_API EventKill(EVENT_HANDLE event) {
    std::lock_guard lock(state.mutex);
    if (event != &event_handle || !state.registered) return error(GC_ERR_INVALID_HANDLE, "event handle");
    state.killed = true; // latched: terminates the current or next wait
    state.cv.notify_all();
    log("EventKill");
    return GC_ERR_SUCCESS;
}
// Remaining GenTL 1.4/1.5 entry points: exported (some consumers, such as the
// Aravis 0.9.2 GenTL consumer, refuse producers lacking any of them) but not
// modeled. GC_ERR_NOT_IMPLEMENTED is a valid GenTL result.
GC_API IFGetInfo(IF_HANDLE, INTERFACE_INFO_CMD, INFO_DATATYPE*, void*, size_t*) {
    return error(GC_ERR_NOT_IMPLEMENTED, "IFGetInfo not modeled");
}
GC_API IFGetParentTL(IF_HANDLE, TL_HANDLE*) { return error(GC_ERR_NOT_IMPLEMENTED, "IFGetParentTL not modeled"); }
GC_API DevGetParentIF(DEV_HANDLE, IF_HANDLE*) { return error(GC_ERR_NOT_IMPLEMENTED, "DevGetParentIF not modeled"); }
GC_API DSGetParentDev(DS_HANDLE, DEV_HANDLE*) { return error(GC_ERR_NOT_IMPLEMENTED, "DSGetParentDev not modeled"); }
GC_API DSGetBufferID(DS_HANDLE, uint32_t, BUFFER_HANDLE*) { return error(GC_ERR_NOT_IMPLEMENTED, "DSGetBufferID not modeled"); }
GC_API DSGetBufferChunkData(DS_HANDLE, BUFFER_HANDLE, SINGLE_CHUNK_DATA*, size_t*) {
    return error(GC_ERR_NOT_IMPLEMENTED, "chunks not modeled");
}
GC_API DSGetNumBufferParts(DS_HANDLE, BUFFER_HANDLE, uint32_t* parts) {
    if (parts) *parts = 0;
    return error(GC_ERR_NOT_IMPLEMENTED, "multipart not modeled");
}
GC_API DSGetBufferPartInfo(DS_HANDLE, BUFFER_HANDLE, uint32_t, BUFFER_PART_INFO_CMD, INFO_DATATYPE*, void*, size_t*) {
    return error(GC_ERR_NOT_IMPLEMENTED, "multipart not modeled");
}
GC_API EventGetDataInfo(EVENT_HANDLE, const void*, size_t, EVENT_DATA_INFO_CMD, INFO_DATATYPE*, void*, size_t*) {
    return error(GC_ERR_NOT_IMPLEMENTED, "event data info not modeled");
}
GC_API EventGetInfo(EVENT_HANDLE, EVENT_INFO_CMD, INFO_DATATYPE*, void*, size_t*) {
    return error(GC_ERR_NOT_IMPLEMENTED, "event info not modeled");
}
GC_API EventFlush(EVENT_HANDLE) { return error(GC_ERR_NOT_IMPLEMENTED, "EventFlush not modeled"); }
#endif // GENTL_MOCK_MINIMAL

} // extern "C"
