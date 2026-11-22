# Camera-profile authoring (GIGE-MEL-034/035)

This is the **interface contract**. For the implementation sequence, standalone
build and qualification checklist, start with [Create a Camera Profile](camera-profile-tutorial.md).
Use the [C++ interface reference](index.md#camera-profile-c-interface) and the
[Camera Profile Examples](camera-profile-examples.md) alongside this contract.
A new SDK/transport belongs in [backend authoring](backend-authoring.md) instead. See the
[documentation index](index.md) for navigation and the
[requirements traceability](mel-traceability.md) plus pinned upstream
headers for the full MEL requirements; a camera profile does not itself
implement every required MEL channel or operation.

The [Cognex CIC-5000R profile](../camera_profiles/cognex_cic_5000r/README.md)
is a real-camera-specific module using this unchanged interface: exact identity,
owned state and discrete-rate policy layered on the monochrome defaults without
SDK dependencies. Its qualification record distinguishes fixtures from hardware.

This is an **internal, uninstalled C++20 interface** for statically linked or
explicitly loaded matched-toolchain Linux x86-64 implementations. It is not a
permanently stable, cross-toolchain binary ABI.
Do not expose vendor objects, MEL declarations or native node maps through it.

## Required implementation

Implement `backend::profiles::ICameraProfile` from
`src/profiles/camera_profile.hpp`:

* `id() noexcept`: stable exact canonical selection ID, owned for the object's
  lifetime. The default is `mono_free_running_v1`; the historical diagnostic
  profile ID remains `aravis-mono8-free-running-v1`.
* `open(IDeviceFeatures&) const`: discover aliases/features, build your own
  concrete snapshot, check physical-device compatibility and construct a nonnull
  owned `ICameraProfileState`. Return an error for unsupported devices. Open is
  explicit camera I/O; selection and registration must not call it.

Implement the state operations:

| Operation | Responsibility |
| --- | --- |
| `capabilities()` | Validated immutable interpretation of open-time facts |
| `configuration(features, check)` | Fresh truthful readback, with connection checks surrounding the transaction |
| `apply(features, request, check, before)` | Full-request admission, fresh bounds, ordered native writes, connection/write guards and actual final readback |
| `preflight(features)` | Fresh read-only acquisition admission, including native acquisition-mode policy; freeze the returned configuration for the run |
| `validate_frame(facts, frozen, format)` | Admit only valid selected representations, geometry and checked spans inside received bytes/capacity; return accept/incomplete/reject with diagnostic |
| `native_diagnostics()` (optional) | Owned read-only feature text for internal inspection |

## Reusing the monochrome defaults

[`MonoFreeRunningProfile` and `MonoFreeRunningProfileState`](../src/profiles/mono_free_running_profile.hpp)
implement these interfaces for untriggered Continuous acquisition, manual
exposure, integer-Hz rate control, and single Mono8 or explicitly little-endian
unpacked Mono12 images. The existing factory returns the default profile. It
uses the existing `mono_free_running` algorithms: first-implemented aliases,
checked units/conversions, dynamic constraints, ordered ROI/timing writes,
partial-write diagnostics and truthful readback.

If those assumptions fit your camera, inherit the state and override only the
operations whose policy differs. Override the profile's `open()` to construct
the derived state explicitly: the default `open()` always builds a default
state. Move the validated native snapshot and the advertised capabilities into
the base constructor. Keep native admission facts separate from advertised
limits. For example, Cognex advertises at most 10 Hz, inherits `configuration()`
unchanged, and can read an actual 16 Hz setting and explicitly apply 5 Hz to
recover. The base never validates readback against virtual `capabilities()`.

Cognex checks its discrete request rate and required modes before calling the
base `apply()`. Its `preflight()` admits the already-read snapshot using the
protected `native_admission()` accessor and the shared acquisition helper;
calling the full base preflight would read again and check acquisition mode in
a different order. Preserve native error precedence when selecting what to
reuse. Snapshot access is const and valid only while the state lives. Do not
retain the feature service, connection guard or before-write callback passed to
an operation; the state must outlive its parent profile without their help.

For different native representations, sequencing or ownership needs, implement
the interfaces independently or compose a delegate (as the rate-limited example
does). No common snapshot layout is required. Neither default implements any
optional capability interface. Camera policy can reuse defaults while one common
MEL implementation owns the provider mechanics; it cannot replace arbitrary
unsupported MEL operations.

`IDeviceFeatures` in `src/device_features.hpp` supplies project-owned typed
scalar values, fresh numeric bounds/increments/units, implementation queries,
enum-entry availability and checked integer/float writes. Adapters check native
type, access, availability and write-time constraints. A failed query is not
absence. Profile policy decides how to interpret the facts; it must not silently
normalize trigger, format, timing or other unavailable behavior. The current
interface deliberately excludes generic register transport, enum setters and
new payloads. Add operations only with a concrete reviewed use case.

## Register and select

`BackendCatalog` is explicitly owned. It contains a default monochrome profile
factory; `register_profile(id, factory)` adds independently implemented profiles.
Factories return `Result<shared_ptr<const ICameraProfile>>`, make independent
instances per selection and do no device I/O. Register profiles before compatible
backends; duplicates, missing profiles and incompatible selections fail. Backend
constructors take the selected profile and **must inject it** into the adapter.
They must not ignore it or choose a fallback. Compatibility is a composition
promise about source interfaces, not qualification of a particular camera.

For example, an application built in this source tree with the optional Aravis
target can explicitly select backend, device and default profile:

```cpp
#include "selection/session_selection.hpp"
#include "backends/aravis/aravis_backend.hpp"
#include <stdexcept>

auto create_session(const std::string& exact_device_id) {
    namespace s = gige_mel::selection;
    s::BackendCatalog catalog;
    auto status = catalog.register_backend("aravis",
        std::vector<std::string>{std::string(s::mono_free_running_v1)},
        [](s::BackendCatalog::Profile profile) {
            return gige_mel::backend::aravis::make_backend(std::move(profile));
        });
    if (!status) throw std::runtime_error(status.error().message);
    auto selected = s::select_session(catalog,
        {"aravis", exact_device_id, "mono_free_running_v1", {}});
    if (!selected) throw std::runtime_error(selected.error().message);
    auto& session = selected.value();
    return gige_mel::make_api_manager(session.settings, session.backend);
}
```

The function returns a manager after the catalog is destroyed, without opening
a camera. Normal MEL `getControl(...)->init()` performs explicit open. For Pleora,
include `backends/pleora/pleora_backend.hpp`, register ID `pleora`, inject using
`backend::pleora::make_backend(std::move(profile))`, and select the exact discovered
MAC address. No evaluation permission is enabled by this example. The built-in
catalog already composes compiled adapters with these defaults.

To add your profile, register its exact ID with a factory, include that ID in
either/both backend compatibility lists and select it instead. One implementation
uses either adapter's `IDeviceFeatures`; camera policy must not branch on SDK
types. Zero-argument adapter factories still select the existing monochrome
profile. Synthetic's legacy fixed-policy constructor supports only its default
binding, not arbitrary profiles.

## Ownership, threading and errors

Backends own the selected profile. Device cores own both the profile and its
immutable state. Streams retain device state through native teardown; Pleora's
copy stream captures the selected state's frame validator. Outstanding leases
retain their original storage owners and bytes after stop/close. States must own
all their dependencies independently of the parent profile: a native run or
lease may retain state beyond profile/backend destruction. Loaded modules use
the additional independently pinned state ownership described below.

Feature services, connection guards and before-write callbacks are borrowed only
for the synchronous operation; **never retain them**. The adapter keeps its
native device and existing operation lock for the transaction. Frame validation
must be thread-safe against other const state operations. Do not add provider
callbacks, lock dependencies or reentry. Catalog mutation is composition-thread
only; immutable selection may be shared when factories are thread-safe.

Use `Result`/`Status` errors, not exceptions for expected camera rejection. Catalog
construction translates allocation and unexpected constructor exceptions.
Null/mismatched profile factory results and null open states fail explicitly.
Apply errors preserve native provenance and partial-write readback; do not roll
back or report a requested configuration as actual. Configuration errors flow
through existing backend-to-MEL mapping. Frame decisions retain each adapter's
existing receive semantics: Aravis discards rejected buffers, returning timeout
with the last rejection if none is admitted; Pleora releases the native buffer
and returns its existing frame error. Production timestamps remain unqualified.

## Behavioral evidence and limits

`tests/support/alternate_camera_profile.hpp` is a test-only independent profile
and state, composed with the existing representation algorithms. It changes
capabilities, rejects >20 Hz writes/runs and rejects odd frame IDs. It does not
inherit the default profile or merely rename its ID. SDK-free recording tests,
Aravis Fake open/apply/receive/lifetime tests and Pleora actual GenApi plus SDK
copy-stream tests execute the same implementation. Aravis MEL C2 embedding also
observes alternate admission with unchanged provider sources.

Native fixtures are not physical-camera certification. In particular the Pleora
offline tests do not connect a `PvDeviceGEV` or exercise real transport/licensing.
Physical tests remain opt-in, and this test profile is never registered in
production. Live backend switching, reconnect,
navigation transport, new payload representations and qualified camera time are
separate work.

## Optional navigation handler (GIGE-MEL-047)

A profile may optionally interpret, validate and prepare MEL navigation for its
detector. This is an additional interface on the **same concrete state object**,
[`IProfileNavigationHandler`](../src/profiles/navigation_handler.hpp). It is not a
new virtual method on `ICameraProfile` or `ICameraProfileState`, and a profile
that omits it provides no navigation processing (no default handler exists).
Design, mapping and evidence are in the [GIGE-MEL-047 design](tasks/GIGE-MEL-047-design.md)
and [validation](tasks/GIGE-MEL-047-validation.md); the runtime path is in
[navigation forwarding](navigation-backend-forwarding.md#optional-camera-profile-navigation-handler-gige-mel-047).

Contract for `prepare_navigation(const NavigationReport&) const`:

* The common provider has already validated the report structurally and checked
  its timestamp ordering. Apply only your **additional** sensor policy. Return
  `unsupported` for a profile-specific unsupported condition and
  `invalid_argument` for values outside profile policy. Errors are definite;
  nothing has been submitted.
* Return an owned `NavigationOperation`: a nonempty, profile-defined `format`
  label, an **unchanged** copy of the complete report (timestamps and all 18
  covariance values; the bridge rejects an altered copy, compared bit for bit,
  so even turning +0.0 into -0.0 is refused), and your own
  `payload` bytes. There is no implicit coordinate, time or unit conversion. Your
  encoding is yours: there is no universal detector wire format.
* The call must be bounded and must not perform device I/O, block or wait,
  retain the borrowed report, start workers or queues, or call provider or
  consumer code. Frame receive may run concurrently, so use immutable state or
  synchronized instrumentation.
* The operation is a plain value. It may outlive your state and module, and
  must therefore contain no pointers into them.

Execution belongs to the backend, not to the profile. A backend constructs an
`INavigationOperationPort` it owns and a `ProfileNavigationBridge` from
[`navigation_bridge.hpp`](../src/profiles/navigation_bridge.hpp). It exposes
`INavigationSink` on its device **only** when the selected state has a handler.
SDK-free example, test only, from the
[fixture profile](../examples/profile_module/navigation_record_policy.hpp) and
[recording backend](../tests/support/profile_navigation_backend.hpp):

```cpp
namespace p = gige_mel::backend::profiles;

// 1. State: ordinary profile state plus the optional handler.
class State final : public p::ICameraProfileState, public p::IProfileNavigationHandler {
    // ... capabilities/configuration/apply/preflight/validate_frame ...
    gige_mel::backend::Result<p::NavigationOperation> prepare_navigation(
        const gige_mel::backend::NavigationReport& report) const override {
        if (report.state == gige_mel::backend::NavigationSolutionState::aligning)   // 3. own policy
            return gige_mel::backend::Error{gige_mel::backend::ErrorCode::unsupported, "Aligning not accepted"};
        return p::NavigationOperation{"test.fictional-navigation-record.v1", report, encode(report)};
    }
};

// 2. Discovery and 4. bridge construction inside the backend's open().
auto state = profile->open(features);                    // borrowed features: this call only
if (!state) return state.error();
auto shared = std::move(state).value();
if (!p::navigation_handler(*shared))                     // Profile A: no sink at all
    return std::make_shared<PlainDevice>(inner, shared);
auto bridge = p::ProfileNavigationBridge::create(shared, std::make_unique<RecordingPort>(recording));
if (!bridge) return bridge.error();
return std::make_shared<NavigationDevice>(inner, shared, std::move(bridge).value(), recording);

// Device: INavigationSink delegates; close() and ~Device() call bridge->shutdown() first.
gige_mel::backend::Status submit_navigation(gige_mel::backend::NavigationReport report) override {
    return bridge_->submit(report);   // 6. preparation errors are definite; uncertainty is `timeout`
}

// 5. MEL: unchanged public API.
auto manager = gige_mel::make_api_manager(settings, backend);
auto control = ams::iface::irmel::getControl(settings.instance, manager);
control->init();
auto image = /* attach the Image channel */;
auto reply = image->send(report).get();   // success = recording port accepted it, nothing more

// 7. Shutdown: detach / drop Control as usual. The device close drains an active
// submission, retires the port, then releases the bridge's state (and module pin).
```

The port returns `accepted`, `not_submitted` (definite, with a code) or
`uncertain`, plus any control loss. The bridge maps these conservatively: once
uncertainty or loss is reported, it refuses later submissions and never retries
automatically. A recording port's acceptance is fixture acceptance. It does not
mean that a transport acknowledged the report or that a detector received,
applied or used it. No production adapter exposes this sink, and the Cognex
profile has no handler.

**Modules.** Implement the handler on the module's state as above. The host
loader wraps that state in a forwarding subtype only when the module state
provides the capability, and keeps the module mapped until the bridge's state
reference retires. `navigation_handler.hpp` is in the build-identity closure, so
modules built before GIGE-MEL-047 are refused and must be rebuilt with the host.

## Matched-C++ dynamic modules, v1 (GIGE-MEL-035)

See the [independent build example](../examples/profile_module/README.md),
[runtime loading](runtime-selection.md#explicit-module-loading), and
[pre-implementation ownership audit](tasks/GIGE-MEL-035-design.md).

`src/profiles/profile_module.hpp` defines the sole discovery symbol:

```cpp
extern "C" __attribute__((visibility("default")))
const gige_mel::backend::profiles::module::Descriptor*
gige_mel_camera_profile_module_v1() noexcept;
```

The static borrowed descriptor contains uint32 API version (1), exact structure
size, profile-interface version (1), explicit Aravis/Pleora compatibility bits,
bounded NUL-terminated build identity, profile ID, vendor and module version, and
noexcept create/destroy function pointers. It must remain immutable until unload.
Unknown versions, sizes, flags, missing metadata or callbacks are rejected before
creation. The factory writes a project diagnostic into a host-owned buffer and
returns int32 zero with a nonnull owned profile on success, nonzero on error.
The host validates `id()` against the descriptor before registration. Destruction
uses the matching module callback exactly once, including error cleanup.

Use `guarded_create` to catch standard and unknown factory exceptions in-module;
neither discovery nor destruction may throw. Violating a noexcept callback is a
broken trusted module (termination), not a recoverable compatibility mismatch.
C++ `open()` failures preserve project errors; the host wrapper contains unexpected
open exceptions. Ordinary state operations obey the existing Result/Status
contract. No callback may retain borrowed features/guards or start untracked work.

Only discovery has C linkage. Virtual calls, returned std::string/Result/vector
values, shared_ptr state/control blocks and destructors cross the boundary after
validation. They require matched compiler/version, standard library, dual ABI,
debug-STL, compiler ABI flags, sanitizer configuration and interface-header
revision. `cmake/ProfileBuildIdentity.cmake` and its generated header construct
the exact conservative identity (including build configuration/flags hash).
Metadata is not proof of honest compilation; modules are trusted executable code.
Do not claim universal binary compatibility or a future stable C ABI from this.

The host profile wrapper pins a shared RAII dlopen owner and calls the module's
destroy callback before releasing it. Every successful open returns a separate
host state wrapper with its **own** shared module owner. Its plugin shared_ptr,
including control block/deleter, is destroyed before its module reference. Thus
state lifetime does not depend on profile lifetime. Catalog factory closures also
pin the module and create independent profiles on selection. Devices and streams
retain these wrappers using unchanged native ownership contracts. No new
asynchronous callback exists; synchronous operations retain their normal owner.

Last-owner destruction attempts dlclose. There is no force unload, NODELETE,
global registry or intentional leaked handle. Module implementations must avoid
GNU unique exports/NODELETE and untracked threads, callbacks or TLS destructors
that could execute after the final owner. The example hides all symbols except
discovery and disables GNU unique symbols. glibc may retain other external owners;
the loader cannot forcibly override process-wide dynamic-loader references.
Aravis retained native storage and Pleora retained copy bytes contain no plugin
code after stream retirement, so they remain valid even after actual unload.
The native tests observe finalization while those frame bytes are still retained.