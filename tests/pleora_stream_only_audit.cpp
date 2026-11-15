// Test-only native receiver. No production adapter, PvDevice, discovery or MEL.
#include <PvStreamGEV.h>
#include <PvPipeline.h>
#include <PvBuffer.h>
#include <PvGenBoolean.h>
#include <PvGenParameterArray.h>

#include <net/if.h>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void stage(const char* name) {
    std::cout << "STAGE " << name << std::endl;
    std::string response;
    if (!std::getline(std::cin, response) || response != "continue")
        throw std::runtime_error("AUDIT_INCOMPLETE: observer barrier missing");
}
bool result(const char* name, const PvResult& value) {
    std::cout << "RESULT " << name << " code=" << value.GetCode()
              << " name=" << value.GetCodeString().GetAscii()
              << " description=" << value.GetDescription().GetAscii() << std::endl;
    return value.IsOK();
}
void isolated() {
    auto* interfaces = if_nameindex();
    if (!interfaces) throw std::runtime_error("BLOCKED: cannot inventory interfaces");
    bool only_loopback = true;
    unsigned count = 0;
    for (auto* entry = interfaces; entry->if_index != 0; ++entry) {
        ++count;
        only_loopback = only_loopback && std::string(entry->if_name) == "lo";
    }
    if_freenameindex(interfaces);
    if (!only_loopback || count != 1)
        throw std::runtime_error("BLOCKED: requires a fresh network namespace containing only lo");
}
}

int main(int argc, char** argv) {
    try {
        isolated();
        if (argc != 2 || (std::string(argv[1]) != "default" && std::string(argv[1]) != "disabled"))
            throw std::runtime_error("usage: pleora_stream_only_audit default|disabled (observer required)");
        const bool disable = std::string(argv[1]) == "disabled";
        stage("runtime_initialized");
        auto stream = std::make_unique<PvStreamGEV>();
        stage("stream_constructed");
        stage("before_open");
        const bool opened = result("Open", stream->Open(PvString("127.0.0.2"),
            static_cast<uint16_t>(41040), 0, PvString("127.0.0.1"), 16));
        stage("after_open");
        bool completed = opened;
        if (opened) {
            std::cout << "LOCAL " << stream->GetLocalIPAddress().GetAscii() << ':'
                      << stream->GetLocalPort() << std::endl;
            auto* resend = dynamic_cast<PvGenBoolean*>(stream->GetParameters()->Get("RequestMissingPackets"));
            bool enabled = false;
            if (!resend || !result("RequestMissingPackets.GetValue", resend->GetValue(enabled))) {
                completed = false;
            } else {
                std::cout << "RESEND default=" << enabled << " type=PvGenBoolean" << std::endl;
                if (disable) {
                    completed = result("RequestMissingPackets.SetValue", resend->SetValue(false)) && completed;
                    completed = result("RequestMissingPackets.Readback", resend->GetValue(enabled)) && completed;
                    completed = !enabled && completed;
                    std::cout << "RESEND readback=" << enabled << std::endl;
                }
            }
            stage("parameters_read");
            auto pipeline = std::make_unique<PvPipeline>(stream.get());
            stage("pipeline_created");
            completed = result("SetBufferCount", pipeline->SetBufferCount(4)) && completed;
            pipeline->SetBufferSize(65536);
            const bool started = result("Pipeline.Start", pipeline->Start());
            completed = started && completed;
            stage("pipeline_started");
            if (started) {
                for (unsigned index = 0; index != 4; ++index) {
                    PvBuffer* buffer = nullptr;
                    PvResult operation;
                    const auto received = pipeline->RetrieveNextBuffer(&buffer, 250, &operation);
                    result("RetrieveNextBuffer", received);
                    if (received.IsOK() && buffer) {
                        result("Buffer.Operation", operation);
                        std::cout << "BUFFER id=" << buffer->GetBlockID() << " size="
                                  << buffer->GetAcquiredSize() << std::endl;
                        completed = result("ReleaseBuffer", pipeline->ReleaseBuffer(buffer)) && completed;
                    }
                    stage("receive_poll");
                }
                stage("before_pipeline_stop");
                const auto begin = std::chrono::steady_clock::now();
                completed = result("Pipeline.Stop", pipeline->Stop()) && completed;
                std::cout << "STOP_US " << std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - begin).count() << std::endl;
                stage("pipeline_stopped");
            }
            pipeline.reset();
            stage("pipeline_destroyed");
        }
        stage("before_close");
        const bool closed = result("Close", stream->Close());
        stage("after_close");
        stream.reset();
        stage("stream_destroyed");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        stage("before_process_exit");
        std::cout << (completed && closed ? "API_LIFECYCLE_COMPLETE" : "NATIVE_RECEIVE_PENDING") << std::endl;
        // A successful API lifecycle is NOT a traffic or reception qualification.
        return completed && closed ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}