#pragma once

// Private native boundary, also used by tests with real, locally allocated
// PvBuffers. Test sources are not a GigE transport or a camera simulator.
#include "pleora_backend.hpp"
#include <PvBuffer.h>
#include <PvResult.h>

namespace gige_mel::backend::pleora {

struct RunConfiguration {
    CameraConfiguration configuration;
    PixelFormat format{PixelFormat::mono8};
    std::size_t payload_bytes{0};
};
struct NativeBuffer {
    PvBuffer* buffer{nullptr};
    PvResult operation;
};
class NativeSource {
public:
    virtual ~NativeSource() = default;
    virtual Result<RunConfiguration> start() = 0;
    virtual Result<NativeBuffer> next(std::uint32_t timeout_ms) = 0;
    virtual Status release(PvBuffer* buffer) = 0;
    virtual Status stop() = 0;
};

// Construct a stopped wrapper, retaining the native source and (in production)
// the selected per-device profile state without native I/O. Frame leases pin
// copied host bytes, not the pipeline or requeued native buffers. The one-argument
// overload uses default monochrome frame admission for controlled test sources.
std::unique_ptr<IFrameStream> make_stream(std::shared_ptr<NativeSource> source);
std::unique_ptr<IFrameStream> make_stream(std::shared_ptr<NativeSource>,
    std::shared_ptr<const profiles::ICameraProfileState>);

} // namespace gige_mel::backend::pleora
