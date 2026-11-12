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

This extraction does not implement a generic feature-access service, a second
SDK backend, profile selection or dynamically loaded profile plugins. Aravis
still reads the device's GenICam nodes, builds native snapshots, executes write
plans and normalizes native buffer facts. Those are the next boundaries to
separate and prove with a Pleora implementation.

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

## Responsibilities for the next steps

| Component | Responsibility |
| --- | --- |
| Camera profile | Model/firmware matching, feature aliases, units, dependencies, ordered camera operations, readback interpretation and documented device protocols |
| Shared device services | Typed feature access, current availability/access state, bounds/increments, enumeration entries, commands, explicit readback and structured errors |
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
| 2: typed device services | Move feature/snapshot/write orchestration behind a small common interface, retaining Aravis implementation | Recording-service tests prove required operations, ordering, fresh readback and error distinctions; actual Aravis tests remain passing |
| 3: narrow Pleora backend | Implement that interface and native acquisition for the same initial camera/profile | The same unchanged profile and applicable MEL tests work with both actual SDKs |
| 4: selection and device identity | Select backend and profile independently; match verified physical device identity | Reconnect through either backend without profile or MEL-client edits; incompatible requirements are diagnosed |
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
