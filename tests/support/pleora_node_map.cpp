#include "pleora_node_map.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>

namespace pleora_node_map {
namespace {

// No vendor XML is copied into this fixture. Named profile nodes, access
// predicates, converters, coupled ROI bounds and register layout are declared
// here so the real GenApi parser and node implementations determine behavior.
constexpr const char* xml = R"xml(<?xml version="1.0" encoding="utf-8"?>
<RegisterDescription ModelName="GigeMelTest" VendorName="GigeMelTests"
 StandardNameSpace="None" SchemaMajorVersion="1" SchemaMinorVersion="1" SchemaSubMinorVersion="0"
 MajorVersion="1" MinorVersion="0" SubMinorVersion="0"
 ProductGuid="C546CD34-2338-4C66-AEFB-F08AD524B120"
 VersionGuid="98E56124-0250-4260-8200-123456789ABC"
 xmlns="http://www.genicam.org/GenApi/Version_1_1">
 <Category Name="Root"><pFeature>Width</pFeature><pFeature>Height</pFeature><pFeature>ExposureTime</pFeature></Category>
 <Port Name="Device"/>

 <Integer Name="SensorWidth"><ImposedAccessMode>RO</ImposedAccessMode><Value>640</Value></Integer>
 <Integer Name="SensorHeight"><ImposedAccessMode>RO</ImposedAccessMode><Value>480</Value></Integer>
 <Integer Name="WidthMinimum"><Value>2</Value></Integer>
 <Integer Name="WidthIncrement"><Value>2</Value></Integer>
 <Integer Name="Width"><pIsLocked>WidthLocked</pIsLocked><pValue>WidthReg</pValue><pMin>WidthMinimum</pMin><pMax>WidthMax</pMax><pInc>WidthIncrement</pInc></Integer>
 <IntReg Name="WidthReg"><Address>0x00</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <IntSwissKnife Name="WidthMax"><pVariable Name="S">SensorWidth</pVariable><pVariable Name="O">OffsetXReg</pVariable><Formula>S-O</Formula></IntSwissKnife>
 <Integer Name="Height"><pValue>HeightReg</pValue><Min>2</Min><pMax>HeightMax</pMax><Inc>2</Inc></Integer>
 <IntReg Name="HeightReg"><Address>0x08</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <IntSwissKnife Name="HeightMax"><pVariable Name="S">SensorHeight</pVariable><pVariable Name="O">OffsetYReg</pVariable><Formula>S-O</Formula></IntSwissKnife>
 <Integer Name="OffsetX"><pValue>OffsetXReg</pValue><Min>0</Min><pMax>OffsetXMax</pMax><Inc>2</Inc></Integer>
 <IntReg Name="OffsetXReg"><Address>0x10</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <IntSwissKnife Name="OffsetXMax"><pVariable Name="S">SensorWidth</pVariable><pVariable Name="W">WidthReg</pVariable><Formula>S-W</Formula></IntSwissKnife>
 <Integer Name="OffsetY"><pValue>OffsetYReg</pValue><Min>0</Min><pMax>OffsetYMax</pMax><Inc>2</Inc></Integer>
 <IntReg Name="OffsetYReg"><Address>0x18</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <IntSwissKnife Name="OffsetYMax"><pVariable Name="S">SensorHeight</pVariable><pVariable Name="H">HeightReg</pVariable><Formula>S-H</Formula></IntSwissKnife>

 <Float Name="AcquisitionFrameRate"><pIsImplemented>RateImplemented</pIsImplemented><pValue>RateReg</pValue><Min>1</Min><Max>200</Max><Unit>Hz</Unit></Float>
 <Float Name="AcquisitionFrameRateAbs"><pValue>RateReg</pValue><Min>1</Min><Max>200</Max><Unit>Hz</Unit></Float>
 <FloatReg Name="RateReg"><Address>0x20</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Endianess>LittleEndian</Endianess></FloatReg>
 <Float Name="ExposureTime"><pIsImplemented>ExposureImplemented</pIsImplemented><pIsAvailable>ExposureAvailable</pIsAvailable><pValue>ExposureReg</pValue><Min>1</Min><pMax>ExposureMaximum</pMax><Unit>us</Unit></Float>
 <Float Name="ExposureTimeAbs"><pValue>ExposureReg</pValue><Min>1</Min><pMax>ExposureMaximum</pMax><Unit>us</Unit></Float>
 <FloatReg Name="ExposureReg"><Address>0x28</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Endianess>LittleEndian</Endianess></FloatReg>
 <SwissKnife Name="ExposureMaximum"><pVariable Name="R">AcquisitionFrameRateAbs</pVariable><Formula>1000000/R</Formula></SwissKnife>
 <IntReg Name="ExposureImplemented"><Address>0x30</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <IntReg Name="RateImplemented"><Address>0x38</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <IntReg Name="ExposureAvailable"><Address>0x40</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <IntReg Name="WidthLocked"><Address>0x48</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>

 <Enumeration Name="PixelFormat"><EnumEntry Name="Mono8"><pIsAvailable>Mono8Available</pIsAvailable><Value>17301505</Value></EnumEntry><EnumEntry Name="Mono12"><Value>17825797</Value></EnumEntry><pValue>PixelFormatReg</pValue></Enumeration>
 <IntReg Name="PixelFormatReg"><Address>0x50</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <IntReg Name="Mono8Available"><Address>0x58</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <Boolean Name="AcquisitionFrameRateEnable"><pValue>FrameRateEnableReg</pValue><OnValue>1</OnValue><OffValue>0</OffValue></Boolean>
 <IntReg Name="FrameRateEnableReg"><Address>0x60</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <Enumeration Name="TriggerMode"><EnumEntry Name="Off"><Value>0</Value></EnumEntry><EnumEntry Name="On"><Value>1</Value></EnumEntry><pValue>TriggerModeReg</pValue></Enumeration>
 <IntReg Name="TriggerModeReg"><Address>0x68</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <String Name="TriggerSelector"><Value>FrameStart</Value></String>
 <String Name="ExposureAuto"><Value>Off</Value></String>
 <String Name="DeviceStreamChannelEndianness"><Value>Little</Value></String>
 <String Name="Description"><Value>original local GenApi fixture</Value></String>

 <Float Name="FixedFloat"><pValue>FixedFloatReg</pValue><Min>0.5</Min><Max>10.5</Max><Inc>0.25</Inc><Unit>us</Unit></Float>
 <FloatReg Name="FixedFloatReg"><Address>0x70</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Endianess>LittleEndian</Endianess></FloatReg>
 <Integer Name="ListInteger"><Value>2</Value><Min>2</Min><Max>8</Max><ValidValueSet>2;4;8</ValidValueSet></Integer>
 <Converter Name="ListFloat"><FormulaTo>FROM*2</FormulaTo><FormulaFrom>TO/2</FormulaFrom><pValue>ListInteger</pValue><Slope>Increasing</Slope></Converter>
 <Integer Name="NeverImplemented"><pIsImplemented>Zero</pIsImplemented><Value>1</Value></Integer>
 <Integer Name="BrokenImplementation"><pIsImplemented>BrokenImplementedReg</pIsImplemented><Value>1</Value></Integer>
 <IntReg Name="BrokenImplementedReg"><Address>0x78</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <Integer Name="Zero"><Value>0</Value></Integer>
 <Integer Name="ReadOnly"><ImposedAccessMode>RO</ImposedAccessMode><Value>7</Value><Min>0</Min><Max>20</Max></Integer>
 <Integer Name="SignedInteger"><pValue>SignedIntegerReg</pValue><Min>-9223372036854775808</Min><Max>9223372036854775807</Max><Inc>1</Inc></Integer>
 <IntReg Name="SignedIntegerReg"><Address>0x80</Address><Length>8</Length><AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>WriteThrough</Cachable><Sign>Signed</Sign><Endianess>LittleEndian</Endianess></IntReg>
 <Integer Name="WriteOnly"><pValue>WriteOnlyReg</pValue><Min>0</Min><Max>20</Max></Integer>
 <IntReg Name="WriteOnlyReg"><Address>0x88</Address><Length>8</Length><AccessMode>WO</AccessMode><pPort>Device</pPort><Sign>Unsigned</Sign><Endianess>LittleEndian</Endianess></IntReg>
</RegisterDescription>)xml";

std::size_t checked_offset(std::int64_t address, std::int64_t length) {
    if (address < 0 || length < 0 || address > 256 || length > 256 - address)
        throw std::out_of_range("test GenApi port range");
    return static_cast<std::size_t>(address);
}

} // namespace

void Port::Read(void* buffer, std::int64_t address, std::int64_t length) {
    calls.push_back({false, address, length});
    if (fail_read == address)
        throw PV_GENICAM_NS::GenericException("injected test port read failure", __FILE__, __LINE__);
    const auto offset = checked_offset(address, length);
    std::memcpy(buffer, bytes_.data() + offset, static_cast<std::size_t>(length));
}

void Port::Write(const void* buffer, std::int64_t address, std::int64_t length) {
    calls.push_back({true, address, length});
    if (fail_write == address)
        throw PV_GENICAM_NS::GenericException("injected test port write failure", __FILE__, __LINE__);
    const auto offset = checked_offset(address, length);
    std::memcpy(bytes_.data() + offset, buffer, static_cast<std::size_t>(length));
    if (after_write) after_write(address);
}

void Port::integer(std::int64_t address, std::int64_t value) {
    const auto offset = checked_offset(address, 8);
    const auto bits = std::bit_cast<std::uint64_t>(value);
    for (std::size_t byte = 0; byte < 8; ++byte)
        bytes_[offset + byte] = static_cast<std::byte>((bits >> (8 * byte)) & 0xffu);
}

std::int64_t Port::integer(std::int64_t address) const {
    const auto offset = checked_offset(address, 8);
    std::uint64_t bits = 0;
    for (std::size_t byte = 0; byte < 8; ++byte)
        bits |= static_cast<std::uint64_t>(bytes_[offset + byte]) << (8 * byte);
    return std::bit_cast<std::int64_t>(bits);
}

void Port::real(std::int64_t address, double value) { integer(address, std::bit_cast<std::int64_t>(value)); }
double Port::real(std::int64_t address) const { return std::bit_cast<double>(integer(address)); }

std::size_t Port::write_attempts(std::optional<std::int64_t> address) const {
    return static_cast<std::size_t>(std::count_if(calls.begin(), calls.end(), [&](const Access& call) {
        return call.write && (!address || call.address == *address);
    }));
}

Fixture::Fixture() {
    port.integer(width, 320);
    port.integer(height, 240);
    port.real(rate, 50.0);
    port.real(exposure, 1000.0);
    port.integer(exposure_implemented, 1);
    port.integer(rate_implemented, 1);
    port.integer(exposure_available, 1);
    port.integer(pixel_format, 0x01080001);
    port.integer(mono8_available, 1);
    port.integer(frame_rate_enabled, 1);
    port.real(fixed_float, 1.0);
    port.integer(broken_implemented, 1);
    map._LoadXMLFromString(xml);
    if (!map._Connect(&port, "Device")) throw std::runtime_error("could not connect original GenApi test port");
}

} // namespace pleora_node_map
