# Source-level camera-profile authoring (GIGE-MEL-034)

This is an **internal, uninstalled C++20 interface** for statically linked
implementations. It is not a public ABI, binary plugin SDK or `.so`/`.dll` loader.
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

Each concrete state privately stores its own native snapshot type. There is no
universal monochrome snapshot, `void*`, unchecked downcast or state exchange
between implementations. The default implementation wraps the existing
`mono_free_running` functions rather than duplicating their algorithms. Those
functions retain first-implemented alias rules, checked units/conversions,
dynamic constraints, ROI/timing ordering, partial-write diagnostics, native
readback, Mono8 and little-endian unpacked Mono12 admission.

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
lease may retain state beyond profile/backend destruction. There is no unloading.

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
production. Dynamic loading, ABI versioning, live backend switching, reconnect,
navigation transport, new payload representations and qualified camera time are
separate work.