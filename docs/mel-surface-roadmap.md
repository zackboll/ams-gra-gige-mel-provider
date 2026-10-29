# MEL surface inventory and implementation order

## How to read this plan

The API in scope is **AMS-GRA IR MEL**, with its Common MEL dependencies. This repository provides that API to Skills; it does not merely call an existing provider through a consumer bridge.

This inventory uses the annotations in the pinned IR MEL commit `8d9224519f12b44e0b28815755c56a32a28d24a0` and Common MEL commit `f6908437d8fd2f7fb69896f9eb9cfd272d10c439`. “Required,” “conditional,” and “optional” below describe those header annotations. They are separate from implementation priority and from what the bootstrap currently supports. Annotation inconsistencies are called out explicitly; the table is not a substitute for an agreed deployment conformance profile.

**Current release boundary:** an experimental, native C++ image-stream provider using the synthetic backend. The manager/control/buffer path, selected common channel operations, image delivery, and camera commands form the implemented subset. Unimplemented overloads on attachable channels return an explicit unsupported result; absent channel families are not advertised or attachable. Completing a pure virtual class with unsupported methods does not fulfill its required behaviors.

## Priority order

| Priority | Deliverable | Why it comes here | Exit criterion |
| --- | --- | --- | --- |
| P0 — bootstrap | Native factories, identity, Control, C2/Image lifecycle, registered host buffers, Mono8 frames, camera commands, synthetic backend, ownership tests | Establish the real provider boundary and memory contract before integrating hardware | A native client loads the library through upstream signatures, receives deterministic frames, holds/releases buffers safely, stops, reconfigures, and restarts |
| P1 — operational MEL profile | Required C2 commands/metadata, required Image metadata/navigation, Health channel, and decisions on stack/task semantics | These are conformance gaps even when a camera image already appears | Each claimed required behavior has a real source, semantics, callback/error behavior, and positive/negative tests; unresolved annotation/profile issues are recorded |
| P2 — first real camera | One selected SDK adapter, one explicit camera profile, device selection, connection loss, packet/payload handling, time mapping | Validates the shared model against actual transport behavior | Hardware qualification demonstrates native MEL image/control/lifetime behavior with a pinned SDK and camera/firmware |
| P3 — second implementation | A second narrow SDK adapter with the same initial Mono8/control profile | Proves the architecture is reusable before deeper SDK-specific optimization | The MEL facade and client remain unchanged; the reusable backend contract suite passes for both adapters |
| P4 — requested capabilities | Additional formats, direct registered-memory acquisition, calibration, protection, richer navigation, and instrumentation | Add only capabilities needed by the selected sensor and Skill | Capability advertisement, command semantics, metadata, and tests expand together |
| P5 — specialized products | IRST tasking/tracking, candidate detection, TSI image stacks, advanced bands/scan modes | These require sensor/product semantics beyond acquiring GigE frames | Sensor, coordinate, timing, scheduling, and data contracts are defined and qualified end to end |

P1 and P2 can proceed in parallel once P0 is stable. P5 is not permission to ignore a required annotation: any deployment that requires one of those surfaces moves it into that deployment's release gate. In particular, the stack and scheduling annotations need an explicit decision before any broad MEL-conformance claim.

## P0: provider creation and common objects

### Factories, manager, and Control

The initial entry points use the upstream C++ ABI. The factory symbols have C linkage, but return `std::shared_ptr` and take C++ strings/objects. See [Control][control], [Buffer][buffer], and [IRFactory][factory].

| Surface | Bootstrap | Follow-up/acceptance |
| --- | --- | --- |
| Global `getAPI_Manager(const std::string&)` | Provider context factory | Reject unknown selections; make selected backend/device and context ownership explicit |
| `getControl(std::string_view, std::shared_ptr<::API_Manager>)` | Concrete MEL Control factory | Reject mismatched instance/manager types; do not let unrelated contexts share state accidentally |
| `getBuffer(std::string_view, std::shared_ptr<::API_Manager>)` | Registered-host-memory wrapper factory | Associate wrappers with the correct context and validate ownership on registration |
| `API_Manager` lifetime | Own the provider context | Channels/buffers must not call through an already-destroyed context |
| `Control::init(const std::string&)` | Initialize the selected synthetic profile | Define repeat-init and unknown aperture/config behavior |
| `Control::getCapabilities() const` | Initial discovery snapshot | This returns a reference; the bootstrap keeps the snapshot stable. Use attached `Channel::getCapabilities()` for the current image layout after a command |
| `Control::getVersionInfo() const` | Implementation/upstream version information | Keep implementation version separate from API version and SDK/camera versions |
| `Control::attachChannel(const Config&)` | One C2 and one Image channel per manager | Check type, channel identity, listener, and ownership; unsupported families remain unavailable |
| `Control::detachChannel(std::shared_ptr<Channel>)` | Stop/detach the owned channel | Reject foreign objects; preserve outstanding image bytes until consumer release |

The alternative registry loader uses `createC2Channel`, `createImageChannel`, `createHealthStatusChannel`, `createTrackChannel`, `createSchedulingChannel`, `createInstrumentationChannel`, and `createStackedImageChannel`, with `std::shared_ptr<SpecificChannel>(Config&)` signatures. These are **not exported by implication** when the manager/control path exists. Add them if a target Skill uses upstream `IRFactory`/`IRRegistry`, and test that path separately.

### Common Channel surface

All concrete channel families inherit these required members from [Channel][channel].

| Member | Bootstrap | Required behavior to retain as adapters grow |
| --- | --- | --- |
| `sendKeepAliveRep()` → `RequestFor<Return>` | Supported for attached channels | Return an initialized future, including failure paths; define the effect of detached/closed state |
| `send(ChannelCommsTestReq)` → `RequestFor<ChannelCommsTestRep>` | Supported | Preserve request correlation and produce the corresponding response/callback semantics |
| `registerMetadataCallback(ChannelCommsTestRep)` | Supported | First valid registration succeeds; duplicate registration fails; callback runs outside provider locks |
| `registerBuffer(std::shared_ptr<Buffer>)` | Image host buffers | Require a disabled channel, correct provider affinity, valid nonoverlapping memory, and sufficient capacity |
| `unregisterBuffer(std::shared_ptr<Buffer>)` | Image host buffers | Require a disabled channel and respect an outstanding delivery; do not free consumer memory |
| `enable()` | C2 control state; Image acquisition | Validate initialization, listener, buffers, backend state, and current layout before accepting acquisition |
| `disable()` | Stop new image publication and join from a control thread | Wake receives, contain late callbacks, preserve held bytes, and avoid callback-thread self-join |
| `getCapabilities() const` | Current channel snapshot | Report actual current dimensions, pitch, byte counts, image format, and supported metadata |

There is no general unregister-metadata-callback overload in this interface. Replacing or clearing a callback must not be invented as a compatible upstream API. Consumers should use channel lifetime and the supported registration contract.

Image and synchronous metadata callbacks share the [profile's callback policy](implementation-profile.md#callbacks-threads-and-shutdown): lifecycle/configuration mutations, including Control lookup/creation, reject calls from any provider callback on the current thread, across provider instances. Explicit buffer release, capability/statistics queries, keep-alive, and correlated communications remain available. Queue mutations to a control thread and return without waiting on work that could depend on callback completion. External detach closes metadata admission and drains admitted callbacks.

### Buffer and image delivery

All [Buffer][buffer] members are required: `init`, `getBufferAddress`, `getImageAddress`, `getSize`, `getContext`, `release`, `getFlags`, `addFlag`, and `setFlags`.

The bootstrap supports host storage. It must not advertise GPU or RDMA support merely because the upstream `BufferFlag` enum contains those values. A flag request cannot turn an arbitrary host pointer into an SDK-compatible GPU/RDMA allocation.

The consumer supplies an `ImageListener` in `Config`. The provider invokes:

```cpp
void ImageListener::onImage(
    const Channel& channel,
    const FrameHeader& header,
    std::shared_ptr<Buffer> buffer);
```

The initial delivered layout is one-band Mono8 with contiguous rows. A delivery keeps a destination slot occupied until `Buffer::release()`. Retaining only a shared-pointer copy is not the release protocol. The provider must use checked sizes, preserve the caller's context, refuse overlapping registered memory, and keep all pools bounded.

`FrameHeader` mapping includes `systemTime`, `integrationTime`, width/height, `bitsPerPixel`, `numBands`, FOV, contributing sensor, format, 32-bit frame/subframe fields, image type/flip/flags, dither, ROI offsets, navigation/inertial vectors, and band index. Every populated value needs an actual meaning. Synthetic FOV/time assumptions must be explicitly synthetic. Hardware navigation or optical calibration cannot be inferred from a GigE payload. See [FrameHeader][frame-header].

## P0/P1: C2Channel

[C2Channel][c2] has nine command overloads and six additional metadata registrations. It inherits the common Channel methods and common communications callback.

### C2 commands

| Request → response | Upstream annotation | Bootstrap | Priority and implementation task |
| --- | --- | --- | --- |
| `BIT_Command` → `Return` | Required | Partial: bounded provider-software test ID 1 | [Software BIT profile](software-bit.md); hardware/asynchronous BIT remains future work |
| `CalibrationConfigurationCmd` → `mel::CalibrationConfiguration` | Optional | Unsupported | P4: return genuine calibration configuration; this is a query, not a generic execute-NUC operation |
| `CalibrationStatusCmd` → `mel::CalibrationStatus` | Optional | Unsupported | P4: report actual calibration state and progress semantics |
| `CameraCommand` → `CameraCommandResp` | Required if camera control | Supported for staring Mono8 profile | P0: serialize with Image commands; full snapshot, validation, readback, command correlation, and disabled-stream policy |
| `CameraProtectCmd` → `CameraProtectCmdResp` | Required if camera protection | Unsupported | P4 if sensor supports it: define safe actuation/state and actual response; never inherit default OK for unsupported hardware |
| `EraseCommand` → `EraseCommandType` | Optional | Unsupported | P4 only with a defined target and lifecycle semantics |
| `SystemTrackDataResponse` → `CommandStatus` | Optional | Unsupported | P5 or earlier pointing integration; distinct from the same-named TrackChannel operation |
| `ModeCmd` → `MFA_Mode` | Required | Unsupported | P1: define accepted MFA states/modes and their relationship to acquisition/tasking |
| `ConfigSetCommand` → `Return` | Required | Supported for provider-defined camera schema v1 | [Exact configuration/status profile](configuration.md); other configuration domains remain future work |

### C2 metadata

| Callback payload | Upstream annotation | Bootstrap | Priority |
| --- | --- | --- | --- |
| `mel::BIT_Configuration` | Required | One observed-state test catalogue | [Software BIT profile](software-bit.md) |
| `CommandStatus` | Required | Supported for C2 ConfigSet, CameraCommand and the documented BIT_Command subset | [Synchronous terminal Accepted/Rejected profile](configuration.md) and [software BIT command outcomes](software-bit.md); ModeCmd and other unsupported commands produce no status; not complete C2 status coverage |
| `mel::CalibrationConfiguration` | Optional | NotSupported | P4 |
| `CameraProtectCmdResp` | Required if camera protection | NotSupported | P4 when protection is implemented |
| `mel::CalibrationStatus` | Optional | NotSupported | P4 |
| `mel::BIT_Status` | Required | Latest software result and current software faults | [Software BIT profile](software-bit.md) |

`CameraCommandResp` metadata registration is on **ImageChannel**, not C2Channel. Do not add a new C2 overload under the assumption that every response type has a C2 callback. `CameraProtectCmd` and its response are declared together in `CameraProtectCmd.h`.

## P0/P1: ImageChannel

[ImageChannel][image] adds two command overloads and eleven metadata registrations.

| Command → response | Upstream annotation | Bootstrap | Priority |
| --- | --- | --- | --- |
| `CameraCommand` → `CameraCommandResp` | Required if camera control | Supported for the initial profile | P0, shared implementation with C2 |
| `NavigationReport` → `NavigationReportResp` | Required | Unsupported | P1: define accepted navigation frames/timing and how reports associate with imagery |

| Callback payload | Upstream annotation | Bootstrap | Priority and data source |
| --- | --- | --- | --- |
| `BadPixelList` | Required | NotSupported | P1: camera calibration/profile; an empty list is meaningful only when it truthfully represents the selected source |
| `OpticalDistortionMap` | Optional | NotSupported | P4: calibrated optics/profile |
| `LineOfSightReport` | Required | NotSupported | P1: mounting/pointing/navigation model |
| `LineOfSightQuaternion` | Required if quaternion LOS | NotSupported | P4 if that coordinate representation is supported |
| `LineOfSightEuler` | Required | NotSupported | P1: mounting/pointing/navigation model |
| `CameraCommandResp` | Required if camera control | Supported | P0: emit correlated responses through the shared camera-command path |
| `NavigationReportResp` | Required | NotSupported | P1 alongside navigation input |
| `LOS3D_KinematicsType` | Optional | NotSupported | P4: navigation/pointing integration |
| `CandidateObjectMessage` | Required if candidate detection | NotSupported | P5: actual detector output, not an image counter |
| `CandidateObjectPreProcMessage` | Optional | NotSupported | P5: detector/preprocessing product |
| `NUC_TempData` | Optional | NotSupported | P4: actual sensor NUC/calibration temperature data |

The `LOS3D_KinematicsType` registration takes its `std::function` by `const&`; match the exact upstream signature. All other signatures must also be compiled against the pinned headers rather than recreated from a prose table.

## P1: HealthStatusChannel

[HealthStatusChannel][health] is annotated required and has no extra `send` overloads. It adds eight callbacks to the common Channel interface. The provider now advertises and attaches a **partial** Health channel: common communications plus MFA_Status, MFA_StatusDetailed, the bounded software [BIT_Status](software-bit.md), and the bounded software [SubsystemStatusResp](subsystem-status.md), with zero image geometry. All four are published by one 100 ms worker from one captured core state per batch. The other four Health registrations (LFStatus, DiscreteStatus, MFA_SecurityAuditRecord, NUC_TempData) remain unsupported and unadvertised. This does not close the required Health conformance gaps below. See [the exact implemented contract](health-status.md) for 100 ms current-state snapshots, state mapping, diagnostic vocabulary, lifecycle and limitations.

| Callback payload | Upstream annotation | Priority and intended source |
| --- | --- | --- |
| `LFStatus` | Required if local functions | P4 if supported local functions exist |
| `mel::MFA_Status` | Required | Partial: sampled observed provider readiness/acquisition state; no hardware health claim |
| `mel::BIT_Status` | Required | Partial: latest software result/current faults on existing Health worker; hardware BIT remains future work |
| `SubsystemStatusResp` | Required | Partial: one provider **software** subsystem/CSCI with version 0.1.0, current readiness, committed BIT-state revision and prepared-report sequence ([profile](subsystem-status.md)); physical device/transport inventory, dependent subsystems, assigned criticality and connection probing remain P1/P2 work |
| `mel::DiscreteStatus` | Required | P1: define meaningful supported discrete state |
| `mel::MFA_SecurityAuditRecord` | Required | P1: define which actual provider events generate records |
| `mel::MFA_StatusDetailed` | Required | Partial: schema-1 provider diagnostics; no transport/hardware metrics |
| `NUC_TempData` | Optional | P4 if the selected sensor provides it |

A temperature sample is not a complete Health implementation. The implemented increments define initial/late periodic publication, ordering, shutdown, current acquisition failure versus historical diagnostics, the bounded [software BIT profile](software-bit.md), and the bounded [software subsystem profile](subsystem-status.md). They do not provide an event log, physical connection probing, hardware health, hardware BIT, calibration or full Health conformance. Remaining table entries describe broader upstream work, not capability claims.

## P1 profile decision / P5 implementation: SchedulingChannel

[SchedulingChannel][scheduling] is class-annotated `RequiredIfIRSTTrackTasking`. Individual members have more specific conditions:

| Operation or callback | Upstream annotation | Work required before support is advertised |
| --- | --- | --- |
| `send(PrioritizedTrackListCmd)` → `TrackListResponse` | Required within the declared channel | Track-list validation, revision, correlation, and admission semantics |
| `exec(AbortTask)` → `Return` | Required if IRST scan/stare/track tasking | Cancel active/queued work and define completion ordering |
| `exec(IRSTScanTask)` → `Return` | Required if IRST scanning | Pointing/scan execution, timing, and sensor constraints |
| `exec(IRSTStareTask)` → `Return` | Required if IRST staring | Coordinate/duration interpretation; fixed-camera limits must be explicit |
| `exec(IRSTTrackTask)` → `Return` | Required if IRST tracking | Tracking/pointing execution and state machine |
| `TaskExecutingRep` callback | Required if IRST track tasking | Publish actual task state |
| `ExecuteTaskAck` callback | Required if IRST track tasking | Distinguish acceptance from completion |
| `SchedCreatedRep` callback | Required if IRST track tasking | Report an actual created schedule |
| `UpdateTrackListResponse` callback | Required if tracking | Correlate actual track-list updates |
| `TaskEvents` callback | Optional | Defined task-event lifecycle |
| `ScanPerformanceReport` callback | Optional | Actual scan-performance measurements |

The [ChannelCapability][capability] comment says every EOIR MFA has task scheduling, and **`TaskScheduleDepth == 0` means an immediate task replaces the executing task**. It does not mean “no scheduling.” The bootstrap's lack of tasking therefore remains an explicit limitation; do not treat a zero-initialized field as a conformance waiver. Resolve this contract/profile question before a release claims general EOIR MEL support.

## P5: TrackChannel

[TrackChannel][track] is conditional on tracking. It is not advertised or attachable in the bootstrap.

| Operation or callback | Upstream annotation | Work required |
| --- | --- | --- |
| `send(SystemTrackDataResponse)` → `CommandStatus` | Optional | Feed external system tracks into the actual algorithm |
| `send(TrackDataUpdate)` → `CommandStatus` | Required if track updates | Define update acceptance, correlation, and stale input handling |
| `CandidateObjectMessage` callback | Required if candidate detection | Real detector output |
| `IRSTTrackReport` callback | Required if tracking | Actual track reports with IDs, timing, and coordinates |
| `RequestSystemTrackData` callback | Optional | Requests for externally supplied tracks/pointing support |
| `CandidateObjectPreProcMessage` callback | Optional | Defined detector/preprocessing product |

`Config::supportsSystemTrackData` and `supportsTrackDataUpdates` describe the **Skill's ability to supply data**. They are not proof that the camera contains a detector or tracker.

## P4: InstrumentationChannel

[InstrumentationChannel][instrumentation] is conditional on instrumentation. Add `send(InstrumentationLevelCmd)` → `InstrumentationReport` and registration of `InstrumentationReport` together, plus inherited Channel methods.

The pinned `InstrumentationReport` carries command ID, size, timestamp, and instrumentation priority (`Normal`/`Debug`). It is not an arbitrary counter map. Keep backend packet/drop/pool statistics in an appropriate project diagnostic surface until a supported mapping exists; do not squeeze unrelated numbers into unrelated MEL fields.

## P1 profile decision / P5 implementation: stacked imagery

[StackedImageChannel][stacked] is class-annotated **Required**, while [StackedImageListener][stacked-listener] and its `onImageStack` operation are conditional on image-stack support. The channel's additional `LOS3D_KinematicsType` callback is optional. Record and resolve that distinction for the target profile; do not describe the whole channel as unconditionally optional.

The bootstrap does not expose this channel. Supporting it requires the inherited Channel surface, the appropriate listener in the consumer configuration, `StackFrameHeader`, and delivery through:

```cpp
Return StackedImageListener::onImageStack(
    Channel& channel,
    const StackFrameHeader& header,
    std::shared_ptr<Buffer> buffer);
```

A set of consecutive frames or a GigE multipart payload is not automatically a valid MEL image stack. Define the stack's spatial/temporal/band semantics, source association, layout, and lifetime before mapping it.

## Cross-cutting obligations

### Capability accuracy

`ChannelCapability` covers identity, image width/height, signal bit depth, byte pitch, buffer/image sizes, bands/format, sensor types, platform/location, channel types, task queue depth, distortion/NUC availability, metadata, band information, and navigation frames. Populate it from the selected source and implemented profile; do not blindly publish SDK feature discovery.

The pinned [ChannelMetadataCapabilityType][metadata-enum] lacks entries corresponding to `CalibrationConfiguration`, `CalibrationStatus`, `NUC_TempData`, `DiscreteStatus`, and `MFA_SecurityAuditRecord`, despite declarations of those callbacks. **Do not repurpose reserved enum values.** A conformance/deployment decision or upstream resolution is needed when those surfaces are implemented, and tests must check both registration and advertisement behavior.

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
| Second backend | Same contract tests and MEL client; no new vendor types in common headers |

These are release criteria, not assertions that hardware qualification has occurred. The README's validation section records what was actually run for this bootstrap.

[control]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/Control.h
[buffer]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/Buffer.h
[factory]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/factory/IRFactory.h
[channel]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/Channel.h
[frame-header]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/FrameHeader.h
[c2]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/c2/C2Channel.h
[image]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/image/ImageChannel.h
[health]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/health-status/HealthStatusChannel.h
[scheduling]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/scheduling/SchedulingChannel.h
[track]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/track/TrackChannel.h
[instrumentation]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/instrumentation/InstrumentationChannel.h
[stacked]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/stacked-image/StackedImageChannel.h
[stacked-listener]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/stacked-image/StackedImageListener.h
[capability]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/ChannelCapability.h
[metadata-enum]: https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/ChannelMetadataCapabilityType.h
