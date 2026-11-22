#pragma once

// Private device/stream composition boundary. Native runs, buffers, and the
// concrete stream stay in aravis_stream.cpp; device ownership and close
// stay in aravis_backend.cpp. Never installed.
#include "aravis_native.hpp"

#include <mutex>

namespace gige_mel::backend::profiles {
class ICameraProfile;
class ICameraProfileState;
} // namespace gige_mel::backend::profiles

namespace gige_mel::backend::aravis {

struct TestHooks;

namespace detail {

class Reservation;
struct ControlLoss;
struct NativeOpen;
struct Run;

// Adapter command/lifecycle state of camera acquisition (GIGE-MEL-009-R1):
// what the adapter knows about the outcome of ITS OWN AcquisitionStart and
// AcquisitionStop commands. It is not a measurement of camera exposure or
// readout, and it is distinct from ownership of a native receiver (a Run).
enum class Acquisition {
    none,     // no unresolved acquisition command
    owned,    // AcquisitionStart succeeded and a run owns the acquisition
    uncertain // a start/stop command was attempted and its outcome is unknown
};

struct DeviceCore {
    DeviceCore(NativeOpen native, CameraCapabilities c, std::shared_ptr<const profiles::ICameraProfileState> e,
               std::shared_ptr<const profiles::ICameraProfile> p, std::shared_ptr<const TestHooks> h);
    ~DeviceCore();
    void disconnect_loss() noexcept;
    bool lost() const noexcept;

    std::mutex mutex; // D
    std::unique_ptr<Reservation> reservation;
    GObjectPtr<ArvDevice> device; // null after logical close
    const CameraCapabilities caps;
    // Retain the selected profile until after policy destruction.
    const std::shared_ptr<const profiles::ICameraProfile> profile_owner;
    const std::shared_ptr<const profiles::ICameraProfileState> policy;
    const std::shared_ptr<const TestHooks> hooks;
    const std::shared_ptr<ControlLoss> control_loss;
    gulong loss_handler{0}; // D; disconnected before releasing the native device
    // Three distinct private concepts (GIGE-MEL-009-R1):
    //  - receiver ownership: `active`, the run owning a native stream;
    //  - camera-command outcome: `acquisition` and the wrapper token `owner`
    //    that started it or must recover it (meaningful unless `none`);
    //  - history: `fault`, a failure from the void stop() interface kept for
    //    one report by close(); never itself an admission gate.
    // Invariants after every public operation: `owned` <=> `active` is set;
    // `uncertain` => no active run. Retiring a native receiver never proves
    // that AcquisitionStop succeeded; only a successful command clears it.
    std::shared_ptr<Run> active; // the run owning native acquisition, if any
    std::shared_ptr<Run> latest; // most recent successful run (restart lease rule)
    Acquisition acquisition{Acquisition::none};
    std::uint64_t owner{0};             // AravisStream token; 0 = none
    const char* attempted{nullptr};     // command(s) behind the current state (static text)
    std::uint64_t attempted_generation{0};
    struct Fault {
        Error error;                       // first unrecovered failure, with context
        const char* recovered_by{nullptr}; // set (no allocation) once a later stop succeeded
    };
    std::optional<Fault> fault; // not yet reported by close()
};

Error control_loss_error();

// Caller holds the device operation lock. Generation requires an active
// receiver. The close path may retire the run or stop an uncertain command
// directly when there is no receiver to retire.
std::uint64_t active_generation_locked(const DeviceCore& core) noexcept;
Error uncertain_refusal(const DeviceCore& core, const std::string& what);
Status stop_command_locked(DeviceCore& core, const char* recovered_by);
void retire_locked(DeviceCore& core, bool closing, std::optional<Error>* close_failure = nullptr) noexcept;

// Construct a stopped wrapper, retaining the device state without native I/O.
std::unique_ptr<IFrameStream> make_stream(std::shared_ptr<DeviceCore> core);

} // namespace detail

} // namespace gige_mel::backend::aravis
