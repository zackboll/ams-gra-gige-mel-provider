# MEL surface roadmap and implementation history

Use [Requirements Traceability](mel-traceability.md) for the authoritative
operation-by-operation status and evidence. This roadmap keeps milestone history
and sequencing; see [current status](status.md) for audited backend boundaries
and [Qualification Evidence](qualification-evidence.md) for original runs.

Optional profile Image FOV changes the source of two existing `FrameHeader`
fields, not the MEL surface. The provider freezes a selected model's owned
radians for the actual run configuration. Devices without the source retain the
linear session-pitch model. The positive optics are virtual and fixture-only;
see the [Image profile](implementation-profile.md#frozen-image-field-of-view).

GIGE-MEL-GENTL-003A adds isolated simulated-camera wire qualification tooling,
**no MEL surface** ([design](tasks/GIGE-MEL-GENTL-003A-design.md)). Independent
observation now detects writes, privilege changes, unknown/repeated commands and
post-close traffic; offline harness self-tests do not qualify a real producer.
At GENTL-003A, access to Euresys still required an authenticated download and
Balluff's earlier crash remained unresolved. See the [producer evidence matrix](qualification-evidence.md#gentl-producers-and-consumer)
for the audited records. Physical-camera authorization and GENTL-004 are separate gates.

GIGE-MEL-GENTL-003 adds only bounded offline qualification tooling
([evidence](tasks/GIGE-MEL-GENTL-003-validation.md)), **no MEL surface**.
Balluff 3.7.2 initializes but crashes in isolated interface update; Euresys
26.10 requires authenticated SDK access. Independent read-only safety and
producer-specific physical authorization remain prerequisites. No Cognex
GenTL inspection or camera compatibility was established. Resolve those
qualification blockers before separately authorized GENTL-004 streaming.

GIGE-MEL-GENTL-002 adds an internal, default-OFF GenTL backend foundation
([design](tasks/GIGE-MEL-GENTL-002-design.md)). It is read-only: enumerate,
exact open/close, Aravis 0.8.36 GenApi and unchanged profile admission/readback.
It adds **no MEL surface**, no acquisition, no selection-catalog entry and no
qualified camera. The remaining milestones, in order:

1. GIGE-MEL-GENTL-003: producer and Cognex hardware qualification. First a
   read-only open/inspect/close, then a separately authorized Mono8 acquisition task.
2. GENTL-004: streaming.
3. GENTL-005: explicit selection opt-in.

GIGE-MEL-042 adds no MEL capability. The five unsupported C2 commands
(`CalibrationConfigurationCmd`, `CalibrationStatusCmd`, `CameraProtectCmd`,
`EraseCommand` and `SystemTrackDataResponse`) still return a ready typed
`Unsupported` future. Once admitted, they now also publish one correlated
`Rejected`/`CapabilityUnavailable` `CommandStatus`. This is a
provider-defined consistency enhancement; upstream does not require it. Their
capabilities remain unimplemented and unadvertised, and the GIGE-MEL-041 Health
metadata blockers are unchanged. See the [design](tasks/GIGE-MEL-042-design.md).

GIGE-MEL-GENTL-001 evaluates a generic GenTL consumer backend
([design](tasks/GIGE-MEL-GENTL-001-design.md)). It adds no MEL surface or
backend. The recommended next step is GIGE-MEL-GENTL-002 (an internal backend
foundation, not selectable by default), conditional on a qualified producer and
a GenApi decision.

GIGE-MEL-041 adds no MEL capability. It classifies the required Health
`DiscreteStatus` and `MFA_SecurityAuditRecord` registrations as blocked by the
pinned capability enumeration (and, for security audit, by a missing provider
event source). Both remain NotSupported and unadvertised, under executable
classification tests. `HealthStatusChannel` remains partial. See the
[design](tasks/GIGE-MEL-041-design.md).

GIGE-MEL-047 adds an internal, optional camera-profile
[navigation handler](navigation-backend-forwarding.md#optional-camera-profile-navigation-handler-gige-mel-047)
and a reusable bridge to the existing navigation sink, preserved through module
loading. The MEL surface, advertisement and provider dispatch are unchanged.
Only an SDK-free recording backend is wired. There is no production adapter,
transport, detector protocol or authority grant
(`PROFILE_NAVIGATION_FIXTURE_QUALIFIED`; [design](tasks/GIGE-MEL-047-design.md)).

GIGE-MEL-039 adds a [loopback-only single-owner authority](gvcp-single-owner.md),
not a MEL capability. Shared control/heartbeat and retirement evidence does not
qualify physical ownership, SDK integration, stale-session fencing or navigation.
R1 preserves unresolved ownership on negative device ACKs; it adds no production
capability or wire-protocol change.

GIGE-MEL-038 adds an independent [controlled GVCP feasibility artifact](controlled-gvcp-transport.md),
restricted in code to loopback. Packet-level deadline/cancellation evidence does
not add a MEL capability or production navigation path. SDK camera ownership and
sensor protocol/acceptance remain explicit next-integration blockers.

GIGE-MEL-037 adds only an internal [register/memory transport](device-register-transport.md).
Navigation wire encoding, sensor acknowledgement and MEL submission integration
remain unimplemented; bounded execution and SDK-internal retries remain open.

## Cognex profile (GIGE-MEL-036)

The first real-camera-specific [independent module](../camera_profiles/cognex_cic_5000r/README.md)
adds exact CIC-5000R firmware identity, Mono8-only and conservative discrete-rate
admission through both existing SDKs. This is internal camera policy, not a new
MEL capability or factory. Production timestamp, licensed hardware qualification,
Mono12 native byte order and additional firmware compatibility remain pending.

## Explicit matched-C++ profile modules (GIGE-MEL-035)

An opt-in Linux x86-64 loader now validates separately compiled camera-profile
shared libraries through a versioned C-linkage discovery entry point, then uses
the existing C++ profile interface under an explicitly matched compiler/stdlib/
ABI/interface contract. This is not a stable cross-toolchain C ABI or a runtime
backend plugin API. The installed common provider and exported MEL factories
remain independent and unchanged. No new MEL capability or camera/clock
qualification is claimed.

Module profiles and independently surviving states each retain a host-owned
shared RAII module pin. Plugin objects and shared_ptr deleters retire before the
last pin attempts dlclose. Existing native stream/lease contracts are unchanged;
retired native/copy frame storage remains valid without plugin code. There is no
force unload, hot replacement, implicit search/discovery or fallback. See
[profile authoring](camera-profile-authoring.md),
[runtime loading](runtime-selection.md#explicit-module-loading), and the
[pre-implementation boundary audit](tasks/GIGE-MEL-035-design.md).


GIGE-MEL-034 introduces an internal, SDK-independent source-level camera-profile
interface with owned per-device policy state and explicit injection into Aravis
and Pleora. Selection now executes independently registered implementations,
not just profile IDs. The default monochrome algorithms and historical diagnostic
ID are preserved. No MEL capability, channel, factory, buffer ownership or
production timestamp qualification changes. See [profile authoring and lifetime](camera-profile-authoring.md)
and the [pre-implementation dependency audit](tasks/GIGE-MEL-034-design.md).

## Internal selection (GIGE-MEL-033)

An explicitly owned catalog now selects built `synthetic`, `aravis` and `pleora`
backends with exact device identity and the default `mono_free_running_v1` binding.
This is [internal composition](runtime-selection.md), not a new MEL surface,
factory name or stable binary ABI. The common provider remains unchanged. Source-level profile dispatch is now
implemented by GIGE-MEL-034; matched-toolchain loading by GIGE-MEL-035. Stable binary ABI,
cross-SDK physical-camera ownership qualification and production clocks remain
future work.

## Optional Pleora backend (GIGE-MEL-025 integration)

GIGE-MEL-027 selects PR #26 as the single canonical implementation and adds
native feature and test-clock-only MEL coverage from duplicate PR #25. It adds
no MEL capability or production time qualification. See the
[source/contract comparison](pleora-consolidation.md).

An internal, default-OFF eBUS adapter now implements the shared typed feature
service and native acquisition with bounded copy leases, without changing the
shared profile or MEL provider. [Integration and qualification record](pleora-integration.md)
distinguishes SDK-buffer tests from real watermarked Cognex Mono8 evaluation.
Receiver evaluation is explicitly selected, default-off and not production-qualified;
the SDK remains authoritative and development entitlement remains unresolved.
P3 is **not complete**: licensed control/acquisition qualification and qualified
camera timestamps remain pending. Sequential Aravis/Cognex evaluation results
are recorded, not a production benchmark. No
navigation transport, backend factory selection or new MEL capability is added.

## Typed device services (GIGE-MEL-025)

The shared monochrome profile now selects aliases, builds fresh snapshots,
executes ordered configuration writes and reads actual state through an
internal SDK-free typed feature interface. Aravis implements native access;
recording-service tests cover orchestration without an SDK. Device locking,
control loss, acquisition-command uncertainty and stream ownership remain
adapter responsibilities. The optional Pleora implementation above consumes
the same service and acquisition contracts; production use remains unqualified. See
[camera-profile portability](camera-profile-portability.md).

## Shared camera-profile foundation (GIGE-MEL-024)

The existing monochrome/free-running profile is an internal SDK-free module,
consumed by Aravis and a default profile contract test. Host pool policy is
separate and passed explicitly. This is a behavior-preserving extraction with
no new MEL capability or production backend. GIGE-MEL-025 adds typed device
services; the optional Pleora implementation above still needs qualification.
Internal backend/profile binding and source-level profile dispatch are implemented
above; stable cross-toolchain binary plugins remain subsequent work; see
[the sequence and acceptance criteria](camera-profile-portability.md).

## Backend-independent NavigationReport forwarding (GIGE-MEL-023)

The existing Image navigation receipt path optionally submits every validated
field to a backend navigation sink, with explicit acceptance and typed failures.
Devices without a sink remain receipt-only. The MEL surface and advertisement
are unchanged. [Implemented behavior and tests](navigation-backend-forwarding.md)
do not establish navigation accuracy, clock synchronization, image correlation
or sensor-side processing completion. No production navigation transport exists.

## Unpacked Mono12 (GIGE-MEL-022)

The existing Image capability/header/buffer surface maps explicitly selected
Mono12 to 12 significant bits, 16 storage bits, MEL Mono, one band and packed
two-byte rows. No new public MEL method, format command, C ABI or plugin ABI.
[The profile](mono12-image-profile.md) defines byte-order admission, arithmetic,
lease ownership and simulated evidence. Mono12Packed, Bayer/RGB, physical Cognex
Mono12, production Aravis timestamps and full MEL conformance remain open or
unsupported; these are not implied by successful image delivery.

## How to read this plan

The API in scope is **AMS-GRA IR MEL** and its Common MEL dependencies.
The original P0–P5 order below is planning history, not a deployment compliance
profile or a list of operations currently supported. Resolve applicable
requirements and sensor/adapter qualification before claiming support.

## Priority order

| Priority | Deliverable | Why it comes here | Exit criterion |
| --- | --- | --- | --- |
| P0 — bootstrap | Native factories, identity, Control, C2/Image lifecycle, registered host buffers, Mono8 frames, camera commands, synthetic backend, ownership tests | Establish the real provider boundary and memory contract before integrating hardware | A native client loads the library through upstream signatures, receives deterministic frames, holds/releases buffers safely, stops, reconfigures, and restarts |
| P1 — operational MEL profile | Required C2 commands/metadata, required Image metadata/navigation, Health channel, and decisions on stack/task semantics | These are conformance gaps even when a camera image already appears | Each claimed required behavior has a real source, semantics, callback/error behavior, and positive/negative tests; unresolved annotation/profile issues are recorded |
| P2 — first real camera | One selected SDK adapter, one explicit camera profile, device selection, connection loss, packet/payload handling, time mapping | Validates the shared model against actual transport behavior | Hardware qualification demonstrates native MEL image/control/lifetime behavior with a pinned SDK and camera/firmware |
| P3 — second implementation | A second narrow SDK adapter with the same initial Mono8/control profile | Proves the architecture is reusable before deeper SDK-specific optimization | The MEL facade and client remain unchanged; the reusable backend contract suite passes for both adapters |
| P4 — requested capabilities | Additional formats, direct registered-memory acquisition, calibration, protection, richer navigation, and instrumentation | Add only capabilities needed by the selected sensor and Skill | Capability advertisement, command semantics, metadata, and tests expand together |
| P5 — specialized products | IRST tasking/tracking, candidate detection, TSI image stacks, advanced bands/scan modes | These require sensor/product semantics beyond acquiring GigE frames | Sensor, coordinate, timing, scheduling, and data contracts are defined and qualified end to end |

**Historical P2 observations (through GIGE-MEL-018).** An optional, internal Aravis 0.8.36 camera-control foundation (discovery, exact selection, ownership, capabilities, configuration, truthful readback) is implemented and exercised through the real SDK's Fake device ([Aravis integration](aravis-integration.md)). GIGE-MEL-009 adds bounded Mono8 source acquisition (SDK buffers, leases, cancellation, direct capture), also exercised only through the Fake device; its frames carry an invalid timestamp, so the common provider rejects them and no Aravis MEL Image is published. GIGE-MEL-010 has three parts. It adds **read-only observation** of the raw SDK timestamps (adapter path and `time-probe` diagnostic). It records their source-backed provenance: on the pinned Fake both are host wall-clock fill time, and on GigE the value is a leader tick conversion with a host-time fallback. It also **test-qualifies** positive MEL Image publication of genuine Aravis pixels, but only under a **test-only virtual integration-start clock**. That is not timestamp qualification. A real Aravis integration-start timestamp is **not qualified**, and production Aravis-backed MEL Image publication is **not enabled**. The physical-camera timing gate stays open until every item of the [camera-timing qualification checklist](aravis-integration.md#camera-timing-qualification-checklist) is established for a selected camera. GIGE-MEL-011 exercises the unchanged production GigE backend over **simulated** GVCP/GVSP transport: the pinned SDK's `ArvGvFakeCamera` on IPv4 loopback (real discovery, control, UDP image transfer, cancellation, diagnostics, simulated packet loss, and virtual-clock MEL publication of network pixels). It qualifies no physical camera, NIC or timing. Its client traffic is loopback-only, but the pinned simulator also binds a global GVCP discovery listener to 255.255.255.255:3956, which is effectively wildcard on Linux. The simulator process is therefore not strictly confined to loopback. The test runs under an explicitly documented test-only exception (GIGE-MEL-011-R1), not under network isolation, and owner acceptance is pending. GIGE-MEL-018 adds the first physical-camera evidence: a Cognex CIC-5000R was discovered, inspected and captured (Mono8) through the unchanged GigE backend, and its genuine pixels reached MEL Image callbacks, with Standby, held-buffer release and restart, in an opt-in test that uses only the test-only virtual clock. Two narrow adapter compatibility fixes were needed (legacy `AcquisitionFrameRateAbs`, Integer identity nodes). The camera's power-on configuration is unsupported, and the tests used a documented temporary arrangement ([record](cognex-cic-5000r.md)). That milestone recorded limited physical pixels with virtual time, not production qualification.

| P2 milestone | Status |
| --- | --- |
| Aravis camera control and truthful readback | Implemented, Fake-tested (GIGE-MEL-008) |
| Native Mono8 source acquisition | Implemented, Fake-tested (GIGE-MEL-009) |
| Raw SDK timestamp observation | Implemented (GIGE-MEL-010) |
| Aravis-pixel MEL Image publication under virtual test time | Test-qualified only (GIGE-MEL-010) |
| Real Aravis integration-start timestamp | **Not qualified**; checklist gate open |
| Production Aravis-backed MEL Image publication | **Not enabled** |
| Simulated GigE transport (loopback `ArvGvFakeCamera`: discovery, GVCP, GVSP) | Exercised (GIGE-MEL-011); simulator only |
| Confirmed Aravis camera-control loss (`ArvDevice::control-lost`) | Terminal backend failure, blocked configuration/start, safe leases/close and explicit reopen exercised with real simulator privilege revocation (GIGE-MEL-012); existing MEL stream-failure path, no new MEL surface; **not physical unplug detection** |
| Network isolation of the loopback test | **Not established**: the simulator's wildcard 255.255.255.255:3956 discovery listener is a documented test-only exception (GIGE-MEL-011-R1); owner acceptance pending |
| Physical camera / NIC | Limited Cognex Mono8 observation; no production timing or NIC qualification in this milestone |

**Current P2/P3 disposition.** Aravis has physical Cognex observations and Pleora
exists as the second internal SDK adapter, with native fixtures and limited
watermarked Cognex evaluation. Neither record qualifies production integration-
start timestamps, and licensed production Pleora use is still gated. See
[current status](status.md) and the [Pleora qualification record](pleora-integration.md).

P5 is not permission to ignore a required annotation. Resolve the deployment
profile first: [operations and gaps](mel-traceability-evidence.md) identify what
is absent or only partial across factories, Channel/Buffer, C2, Image, Health,
Scheduling, Track, Instrumentation and Stack. The [implementation profile](implementation-profile.md)
retains ownership, memory, command and callback details.

## P1 profile decision / P5 implementation: SchedulingChannel

GIGE-MEL-045 implements a conditional, recording-qualified virtual-stare subset:
an owned TaskSched session, Immediate-only no-queue replacement, current abort
and ExecuteTaskAck. TaskExecutingRep stays unsupported because the pinned Task
enum lacks an unambiguous stare mapping. AbortTask has no public TaskInfo
accessor, so targeted abort cannot be claimed. The [task profile](task-scheduling-provider.md)
defines time, coordinates, capacity-zero and lifecycle contracts. The
[Scheduling operation inventory](traceability/scheduling.json) records remaining
required and conditional gaps. Default and production backends remain unsupported;
this is not complete scheduling conformance.

## P1 profile decision / P5 implementation: stacked imagery

[StackedImageChannel][stacked] is class-annotated **Required**, while [StackedImageListener][stacked-listener] and its `onImageStack` operation are conditional on image-stack support. The channel's additional `LOS3D_KinematicsType` callback is optional. Record and resolve that distinction for the target profile; do not describe the whole channel as unconditionally optional.

GIGE-MEL-044 implements this channel through the existing Control pathway only
for explicitly qualified stack sources. The [stack profile](stacked-image-provider.md)
defines the inherited Channel surface, dynamic listener admission, immutable
capabilities, header mapping and owned buffer delivery through:

```cpp
Return StackedImageListener::onImageStack(
    Channel& channel,
    const StackFrameHeader& header,
    std::shared_ptr<Buffer> buffer);
```

A set of consecutive frames or a GigE multipart payload is not automatically a valid MEL image stack. Define the stack's spatial/temporal/band semantics, source association, layout, and lifetime before mapping it.

The implemented test-only recording supplies pre-formed Mono8 two-layer test
volumes, not camera frame concatenation or qualified production TSI. Ordinary
synthetic and all production SDK adapters remain unsupported/unadvertised.
Callback non-success stops acquisition without releasing delivered memory.
Physical TSI meaning, sensor and timing qualification remain open; see
[validation](tasks/GIGE-MEL-044-validation.md). LOS3D and alternative registry
factories remain unsupported. Class presence alone is not full conformance.

## Cross-cutting obligations

### Capability accuracy

`ChannelCapability` covers identity, image width/height, signal bit depth, byte pitch, buffer/image sizes, bands/format, sensor types, platform/location, channel types, task queue depth, distortion/NUC availability, metadata, band information, and navigation frames. Populate it from the selected source and implemented profile; do not blindly publish SDK feature discovery.

The pinned [ChannelMetadataCapabilityType][metadata-enum] lacks entries corresponding to `CalibrationConfiguration`, `CalibrationStatus`, `NUC_TempData`, `DiscreteStatus`, and `MFA_SecurityAuditRecord`, despite declarations of those callbacks. **Do not repurpose reserved enum values.** A conformance/deployment decision or upstream resolution is needed when those surfaces are implemented, and tests must check both registration and advertisement behavior.

GIGE-MEL-041 classified the two **required** members of this set
(`DiscreteStatus`, `MFA_SecurityAuditRecord`) as `BLOCKED_BY_UPSTREAM_CAPABILITY_ENUM`.
Both keep returning NotSupported and are unadvertised.

* Compile-time tripwires fail the build if a future pinned revision adds a
  corresponding enumerator, or if a reserved value moves.
* A [prepared upstream issue](tasks/GIGE-MEL-041-design.md#6-prepared-upstream-issue-not-filed-by-this-task)
  requests explicit enumerators.

### Error and future behavior

`RequestFor<T>` is a future of an error-or-shared-response result. All paths, including unsupported operations, invalid state, allocation failure where recoverable, and stop/disconnect, must settle the future. A ready future is acceptable; asynchronous implementation is not a reason to leave a request unresolved.

Use the actual Common MEL error enumeration. It includes invalid ID/state/parameters, insufficient permissions/resources, and unsupported categories; it does **not** have a dedicated timeout enumerator. Preserve native diagnostics in a description and choose the documented mapping. Do not confuse these errors with IR `Return::{Success,BadPointer,Fail,NotSupported,NotImplemented}`.

### Qualification gates

The tests and hardware evidence for each claimed milestone should cover these behaviors:

| Area | Required evidence |
| --- | --- |
| ABI/loading | Load actual exported factory names from the built shared object and call through upstream interfaces |
| Image ownership | Hold one delivery while newer frames arrive; prove its bytes are unchanged until explicit release |
| Bounded resources | Exhaust the destination/source pools; verify bounded behavior and recovery after release |
| Lifecycle | Repeated enable/disable, held images across stop, invalid registration/unregistration, and detached/foreign objects |
| Commands | Valid full snapshots, invalid ROI/exposure/rate, unchanged state on prevalidation failure, and shared C2/Image state |
| Callback behavior | Duplicate registration, unsupported registration, reentry policy, callback failure, and no user callback under internal locks |
| Metadata | Correct dimensions/stride/time/configuration generation; incomplete or stale frames are not delivered as valid |
| Real SDK | Device unavailable/busy/disconnected, interrupted receive, late callbacks, packet loss, timestamp reset/wrap, and reconnection policy |
| Second backend | Same shared MEL-provider portability scenarios and MEL client; no new vendor types in common headers. The scenarios already pass for the synthetic backend and a test-only fixture backend ([backend portability](backend-portability.md)); that does not complete P2 or P3 |

These are release criteria, not assertions that hardware qualification has occurred.
The [bootstrap validation record](validation.md) and [Qualification Evidence](qualification-evidence.md)
distinguish original executions from later milestones.

[stacked]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/stacked-image/StackedImageChannel.h
[stacked-listener]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/stacked-image/StackedImageListener.h
[metadata-enum]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/ChannelMetadataCapabilityType.h

### GIGE-MEL-013: reproducible evidence, no new MEL surface

An uninstalled Python collector invokes the existing diagnostic (inspect-only by
default; capture/time-probe require `--acquire`), validates P5 and schema-v2 JSONL,
retains complete hashes/metadata and generates an offline-verifiable manifest and
13-item all-open qualification checklist. The diagnostic's optional, default-off
`--discovery-interface IFNAME` restricts child discovery before device I/O; exact
ID remains required. The actual pinned Aravis 0.8.36 network simulator exercises
this path, including native release and offline verification. This is **not** a
physical-camera qualification or a supported Aravis MEL image profile. Timestamp
admission, GIGE-MEL-012 control-loss behavior and all public surfaces are unchanged.
All thirteen [timing gates](aravis-integration.md#camera-timing-qualification-checklist),
physical identity/NIC/MTU/loss/disconnect and reliability remain open. The simulator
wildcard-discovery-listener exception remains unchanged; child discovery
restriction does not establish network isolation.
