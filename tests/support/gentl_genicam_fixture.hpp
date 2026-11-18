#pragma once

// SIMULATED GenICam register model for GIGE-MEL-GENTL-002 offline tests.
// Original test arrangement: it is NOT vendor XML and NOT physical-camera
// evidence. It reproduces only the documented Cognex CIC-5000R identity and
// the feature TYPES/units the unchanged profile reads (docs/cognex-cic-5000r.md).
// Values live in a big- or little-endian register image served by the
// controlled GenTL fixture producer through GCReadPort/GCWritePort.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace gentl_fixture {

constexpr std::size_t xml_address = 0x10000;
constexpr std::size_t port_size = 0x40000;

// Register addresses (simulated layout).
enum : std::uint32_t {
    vendor = 0x0100, model = 0x0120, firmware = 0x0140,  // StringReg, 32 bytes
    sensor_width = 0x0200, sensor_height = 0x0204,
    width = 0x0210, height = 0x0214, offset_x = 0x0218, offset_y = 0x021c,
    pixel_format = 0x0220, trigger_selector = 0x0224, trigger_mode = 0x0228,
    acquisition_mode = 0x022c, rate_enable = 0x0230, exposure_auto = 0x0234,
    rate = 0x0300, exposure = 0x0308, // FloatReg, 8 bytes
    width_inc = 0x0400,               // Integer register driving Width <pInc>
};

struct Model {
    bool big_endian{true};             // every register's <Endianess>
    std::string schema_minor{"1"};     // RegisterDescription SchemaMinorVersion
    std::string vendor_name{"COGNEX"};
    std::string model_name{"CIC-5000R"};
    std::string firmware_version{"106349-08;U;CIC-5000R;V3.7-1;0"};
    std::int64_t sensor_w{2592}, sensor_h{1944};
    std::int64_t w{2592}, h{1944}, x{0}, y{0};
    std::int64_t width_increment{8};   // read through <pInc> from a register
    std::string pixel{"Mono8"};
    std::string trigger{"Off"};
    std::string mode{"Continuous"};
    bool rate_enabled{true};
    double rate_hz{10.0};
    double exposure_us{35000.0};
    // Structural variants for negative tests.
    std::string rate_feature{"AcquisitionFrameRateAbs"};
    std::string exposure_feature{"ExposureTimeAbs"};
    bool rate_enable_is_boolean{true}; // false: declared as an Integer (wrong type)
    bool include_trigger_mode{true};
    bool include_acquisition_mode{true};
    bool include_sensor_width{true};
    std::string extra_nodes;           // appended verbatim inside RegisterDescription
};

inline std::map<std::string, std::int64_t> pixel_values() {
    return {{"Mono8", 0x01080001}, {"Mono12", 0x01100005}, {"RGB8", 0x02180014}};
}

inline std::string endian(const Model& m) { return m.big_endian ? "BigEndian" : "LittleEndian"; }

inline std::string int_reg(const Model& m, const char* name, std::uint32_t address, const char* access,
                           const char* extra = "") {
    char text[512];
    std::snprintf(text, sizeof text,
                  "  <IntReg Name=\"%s\"><Address>0x%x</Address><Length>4</Length><AccessMode>%s</AccessMode>"
                  "<pPort>Device</pPort>%s<Sign>Unsigned</Sign><Endianess>%s</Endianess></IntReg>\n",
                  name, address, access, extra, endian(m).c_str());
    return text;
}

inline std::string enumeration(const Model& m, const char* name, std::uint32_t address,
                               const std::vector<std::pair<std::string, std::int64_t>>& entries) {
    std::string xml = "  <Enumeration Name=\"" + std::string(name) + "\">";
    for (const auto& [entry, value] : entries)
        xml += "<EnumEntry Name=\"" + entry + "\"><Value>" + std::to_string(value) + "</Value></EnumEntry>";
    xml += "<pValue>" + std::string(name) + "Reg</pValue></Enumeration>\n";
    return xml + int_reg(m, (std::string(name) + "Reg").c_str(), address, "RW");
}

inline std::string hex(std::uint32_t address) {
    char a[16];
    std::snprintf(a, sizeof a, "0x%x", address);
    return a;
}

inline std::string integer_feature(const Model& m, const std::string& name, std::uint32_t address,
                                   const std::string& bounds);

// Float feature with explicit native bounds/unit through a (non-linear, so
// increment-free) Converter. `period` models a rate held as an integer
// microsecond period register (Hz = 1e6 / period, as the documented V3.7-1
// quantization); otherwise an identity converter over an 8-byte FloatReg.
inline std::string float_feature(const Model& m, const std::string& name, std::uint32_t address, const char* min,
                                 const char* max, const char* unit, bool period) {
    std::string x = "  <Float Name=\"" + name + "\"><pValue>" + name + "Converter</pValue><Min>" + min +
                    "</Min><Max>" + max + "</Max><Unit>" + unit + "</Unit></Float>\n";
    x += "  <Converter Name=\"" + name + "Converter\"><FormulaTo>" + (period ? "(1000000 / FROM)" : "FROM") +
         "</FormulaTo><FormulaFrom>" + (period ? "(1000000 / TO)" : "TO") + "</FormulaFrom><pValue>" + name +
         (period ? "Period" : "Reg") + "</pValue></Converter>\n";
    // Bounded integer period (as the pinned Aravis Fake models its period):
    // 1 us .. 67114093 us, i.e. 1 MHz .. 0.0149 Hz.
    if (period)
        return x + integer_feature(m, name + "Period", address, "<Min>1</Min><Max>67114093</Max>");
    return x + "  <FloatReg Name=\"" + name + "Reg\"><Address>" + hex(address) +
           "</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Endianess>" + endian(m) +
           "</Endianess></FloatReg>\n";
}

// Integer feature over an IntReg; `bounds` is the verbatim Min/Max/Inc XML.
inline std::string integer_feature(const Model& m, const std::string& name, std::uint32_t address,
                                   const std::string& bounds) {
    return "  <Integer Name=\"" + name + "\"><pValue>" + name + "Reg</pValue>" + bounds + "</Integer>\n" +
           int_reg(m, (name + "Reg").c_str(), address, "RW");
}

inline std::string string_reg(const char* name, std::uint32_t address) {
    return "  <StringReg Name=\"" + std::string(name) + "\"><Address>" + hex(address) +
           "</Address><Length>32</Length><AccessMode>RO</AccessMode><pPort>Device</pPort></StringReg>\n";
}

inline std::string knife(const char* name, const char* sensor, const char* other) {
    return "  <IntSwissKnife Name=\"" + std::string(name) + "\"><pVariable Name=\"S\">" + sensor +
           "</pVariable><pVariable Name=\"O\">" + other + "</pVariable><Formula>S-O</Formula></IntSwissKnife>\n";
}

inline std::string xml(const Model& m) {
    std::string x = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<RegisterDescription ModelName=\"SimulatedCIC5000R\" VendorName=\"GIGE-MEL-fixture\" StandardNameSpace=\"None\" "
        "SchemaMajorVersion=\"1\" SchemaMinorVersion=\"" + m.schema_minor + "\" SchemaSubMinorVersion=\"0\" "
        "MajorVersion=\"1\" MinorVersion=\"0\" SubMinorVersion=\"0\" "
        "ProductGuid=\"5f8e0c52-6a4a-4f53-9d22-6b1f00000101\" VersionGuid=\"5f8e0c52-6a4a-4f53-9d22-6b1f00000102\" "
        "xmlns=\"http://www.genicam.org/GenApi/Version_1_1\">\n"
        "  <Category Name=\"Root\"><pFeature>DeviceVendorName</pFeature></Category>\n";
    x += string_reg("DeviceVendorName", vendor) + string_reg("DeviceModelName", model) +
         string_reg("DeviceFirmwareVersion", firmware);
    if (m.include_sensor_width) x += int_reg(m, "SensorWidth", sensor_width, "RO");
    x += int_reg(m, "SensorHeight", sensor_height, "RO");
    x += int_reg(m, "WidthIncrementReg", width_inc, "RO");
    // Coupled ROI maxima evaluated from live registers on every read.
    x += knife("WidthMax", "SensorWidth", "OffsetX") + knife("HeightMax", "SensorHeight", "OffsetY") +
         knife("OffsetXMax", "SensorWidth", "Width") + knife("OffsetYMax", "SensorHeight", "Height");
    x += integer_feature(m, "Width", width, "<Min>64</Min><pMax>WidthMax</pMax><pInc>WidthIncrementReg</pInc>");
    x += integer_feature(m, "Height", height, "<Min>64</Min><pMax>HeightMax</pMax><Inc>2</Inc>");
    x += integer_feature(m, "OffsetX", offset_x, "<Min>0</Min><pMax>OffsetXMax</pMax><Inc>8</Inc>");
    x += integer_feature(m, "OffsetY", offset_y, "<Min>0</Min><pMax>OffsetYMax</pMax><Inc>2</Inc>");
    x += enumeration(m, "PixelFormat", pixel_format, {{"Mono8", 0x01080001}});
    x += enumeration(m, "TriggerSelector", trigger_selector, {{"FrameStart", 0}});
    if (m.include_trigger_mode) x += enumeration(m, "TriggerMode", trigger_mode, {{"Off", 0}, {"On", 1}});
    if (m.include_acquisition_mode)
        x += enumeration(m, "AcquisitionMode", acquisition_mode, {{"Continuous", 0}, {"SingleFrame", 1}});
    x += enumeration(m, "ExposureAuto", exposure_auto, {{"Off", 0}});
    if (m.rate_enable_is_boolean)
        x += "  <Boolean Name=\"AcquisitionFrameRateEnable\"><pValue>AcquisitionFrameRateEnableReg</pValue>"
             "<OnValue>1</OnValue><OffValue>0</OffValue></Boolean>\n";
    else
        x += "  <Integer Name=\"AcquisitionFrameRateEnable\"><pValue>AcquisitionFrameRateEnableReg</pValue></Integer>\n";
    x += int_reg(m, "AcquisitionFrameRateEnableReg", rate_enable, "RW");
    x += float_feature(m, m.rate_feature, rate, "0.0149", "1000000", "Hz", true);
    x += float_feature(m, m.exposure_feature, exposure, "35", "999985", "us", false);
    x += m.extra_nodes;
    x += "  <Port Name=\"Device\"/>\n</RegisterDescription>\n";
    return x;
}

// Register image matching `m`, with `document` placed at xml_address.
inline std::vector<unsigned char> port(const Model& m, const std::string& document) {
    std::vector<unsigned char> p(port_size, 0);
    const auto put = [&](std::uint32_t at, std::uint64_t value, std::size_t length) {
        for (std::size_t i = 0; i < length; ++i) {
            const auto shift = 8 * (m.big_endian ? length - 1 - i : i);
            p[at + i] = static_cast<unsigned char>((value >> shift) & 0xffu);
        }
    };
    const auto put_text = [&](std::uint32_t at, const std::string& text) {
        std::memcpy(p.data() + at, text.data(), std::min<std::size_t>(text.size(), 32));
    };
    const auto put_double = [&](std::uint32_t at, double value) {
        std::uint64_t bits;
        std::memcpy(&bits, &value, 8);
        put(at, bits, 8);
    };
    put_text(vendor, m.vendor_name);
    put_text(model, m.model_name);
    put_text(firmware, m.firmware_version);
    put(sensor_width, static_cast<std::uint64_t>(m.sensor_w), 4);
    put(sensor_height, static_cast<std::uint64_t>(m.sensor_h), 4);
    put(width_inc, static_cast<std::uint64_t>(m.width_increment), 4);
    put(width, static_cast<std::uint64_t>(m.w), 4);
    put(height, static_cast<std::uint64_t>(m.h), 4);
    put(offset_x, static_cast<std::uint64_t>(m.x), 4);
    put(offset_y, static_cast<std::uint64_t>(m.y), 4);
    put(pixel_format, static_cast<std::uint64_t>(pixel_values().at(m.pixel)), 4);
    put(trigger_mode, m.trigger == "Off" ? 0u : 1u, 4);
    put(acquisition_mode, m.mode == "Continuous" ? 0u : 1u, 4);
    put(rate_enable, m.rate_enabled ? 1u : 0u, 4);
    put(rate, static_cast<std::uint64_t>(std::llround(1e6 / m.rate_hz)), 4); // microsecond period
    put_double(exposure, m.exposure_us);
    std::memcpy(p.data() + xml_address, document.data(), document.size());
    return p;
}

} // namespace gentl_fixture

