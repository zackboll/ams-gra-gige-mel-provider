// GIGE-MEL-040-R2 test-only, stage-selectable native lifecycle harness.
// No production adapter, PvDevice, discovery, MEL or license interaction.
// Every scenario runs in a fresh isolated process launched by
// scripts/pleora_tsan_stage_audit.py; each STAGE line is an observer barrier.
// Exit 0 only after the complete scenario and "SCENARIO_COMPLETE <name>".
#include <PvBuffer.h>
#include <PvPipeline.h>
#include <PvResult.h>
#include <PvStreamGEV.h>

#include <net/if.h>
#include <pthread.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr std::string_view scenarios[] = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "d1", "d2"};

struct StepFailed : std::runtime_error { using std::runtime_error::runtime_error; };

void stage(const char* name) {
    std::cout << "STAGE " << name << std::endl;
    std::string response;
    if (!std::getline(std::cin, response) || response != "continue")
        throw std::runtime_error("AUDIT_INCOMPLETE: observer barrier missing");
}

void report(const char* name, const PvResult& value) {
    std::cout << "RESULT " << name << " code=" << value.GetCode()
              << " name=" << value.GetCodeString().GetAscii()
              << " description=" << value.GetDescription().GetAscii() << std::endl;
}

// Dependent operations never continue after an unexpected SDK result.
void require_ok(const char* name, const PvResult& value) {
    report(name, value);
    if (!value.IsOK()) throw StepFailed(std::string("SDK_STEP_FAILED ") + name);
}

void isolated() {
    auto* interfaces = if_nameindex();
    if (!interfaces) throw std::runtime_error("BLOCKED: cannot inventory interfaces");
    std::vector<std::string> names;
    for (auto* entry = interfaces; entry->if_index != 0; ++entry) names.emplace_back(entry->if_name);
    if_freenameindex(interfaces);
    const bool exact = names.size() == 2 &&
        ((names[0] == "lo" && names[1] == "veth-rx") || (names[0] == "veth-rx" && names[1] == "lo"));
    if (!exact) throw std::runtime_error("BLOCKED: requires the disposable namespace containing only lo and veth-rx");
}

std::unique_ptr<PvStreamGEV> open_stream() {
    auto stream = std::make_unique<PvStreamGEV>();
    stage("stream_constructed");
    // Exactly the R1 failing path: fixed port 41040, channel 0, explicit local IP, capacity 16.
    require_ok("Open", stream->Open(PvString("10.64.40.2"), 41040, 0, PvString("10.64.40.1"), 16));
    std::cout << "LOCAL " << stream->GetLocalIPAddress().GetAscii() << ':' << stream->GetLocalPort() << std::endl;
    std::cout << "QUEUED_BUFFER_MAXIMUM " << stream->GetQueuedBufferMaximum() << std::endl;
    stage("stream_opened");
    return stream;
}

void close_stream(std::unique_ptr<PvStreamGEV>& stream) {
    stage("before_close");
    require_ok("Close", stream->Close());
    std::cout << "IS_OPEN_AFTER_CLOSE " << stream->IsOpen() << std::endl;
    stage("after_close");
    stream.reset();
    stage("stream_destroyed");
}

std::unique_ptr<PvPipeline> make_pipeline(PvStreamGEV& stream) {
    auto pipeline = std::make_unique<PvPipeline>(&stream);
    // Same documented configuration as the R1 failing path.
    require_ok("SetBufferCount", pipeline->SetBufferCount(4));
    pipeline->SetBufferSize(65536);
    stage("pipeline_constructed");
    return pipeline;
}

void start(PvPipeline& pipeline) {
    stage("before_pipeline_start");
    require_ok("Pipeline.Start", pipeline.Start());
    std::cout << "IS_STARTED_AFTER_START " << pipeline.IsStarted() << std::endl;
    stage("pipeline_started");
}

void stop(PvPipeline& pipeline) {
    // No receive call is active and no retrieved buffer is retained here.
    stage("before_pipeline_stop");
    const auto begin = std::chrono::steady_clock::now();
    const auto result = pipeline.Stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count();
    std::cout << "STOP_RETURNED us=" << elapsed << std::endl;
    require_ok("Pipeline.Stop", result);
    std::cout << "IS_STARTED_AFTER_STOP " << pipeline.IsStarted() << std::endl;
    stage("after_pipeline_stop");
}

void destroy(std::unique_ptr<PvPipeline>& pipeline) {
    stage("before_pipeline_destroy");
    pipeline.reset();
    stage("pipeline_destroyed");
}

// Retrieve exactly one buffer and always return it before any further step.
bool receive_once(PvPipeline& pipeline, bool require_buffer) {
    PvBuffer* buffer = nullptr;
    PvResult operation;
    const auto received = pipeline.RetrieveNextBuffer(&buffer, 250, &operation);
    report("RetrieveNextBuffer", received);
    if (!received.IsOK()) {
        if (require_buffer || received.GetCode() != PvResult::Code::TIMEOUT)
            throw StepFailed("SDK_STEP_FAILED RetrieveNextBuffer");
        return false;
    }
    if (!buffer) throw StepFailed("SDK_STEP_FAILED RetrieveNextBuffer returned no buffer");
    // NO_LICENSE is retained as the SDK's operation result, never rewritten.
    report("Buffer.Operation", operation);
    std::cout << "BUFFER id=" << buffer->GetBlockID() << " size=" << buffer->GetAcquiredSize() << std::endl;
    require_ok("ReleaseBuffer", pipeline.ReleaseBuffer(buffer));
    return true;
}

void pipeline_scenario(std::string_view name) {
    auto stream = open_stream();
    const unsigned cycles = name == "s7" ? 2U : 1U;
    for (unsigned cycle = 0; cycle != cycles; ++cycle) {
        std::cout << "PIPELINE_CYCLE " << cycle + 1 << std::endl;
        auto pipeline = make_pipeline(*stream);
        if (name != "s3") {
            start(*pipeline);
            if (name == "s5") {
                if (receive_once(*pipeline, false)) throw StepFailed("SDK_STEP_FAILED unexpected buffer on idle pipeline");
                stage("receive_returned");
            } else if (name == "s6") {
                bool delivered = false;
                for (unsigned poll = 0; poll != 8 && !delivered; ++poll) delivered = receive_once(*pipeline, false);
                if (!delivered) throw StepFailed("SDK_STEP_FAILED no genuine buffer retrieved");
                stage("buffer_released");
            }
            stop(*pipeline);
        }
        destroy(pipeline);
    }
    if (name == "s8") {
        // Allow any detached SDK workers to finish before Close/destruction.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        stage("post_stop_quiescent");
    }
    close_stream(stream);
}

// Documented PvStream queue/retrieve/abort/dequeue lifecycle (PvStreamSample).
void direct_scenario(std::string_view name) {
    auto stream = open_stream();
    const auto maximum = stream->GetQueuedBufferMaximum();
    const uint32_t count = maximum < 4 ? maximum : 4;
    if (count == 0) throw StepFailed("SDK_STEP_FAILED GetQueuedBufferMaximum returned zero");
    std::vector<std::unique_ptr<PvBuffer>> buffers;
    for (uint32_t index = 0; index != count; ++index) {
        buffers.push_back(std::make_unique<PvBuffer>());
        require_ok("Buffer.Alloc", buffers.back()->Alloc(65536));
    }
    for (auto& buffer : buffers) {
        const auto queued = stream->QueueBuffer(buffer.get());
        report("QueueBuffer", queued);
        if (!queued.IsPending()) throw StepFailed("SDK_STEP_FAILED QueueBuffer not pending");
    }
    std::cout << "QUEUED " << stream->GetQueuedBufferCount() << std::endl;
    stage("buffers_queued");
    bool delivered = false;
    for (unsigned poll = 0; poll != (name == "d2" ? 8U : 1U) && !delivered; ++poll) {
        PvBuffer* buffer = nullptr;
        PvResult operation;
        const auto received = stream->RetrieveBuffer(&buffer, &operation, 250);
        report("RetrieveBuffer", received);
        if (received.IsOK() && buffer) {
            report("Buffer.Operation", operation);
            std::cout << "BUFFER id=" << buffer->GetBlockID() << " size=" << buffer->GetAcquiredSize() << std::endl;
            const auto requeued = stream->QueueBuffer(buffer);
            report("QueueBuffer", requeued);
            if (!requeued.IsPending()) throw StepFailed("SDK_STEP_FAILED requeue not pending");
            delivered = true;
        } else if (received.GetCode() != PvResult::Code::TIMEOUT) {
            throw StepFailed("SDK_STEP_FAILED RetrieveBuffer");
        }
    }
    if (name == "d1" && delivered) throw StepFailed("SDK_STEP_FAILED unexpected buffer on idle stream");
    if (name == "d2" && !delivered) throw StepFailed("SDK_STEP_FAILED no genuine buffer retrieved");
    stage("receive_returned");
    require_ok("AbortQueuedBuffers", stream->AbortQueuedBuffers());
    unsigned dequeued = 0;
    while (stream->GetQueuedBufferCount() > 0) {
        if (++dequeued > count) throw StepFailed("SDK_STEP_FAILED dequeue did not converge");
        PvBuffer* buffer = nullptr;
        PvResult operation;
        report("RetrieveBuffer.Abort", stream->RetrieveBuffer(&buffer, &operation, 1000));
        report("Buffer.AbortOperation", operation);
    }
    std::cout << "DEQUEUED " << dequeued << std::endl;
    stage("buffers_dequeued");
    close_stream(stream);
    buffers.clear(); // harness-owned buffers are freed only after Close
    stage("buffers_freed");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3 || std::string_view(argv[2]) != "veth")
            throw std::runtime_error("usage: pleora_tsan_stage_audit s0..s8|d1|d2 veth (observer required)");
        const std::string_view name = argv[1];
        bool known = false;
        for (auto candidate : scenarios) known = known || candidate == name;
        if (!known) throw std::runtime_error("BLOCKED: unknown scenario");
        isolated();
        std::cout << "SCENARIO " << name << " main_pthread=0x" << std::hex
                  << static_cast<std::uintptr_t>(pthread_self()) << std::dec << std::endl;
        stage("runtime_initialized");
        if (name == "s0") {
            report("PvResult.OK", PvResult(PvResult::Code::OK)); // SDK library initialized, no stream
        } else if (name == "s1") {
            auto stream = std::make_unique<PvStreamGEV>();
            stage("stream_constructed");
            stream.reset();
            stage("stream_destroyed");
        } else if (name == "s2") {
            auto stream = open_stream();
            close_stream(stream);
        } else if (name[0] == 'd') {
            direct_scenario(name);
        } else {
            pipeline_scenario(name);
        }
        stage("before_process_exit");
        std::cout << "SCENARIO_COMPLETE " << name << std::endl;
        return 0;
    } catch (const StepFailed& error) {
        std::cout << error.what() << std::endl;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
