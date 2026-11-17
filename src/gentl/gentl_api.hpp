#pragma once

// Private to the GenTL feasibility foundation and its fixture producer. Uses
// the unmodified official EMVA GenTL.h (GenTL 1.6, SHA-256 verified by CMake
// at configure time). The header is provisioned outside the repository by
// scripts/provision_gentl_header.sh and is never committed or installed.
#include <GenTL.h>

#if !defined(GC_GENTL_HEADER_VERSION) || GenTLMajorVersion != 1 || GenTLMinorVersion != 6
#error "GIGE-MEL-GENTL-001 requires the verified GenTL 1.6 header"
#endif

#include <string_view>

namespace gige_mel::gentl {

namespace G = ::GenTL;

// Pointers use the header's own GC_API_P typedefs for exact ABI signatures.
struct Api {
    // Mandatory: initialization, enumeration, exact open and read-only inspection.
    G::PGCInitLib GCInitLib{};
    G::PGCCloseLib GCCloseLib{};
    G::PGCGetInfo GCGetInfo{};
    G::PGCGetLastError GCGetLastError{};
    G::PGCReadPort GCReadPort{};
    G::PGCGetPortInfo GCGetPortInfo{};
    G::PGCGetNumPortURLs GCGetNumPortURLs{};
    G::PGCGetPortURLInfo GCGetPortURLInfo{};
    G::PTLOpen TLOpen{};
    G::PTLClose TLClose{};
    G::PTLGetInfo TLGetInfo{};
    G::PTLGetNumInterfaces TLGetNumInterfaces{};
    G::PTLGetInterfaceID TLGetInterfaceID{};
    G::PTLGetInterfaceInfo TLGetInterfaceInfo{};
    G::PTLOpenInterface TLOpenInterface{};
    G::PTLUpdateInterfaceList TLUpdateInterfaceList{};
    G::PIFClose IFClose{};
    G::PIFGetNumDevices IFGetNumDevices{};
    G::PIFGetDeviceID IFGetDeviceID{};
    G::PIFUpdateDeviceList IFUpdateDeviceList{};
    G::PIFGetDeviceInfo IFGetDeviceInfo{};
    G::PIFOpenDevice IFOpenDevice{};
    G::PDevGetPort DevGetPort{};
    G::PDevGetInfo DevGetInfo{};
    G::PDevClose DevClose{};
    // Optional group: raw port writes.
    G::PGCWritePort GCWritePort{};
    // Optional group: acquisition. All or nothing.
    G::PDevGetNumDataStreams DevGetNumDataStreams{};
    G::PDevGetDataStreamID DevGetDataStreamID{};
    G::PDevOpenDataStream DevOpenDataStream{};
    G::PDSGetInfo DSGetInfo{};
    G::PDSAnnounceBuffer DSAnnounceBuffer{};
    G::PDSQueueBuffer DSQueueBuffer{};
    G::PDSRevokeBuffer DSRevokeBuffer{};
    G::PDSFlushQueue DSFlushQueue{};
    G::PDSStartAcquisition DSStartAcquisition{};
    G::PDSStopAcquisition DSStopAcquisition{};
    G::PDSGetBufferInfo DSGetBufferInfo{};
    G::PDSClose DSClose{};
    G::PGCRegisterEvent GCRegisterEvent{};
    G::PGCUnregisterEvent GCUnregisterEvent{};
    G::PEventGetData EventGetData{};
    G::PEventKill EventKill{};
};

} // namespace gige_mel::gentl
