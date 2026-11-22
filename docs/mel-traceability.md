# MEL Requirements Traceability

[Documentation](index.md) → [MEL Implementation](implementation-profile.md) →
**Requirements Traceability** → [C++ Implementation](../src/provider.cpp) →
[Tests/Evidence](mel-traceability-evidence.md).

Start with the <!-- subpage --> [operation inventory and actionable gap report](mel-traceability-evidence.md).
The [reviewable JSON](traceability/README.md) is the status source of truth;
the website report is generated from it. Use the [implementation profile](implementation-profile.md)
for exact behavior, the [roadmap](mel-surface-roadmap.md) for sequencing and
milestones, and [current qualification status](status.md) for backend and device limits.
Identifiers such as `C2Channel.ModeCmd` are **project-local**, not AMS-GRA requirement IDs.

## Authority and coverage

- [IR MEL `8d9224519f12b44e0b28815755c56a32a28d24a0`](https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/tree/8d9224519f12b44e0b28815755c56a32a28d24a0)
  and [Common MEL `f6908437d8fd2f7fb69896f9eb9cfd272d10c439`](https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-common-mel/tree/f6908437d8fd2f7fb69896f9eb9cfd272d10c439)
  were inspected as full Git trees (106 and 37 tracked paths). The audit baseline
  is `a11071ba2b8a10ee75eb81dc3a3d7715b6a6545e` (PR #53).
- Reconciled all virtual/destructor and delivery declarations on the listed IR
  provider and listener interfaces, the native factory functions and the
  alternate loader/registry seams (including out-of-closure track
  and instrumentation). Common MEL supplies types such as `RequestFor`,
  `VersionInfo`, BIT and navigation/error payloads, not a second channel graph.
  Payload accessors, constructors, copy/move operations, enum helpers, and
  general service utility internals are not independent provider operations.
- The upstream trees supply declarations, not out-of-line `IRFactory`/`IRRegistry`
  or `LibraryLoader` implementation files. `IRCreateFunctions` provides seven
  function-pointer types, not seven concrete export declarations. The proposed
  `createC2Channel`, `createImageChannel`, `createHealthStatusChannel`,
  `createTrackChannel`, `createSchedulingChannel`, `createInstrumentationChannel`
  and `createStackedImageChannel` names are a loader integration decision, not
  verified required symbol declarations at this pin. `IRInterfaceBase` is not a
  base of the declared channel graph. See the [manual reconciliation](traceability/README.md).
- This is an interface audit, not a system requirements or deployment compliance
  audit. It does not resolve ambiguities on behalf of upstream or assert that all
  separately published AMS-GRA requirements are present in these headers.

### How classifications work

`@Required` is a declared obligation; `@RequiredIf…` is conditional on the
named MFA capability; `@Optional` is optional unless the same declaration
contains conflicting normative wording. We retain exact tags and separate them
from the resolved or unresolved obligation and applicability. A class tag governs
the presence of the interface, not every member's predicate. A required method
in a conditionally present class remains required **within that class**. Methods
inherited from `Channel` have one declaration each; their behavior across attached
families is recorded on those rows. Listener implementations are supplied by the
service; the provider owes correct invocation, buffer handoff and teardown.

Examples requiring explicit decisions:

- **Scheduling:** class tag is `@RequiredIfIRSTTrackTasking`; `exec` has separate
  scan/stare/track predicates, and `AbortTask` carries all three. Resolve whether
  scan-/stare-only profiles must supply the channel. `PrioritizedTrackListCmd`
  is `@Required` in the class, and `UpdateTrackListResponse` uses `@RequiredIfTrack`
  with ambiguous wording. The `ChannelCapability` comment says every EOIR MFA
  schedules. Depth zero means replacement by an immediate task, **not** no tasking.
- **Track:** tracking, accepting track updates, and detecting candidate objects
  are separate predicates. Candidate detections have independent Image and Track
  overloads; confirm the channel contract for a detector without tracking.
- **Stack:** `StackedImageChannel` is class-annotated `@Required`;
  `StackedImageListener` and `onImageStack` are `@RequiredIfImageStack`. Current
  source-gated attachment is not a waiver of the class annotation. Its inherited
  ordinary Image override is an upstream no-op, not another image delivery route.
- **Commands and buffer registration:** several `@Optional` C2/Track command
  annotations say “must be provided … for all IR MEL implementations”; preserve
  them as unresolved. `Channel` buffer methods are required but describe frame
  storage; Health rejects them and C2 cannot register. Confirm non-frame scope
  rather than marking all inherited behaviors complete.

An override establishes **interface completeness** for a concrete C++ class,
not behavior. A ready future or a `Rejected` CommandStatus does not show
successful execution. **Supported** means the named behavior is implemented and
positively evidenced within its stated profile; **partial** means a subset,
source/semantic boundary or evidence gap remains. **Unsupported stub** rejects
on an attachable interface; **unavailable** has no usable route; **blocked**
has a known upstream/interface or source blocker; **unresolved** needs a decision
before assigning a behavior status. Production qualification is separate from
all these labels: even exact header/pixel assertions with fixture clocks do not
qualify native integration-start timing, optics, transport reliability or a
different device/firmware.

## Capability advertisement audit

[`ControlImpl::init`](../src/provider/control.cpp) and
[`capability`](../src/provider/capabilities.cpp) build the immutable
`Control::getCapabilities()` discovery snapshot at first successful init.
Failed discovery construction leaves that vector unchanged; a retry can finish
discovery without reopening the initialized device. This is covered by the
[SDK-free discovery regression](qualification-evidence.md#sdk-free-provider-and-profiles)
and does not change which channels or metadata are advertised.
[`ChannelCore::getCapabilities()`](../src/provider/channel_core.cpp) recomputes current attached configuration from
the same helper. Metadata flags describe registration/delivery, not command
support. Attach requires an advertised identity/type and the proper listener on
frame channels. See the [pinned metadata enumeration](https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/ChannelMetadataCapabilityType.h)
and [capability contract](https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/ChannelCapability.h).

| Channel | Metadata in both capability views (in addition to common `ChannelCommsTestRep`) | Boundary or discrepancy |
| --- | --- | --- |
| `IRSTImage` | `CameraCommandResp`, `NavigationReportResp`; `BadPixelList` if explicitly known empty; `LineOfSightEuler` and `LineOfSightReport` independently from valid source declarations | Camera response is on Image even for C2-origin commands. Navigation advertises acknowledgement, not sensor acceptance. Three required metadata registrations are not generally available: defect map is synthetic-only among shipped backends, LOS Euler/Report have a recording source only. No fallback from navigation or optics. |
| `CommandAndControl` | `CommandStatus`, `BITConfiguration`, `BITStatus` | Discovery also contains camera layout facts, but C2 is not a frame channel. Status includes typed rejections of five unsupported commands; BIT is software only. |
| `HealthAndStatus` | `MFAStatus`, `MFAStatusDetailed`, `BITStatus`, `SubsystemStatusResp` | No image geometry; software observations, not physical health, reachability or hardware BIT. Required `DiscreteStatus` and `MFA_SecurityAuditRecord` cannot be represented in the pinned enum and remain `NotSupported`; audit events also lack a genuine source. |
| `StackedImage` | None beyond communications | Discovery/attachment only with a valid pre-formed volume declaration; in-tree recording fixture only. Optional LOS3D is not delivered. |
| `Scheduling` | `ExecuteTaskAck` for the qualified recording fixture | Admission only, not completion. Depth zero means one active task with Immediate replacement and no queue on this channel; a default zero does not advertise tasking. [Virtual stare profile](task-scheduling-provider.md). |
| Track, Instrumentation | No channel, no advertisement | Their conditional product and reporting contracts remain unimplemented. |

`DiscreteStatus`, `MFA_SecurityAuditRecord`, C2 `CalibrationConfiguration`/
`CalibrationStatus`, and Image/Health `NUC_TempData` have no exact metadata
enumerators at this pin. Do not repurpose `Reserved*` slots. Other unimplemented
metadata with enum values are simply absent; the default ODM/NUC availability
flags are false and no navigation-frame declarations are supplied. For a new
metadata type, resolve enum, registration, product availability and tests together.

Image width/height are profile/device geometry; Mono is one band. Mono8 is
8 significant/storage bits, pitch = width; unpacked little-endian Mono12 is
12 significant bits stored in 16 bits, pitch = twice width. Both report packed
destination size in bytes, not native padding. Stack reports plane width/height,
Mono8 pitch = width and total packed **volume** size; depth is in `StackFrameHeader`,
not a schedule depth or band count. Attached Image geometry can differ from the
discovery snapshot after successful reconfiguration. The timestamp gate still
applies after advertisement; a discovered channel is not proof of a publishable
hardware frame. See [Mono12](mono12-image-profile.md), [stacks](stacked-image-provider.md)
and [LOS](image-los-metadata.md).

## Detector integration responsibility matrix

| Concern | Primary responsibility | Integration contract and remaining work |
| --- | --- | --- |
| MEL object lifecycle, callbacks and teardown | Common provider | Manager/Control attachment, operation serialization, callback admission/drain and bounded teardown; service owns listener lifetime contract and must not unload a library while objects remain. |
| MEL host buffers and publication | Common provider + service | Provider wraps, checks, copies and publishes; service allocates storage and explicitly releases it. No direct registered-storage acquisition. |
| Native Ethernet/SDK transport and cancellation | Backend adapter | Exact device selection, ownership, bounded commands, typed failures, acquire/stop and `FrameLease`; no vendor types across the public boundary. |
| Camera model/firmware admission | Camera profile | Profile admission on exact open, not inferred from discovery or a successful alternate model. |
| Feature names, units and configuration rules | Camera profile + backend feature service | Translate native aliases, ranges/grids and units; read back actual state and configuration generation. |
| Exposure and native acquisition | Detector/backend | Supply truthful exposure, pixel facts, complete frames and lease validity; provider does not fabricate acquisition. |
| `FrameTimestamp` mapping and validity | Qualified backend/device time integration | Prove physical integration-start event, sign/units/epoch/domain, generation and monotonicity. Virtual fixtures do not qualify hardware time. |
| `NavigationReport` acceptance | Sensor transport + backend sink | Provide a typed `INavigationSink`, distinguish receipt, rejection and uncertainty; separate association/processing and timing qualification. |
| LOS, defect maps, NUC and stack products | Actual sensor/product source + provider mapping | Deliver observations, calibration and provenance; provider maps only implemented products and gates advertisement. |
| Health/BIT and security | Detector/backend + external event owner | Supply real status, hardware BIT results and genuine audit events; agree enum/interface changes with upstream. |
| Task execution/tracking | Mission-specific sensor + provider | The recording fixture exercises virtual admission only. Agree predicates and task state machine with the target service before physical execution; no tracker or candidate producer is supplied by the common provider. |

A custom Ethernet detector can implement project-owned
[`ICameraBackend`, `ICameraDevice` and `IFrameStream`](../include/gige_mel/backend.hpp)
and optional [navigation](../include/gige_mel/navigation.hpp) and
[LOS](../include/gige_mel/line_of_sight.hpp) sources, then compose via
[`make_api_manager`](../include/gige_mel/provider.hpp). Translate the detector's
protocol, timestamps, errors and cancellation in that adapter; keep vendor
headers, handles and protocol policy out of MEL and shared profile code. Use the
[backend authoring contract](backend-authoring.md), [profile authoring contract](camera-profile-authoring.md)
and [runtime selection](runtime-selection.md). The detector vendor need not
implement the upstream MEL C++ API, but must supply real device behavior and
data before the provider can truthfully expose sensor-dependent capabilities.

## C++ Implementation and evidence

Read [factory exports](../src/factories.cpp), [manager/session construction](../src/provider.cpp),
the [private provider component map](architecture.md#private-provider-source-organization)
and [installed backend/provider interfaces](../include/gige_mel/provider.hpp).
Then use the [operation-level tests and gap report](mel-traceability-evidence.md)
to cross-check each claim against its exact assertion and environment. See
[validation history](validation.md) and [current backend evidence](status.md)
for historical native SDK, simulated transport and limited physical observations.
No new hardware or optional SDK qualification is implied by this inventory.
