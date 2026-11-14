// Original SDK-only investigation program. No project headers or test fixture.
#include <GenApi/GenApi.h>
#include <GenApi/NodeMapRef.h>
#include <Log/CLog.h>

#include <cstdio>
#include <exception>
#include <string_view>

namespace native = GENAPI_NAMESPACE;

// Original minimal schema: no camera port, registers, profile or feature access.
constexpr const char* xml = R"xml(<?xml version="1.0" encoding="utf-8"?>
<RegisterDescription ModelName="LockOrderRepro" VendorName="IndependentRepro"
 StandardNameSpace="None" SchemaMajorVersion="1" SchemaMinorVersion="1" SchemaSubMinorVersion="0"
 MajorVersion="1" MinorVersion="0" SubMinorVersion="0"
 ProductGuid="BA30D4A0-C37F-4183-98BD-1D1749ABF183"
 VersionGuid="1B284445-3D70-4E77-AEA4-966BE015952D"
 xmlns="http://www.genicam.org/GenApi/Version_1_1">
 <Category Name="Root"/>
</RegisterDescription>)xml";

int main(int argc, char** argv) {
    const std::string_view mode = argc == 2 ? argv[1] : "raii";
    if (argc > 2 || (mode != "raii" && mode != "empty" && mode != "destroy" &&
                     mode != "initialize" && mode != "shutdown")) {
        std::fputs("usage: genapi_lock_order [raii|empty|destroy|initialize|shutdown]\n", stderr);
        return 2;
    }
    try {
        std::fputs("phase: entered main\n", stderr);
        if (mode == "initialize") {
            (void)GENICAM_NAMESPACE::CLog::GetLogger("");
            std::fputs("phase: explicit logger initialization complete\n", stderr);
        }
        {
            native::CNodeMapRef map;
            std::fputs("phase: constructed empty reference\n", stderr);
            if (mode != "empty") {
                map._LoadXMLFromString(xml);
                std::fputs("phase: XML loaded\n", stderr);
            }
            if (mode == "destroy") {
                map._Destroy();
                std::fputs("phase: explicit map destruction complete\n", stderr);
            }
        }
        std::fputs("phase: reference scope destroyed\n", stderr);
        if (mode == "shutdown") {
            std::fputs("phase: calling CLog::ShutDown after all maps\n", stderr);
            GENICAM_NAMESPACE::CLog::ShutDown();
            std::fputs("phase: CLog::ShutDown complete\n", stderr);
        }
        std::fputs("phase: returning from main; global teardown follows\n", stderr);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SDK-only reproduction failed: %s\n", error.what());
        return 1;
    }
}
