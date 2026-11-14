# Camera profiles across SDK backends

## Goal and implemented boundary

The goal is one camera profile that works unchanged with each backend that
implements its required device operations. Camera semantics belong to the
profile; SDK calls, native streams and buffer ownership belong to the backend.
The common MEL provider keeps its channel, callback, buffer and lifecycle rules.

GIGE-MEL-024 establishes the first part of that boundary. The existing
SDK-free monochrome/free-running profile is compiled in the internal
`gige_mel_camera_profiles` target from
[`src/profiles/mono_free_running.hpp`](../src/profiles/mono_free_running.hpp)
and its implementation. The Aravis adapter consumes that target. Profile-only
conversion, constraint, ordering, preflight and frame-validation tests run in
the default `camera_profile_contract` test without Aravis, cameras or networking.

GIGE-MEL-025 adds the internal typed feature service in
[`src/device_features.hpp`](../src/device_features.hpp). The shared profile now
selects feature aliases, builds snapshots, executes configuration write plans
and obtains truthful final readback through that service. The orchestration is
in [`mono_free_running_device.hpp`](../src/profiles/mono_free_running_device.hpp)
and its implementation. A recording service exercises these operations in the
default `device_feature_contract` test without an SDK.

Aravis implements the actual node operations, type/access validation and native
error translation. It retains discovery, device ownership, acquisition-command
uncertainty, streams, buffers and normalization of received-frame facts. A
second SDK backend is now implemented by the optional [Pleora adapter](pleora-integration.md),
but licensed camera-control and transport qualification remain pending.
GIGE-MEL-033 adds [explicit backend/profile binding](runtime-selection.md) for the
single canonical `mono_free_running_v1` profile. Multiple independent profiles
and dynamically loaded profile plugins remain subsequent work. The shared profile
sources are unchanged by that adapter and by runtime selection.

## Preserved behavior

The profile's checked conversions, accepted formats, units, preconditions,
write ordering, readback rules, diagnostics and frame admission remain the same.
The historical diagnostic profile identifier `aravis-mono8-free-running-v1`
is retained for compatibility; its name does not establish a dependency on
Aravis or a registry of selectable camera profiles.

Aravis acquisition resource defaults remain four source buffers, 64 MiB per
buffer and 256 MiB per run. These are adapter policy, declared in
[`acquisition_policy.hpp`](../src/backends/aravis/acquisition_policy.hpp),
and passed explicitly to the common checked pool planner. They are not fixed
requirements for every camera profile or future backend.

The new target and profile headers are internal and are not installed or
exported. No MEL declaration, factory selection, advertised capability,
navigation transport, timestamp mapping or binary plugin ABI is added.
Existing real-Aravis integration tests remain enabled in the optional build.
Relocating a pure test into the default suite does not replace native SDK
control, acquisition, lease, cancellation or teardown tests.

## Typed feature-service contract

`IDeviceFeatures` returns project-owned integers, floats, strings, Booleans and
numeric feature snapshots. Numeric snapshots include current values, bounds,
increments and optional float units. Numeric snapshot reads declare whether the
caller needs readable or read/write access. Typed value and numeric-snapshot
reads validate current implementation, availability, access and type before
reading a node. The two setters handle integer and float features and enforce
current native access and bounds at each write.

`implemented(name)` deliberately answers only existence and implementation. A
successful false result means confirmed absence or a successfully queried
unimplemented node. Unavailable or incorrectly typed implemented nodes remain
errors when read; an implementation-query error is never absence. Alias
selection uses the first implemented name, even when that selected feature
subsequently fails its type, access or value checks. It does not fall through
to a more convenient alias. Enumeration-entry availability is a separate query
from reading its current string value.

All returned values own their data. SDK-specific sentinel normalization stays
inside the adapter; for example, the pinned Aravis undefined-increment sentinel
becomes an empty optional, while explicit invalid increments remain facts for
the profile to reject. The interface exposes the operations the current
configuration profile uses. Enumeration/string/Boolean setters, generic node
trees, register blocks and command execution can be added with a concrete
shared use case; they are not part of this initial service.

The caller holds its device operation lock and native lifetime across the
entire snapshot or configuration transaction. Feature calls do not establish a
transaction by individually locking. Shared orchestration accepts explicit
synchronous `ConnectionCheck` and `BeforeWrite` guards, borrows them only for
the call and never retains them. These are internal adapter guards, not MEL
consumer callbacks, and must not reenter the provider. Aravis uses them to keep
its control-loss checks before/after readback and around its native write hook.
A before-write guard failure returns directly; only a failure returned by a
typed setter enters the successful-prefix and actual-state diagnostic path.

Each apply obtains a fresh snapshot before planning all writes. Numeric setters
must honor bounds and access after preceding writes; the initial snapshot is
not permanent authorization. Successful writes require final actual readback.
A failed write may already have affected the device: the original error stays
primary, successful earlier writes and fresh readback are reported, and there
is no automatic retry or rollback. The current profile does not change
selectors, trigger mode, automatic exposure or pixel format.

## Responsibilities for the next steps

| Component | Responsibility |
| --- | --- |
| Camera profile | Model/firmware matching, feature aliases, units, dependencies, ordered camera operations, readback interpretation and documented device protocols |
| Shared device services | Typed reads, current access validation, bounds/increments/units, selected enumeration-entry availability, integer/float writes and structured errors |
| SDK backend | Discovery/open, actual SDK operations, native streams and buffers, waits/cancellation, transport configuration and normalized received-frame facts |
| Common MEL provider | MEL semantics, runtime serialization, callbacks, capabilities, destination-buffer ownership and publication |
| Deployment data | Selected physical camera/backend, lens and installation information, per-device calibration and qualified clock information |

Shared services must use project-owned values. `ArvDevice`, `PvGenParameter`,
native node-map objects and SDK exceptions stay inside their adapters. An
operation's feature name does not establish that it is implemented, currently
available or writable. Failed queries must not become successful optional
absence. Selector changes and ordered writes require fresh dependent facts.

The same profile should specify the same camera operations through either SDK.
Host socket sizes, native queue policy and receiver-specific packet recovery
remain backend settings. Backend selection can change performance and therefore
needs its own qualification; it must not silently change camera-command units
or acceptance semantics.

## Development sequence and acceptance

| Step | Work | Acceptance evidence |
| --- | --- | --- |
| 1: shared profile (GIGE-MEL-024) | Extract existing SDK-free policy and run its pure tests by default | Existing behavior preserved; default and optional-adapter suites pass; installed package remains independent |
| 2: typed device services (GIGE-MEL-025) | Shared snapshot/configuration orchestration over an internal typed feature interface, with an Aravis implementation | Recording-service tests prove required operations, ordering, fresh readback and error distinctions; actual Aravis tests remain passing |
| 3: narrow Pleora backend | Implement that interface and native acquisition for the same initial camera/profile | The same unchanged profile and applicable MEL tests work with both actual SDKs |
| 4: selection and device identity (GIGE-MEL-033 foundation) | Explicit built-backend, exact device ID and canonical profile binding; no implicit open | Offline selector/native-fixture contracts; physical identity equivalence and cross-SDK hardware qualification remain pending |
| 5: sensor capabilities | Add navigation wire protocol, additional payloads or other requested operations individually | Shared semantic tests plus device/SDK evidence for each advertised capability |
| 6: optional binary plugins | Define loading, versioning, ownership and compatibility if independent distribution is needed | Profile lifetime, ABI mismatch and unload behavior are validated separately |

Start with the same physical camera and profile through both SDKs. Compare
configuration requests and actual readback, invalid requests, pixel layout,
stream cancellation, retained data, close and restart. A recording fake proves
profile logic; it does not qualify a second SDK or physical camera.

Supported capabilities depend on the selected camera/firmware, profile, backend
and implemented MEL surface. Optional features can vary by combination; a
required unavailable feature must fail explicitly. A new payload representation
or MEL operation may require one common framework extension before a profile
can map it to a camera.

Initial backend switching is an orderly stop/close/reopen operation. Switching
under live native leases or uncertain in-flight commands is separate work.
Existing source-level C++ interfaces do not promise independent binary plugin
compatibility; any such ABI remains a separate architecture decision.

## Navigation integration

The optional [navigation sink](navigation-backend-forwarding.md) is the semantic
entry point for a future composed camera device. A camera profile may define
the sensor's field encoding, register/feature mapping, staging, commit and
acknowledgement sequence once shared services support the needed operations.
The SDK backend performs those operations without owning the sensor protocol.

A completed transport write does not establish complete-message acceptance or
sensor processing. The device protocol must define that boundary, including
uncertain outcomes and the effect of explicit retries. This extraction supplies
no such protocol and does not populate frame navigation or qualify clocks.
