# GIGE-MEL-MSTREAM-001 — Multi-stream acquisition foundation

Audited base: `9593bde07d7451d325326074e797132590566ece` (fetched `origin/main`).
This task qualifies a backend fixture, not new MEL channels, native streaming,
or physical-camera behavior.

## Baseline and disposition

| Where | Single-stream assumption | Disposition |
| --- | --- | --- |
| `backend.hpp`, `provider.hpp` | `create_stream()` has no selector; `CameraCapabilities::pixel_format` and `CameraConfiguration` describe the one legacy Image view. A frame has no source/product key. `ProviderSettings` selects a device, not a channel. | Add selected, transactional device start, topology, and frame keys. Keep the legacy view for unchanged MEL consumers; a multi-stream-only device rejects that configuration view. |
| `camera_profile.hpp`, `frame_policy.hpp` | One selected format, one configuration/snapshot and one admitted plain native image. Preflight and frame validation have no transport selector. | An SDK-free fixture profile defines the new topology using common values. Existing profile virtuals and native validation stay single-image; a future native profile will resolve native stream and payload mappings. |
| Synthetic | One state object conflates device, sole receiver, pool and run state. Configuration requires stopped acquisition. Lease retirement can occur after device close. | One device owns per-source state and the acquisition lifecycle. Preserve the default fixture, single-stream configuration exclusion and retained storage. |
| Aravis | `DeviceCore` owns one active run and a device-wide command outcome; a stream wrapper prepares a single `ArvStream` and issues start/stop. Close deals with uncertain stop and outstanding leases. | Preserve existing sequencing and recovery. Native batch preparation, shared command outcome and selected layout readback are future adapter work. |
| Pleora | One shared `PvDevice`, stream and pipeline, destination/enable on the default channel; one attempted acquisition sequence. Copies into bounded host storage to decouple close from leases. | Preserve ownership, copies and SDK sequencing. Native per-channel receive and separate shared command ownership are future work. |
| GenTL | Camera backend enumerates/opens/readbacks only. Lower-level consumer already tracks DataStream handles and separate host pool/event state. | Report no selectable image streams. No new port operations, acquisition or catalog entry. |
| `Runtime`, `ControlImpl`, `capabilities.cpp` | One immutable Image discovery UUID, one attachment per type and one shared config/layout. Runtime tracks one Image-or-Stack acquisition for Health and Standby. | Unchanged. No multi-stream fixture in the MEL factories or MEL composition catalog. |
| `channel_core.cpp`, `stream_state.hpp`, `frame_delivery.cpp` | Enable freezes one geometry, FOV, configuration generation and buffer pool. Worker tracks one accepted stream generation and clock history. Config/Stack/tasking exclude acquisition; shutdown cancels and joins before physical cleanup. | Unchanged. Source-side generation, bytes and timestamp provenance are independent of other sources; no cross-stream frame or time correlation. |

## Identity and values

```text
Backend / exact device open key
  One opened camera; authoritative acquisition owner
    Transport Stream 0 -> Product 100: Mono8, 64 x 48, 10 Hz
    Transport Stream 1 -> Product 200: Mono12, 80 x 60,  5 Hz, padded rows

Future example (not implemented):
    Transport Stream 0 -> Red Float32 primary + Raw UInt16 auxiliary
    Transport Stream 1 -> Blue Float32 primary
    Image-product ID -> provider-owned MEL channel ID (future policy)
```

Device, transport and product identity have separate scopes. Transport and
product keys are distinct types scoped to one opened device, not list indices,
pixel format identifiers or MEL UUIDs. Each frame has both keys. Multiple
products may reference the same transport; a receiver can eventually issue
multiple product leases with a shared storage owner. `IFrameStream::receive()`
still returns one lease per call. This does not define multipart decoding or
camera exposure correlation.

`stream_topology()` returns owned selectable transport and product descriptors
without starting or preparing acquisition. Each product has effective geometry
and timing, the existing qualified representation, and optional known native
stride and readable span. Packed layout remains computable using `image_layout`.
Variable or unknown native stride is not fabricated. Validate unique transport
and product IDs, source references, image geometry and checked stride/span;
admit multiple products per source. Empty topology means no acquisition. Do not
add a new pixel format: future descriptor-based sample representations can
extend the product/frame values without changing selection, reception or leases.

`CameraConfiguration` remains the complete mutable ROI/timing request only for
the legacy view. A product's configuration is descriptive, not another device
control surface. The multi-stream fixture's profiles are fixed; legacy readback
and all mutations return unsupported, including no-op requests. Future real
device-wide changes must serialize against **all** affected active receivers.
Profile-specific selector access and constraints are not implied by this model.

## Ownership and migration

`ICameraDevice::start_acquisition(selected_ids, configuration_generation)` is
the device transaction: validate a nonempty, duplicate-free selection, prepare
stopped receivers and their resources, then activate the whole selection or
return an error without publishing a partial run. Return unique receiver handles
in selection order. Adapters own SDK-specific staging, command sequencing and
compensation. For a native uncertain outcome, fail closed and preserve the
device's diagnostic/recovery state, not a fiction of successful rollback.

The synthetic owner serializes start, individual stop/cancel and close with one
device mutex. Zero active receivers is idle; the first commit starts camera
acquisition, another selected subset may join without restarting its peers, and
the last retirement returns the camera to idle. One live handle per source
(including stopped handles); caller joins receive before restart or destruction.
Each source owns its configured pool, run generation, native ID sequence, next
delivery and counters. Preparation allocates before committing any generation
or acquisition ownership. Retained leases can block reuse of that source's
bounded pool, never another source. Stop/close wake waits without waiting for
leases; a committed receive may finish after cancellation. Pixel generation
writes directly into pinned slots, outside the shared owner lock. Frame leases
do not keep a receiver handle registered. Repeated stop/close are idempotent.

The default device implementations project the legacy view as one transport and
one product (both default keys) and adapt a singleton batch start to
`create_stream()` / `IFrameStream::start()`. The no-selector entry point is a
compatibility path, not a second multi-stream contract; multi-stream-only
devices refuse it. Migrate MEL and other callers to selected batch start as
product-aware routing lands, then retire the old path when all implementations
and embedders are ready. Existing native ownership and close paths do not change
in this task. Config/discovery/start/close retain caller serialization rules;
selected synthetic teardown also synchronizes concurrent stop, close and receive.

No pinned MEL declaration, factory signature, installed header path or CMake
target changes. The installed project C++ contract **does** change: new virtuals
and a changed `FrameDescriptor`/`FrameLease` layout require rebuilding embedders
and adapters, even though legacy source implementations still compile. No blanket
binary compatibility claim. Profile virtuals and module discovery protocol stay
unchanged. `cmake/ProfileBuildIdentity.cmake` already hashes `backend.hpp` as
part of the matched-C++ closure, and `profile_loader.cpp` checks the identity
before constructing a module profile. Rebuild old modules; do not bypass the
identity check. `builtin_backends.cpp` and `session_selection.cpp` keep their
existing exact backend/profile/device selection and no-I/O composition behavior.

## Native feasibility (pinned API/source inspection, not execution)

- **Aravis 0.8.36:** `arvcamera.h` provides GV stream-channel count, select
  and current-channel access. The pinned `arvcamera.c` uses
  `ArvGevStreamChannelCount` / `ArvGevStreamChannelSelector`; `arvgvdevice.c`
  creates a receiver from the same controller device. `arvgvstream.c` captures
  the selected channel at construction and uses it to retire the destination.
  This supports a select/create/queue sequence per stream under the device lock,
  then a shared `AcquisitionStart`/`AcquisitionStop`. Native receiver queues and
  `ArvBuffer` leases need distinct lifetimes; selector restoration, resend and
  destination cleanup after partial failure or stop need qualification with two
  live streams. `arv_camera_get_payload()` reads the native `PayloadSize` node;
  channel count/selection alone does not establish per-stream ROI, payload size,
  format, endianness or timing. The camera XML/profile must establish those facts.
- **Pleora eBUS Pv 6.5.3.7155:** `PvStreamGEV::Open` accepts channel and local
  port/address; `PvDeviceGEV::SetStreamDestination` and reset accept a channel.
  A plausible setup is one connected `PvDeviceGEV`, one stream and one
  `PvPipeline(PvStream*)` per destination, with distinct pipeline starts and
  buffers, then a shared GenApi `AcquisitionStart`. `PvDevice::StreamEnable` /
  `StreamDisable` accept a channel; header comments also describe setting/resetting
  `TLParamsLocked` and say disable after remote stop and reception drain. Do not
  assume disabling one channel is safe during another's run. Qualify the precise
  enable/disable, destination and pipeline order, GenApi selector-dependent
  `PayloadSize`, entitlement and independent native reception before implementation
  claims. Keep the shared device connected until its SDK resources permit close.
- **EMVA GenTL 1.6:** `DevGetNumDataStreams`, `DevGetDataStreamID`,
  `DevOpenDataStream` bind multiple `DS_HANDLE`s to a `DEV_HANDLE` by exact ID;
  buffers and `DSStartAcquisition`/`DSStopAcquisition` are DataStream-scoped.
  `DevGetPort` exposes the separate remote-device port. Queue and start the
  selected host engines before any remote `AcquisitionStart`; stop the remote
  camera once at final retirement, then stop/flush/revoke and close the remaining
  engines as permitted by producer/camera sequencing. Producer payload sizing,
  remote parameter locking, event cancellation, stop/revoke uncertainty and
  multi-handle operation require separate producer qualification. The existing
  low-level consumer already tracks open streams and refuses early device close;
  it does not start remote acquisition. The camera adapter stays read-only.

## Next MEL integration

Under one `Control`, enumerate qualified products in `getCapabilities()`, give
each a stable provider-local Image UUID, and resolve `attachChannel()` by UUID
**and** channel type, not one attachment per Image type. Keep the existing
singleton UUID for legacy users. Freeze per-product representation/layout,
configuration, FOV and frame/clock admission state at enable. Registered-buffer
pools and listeners are per channel; cross-channel memory alias protection stays
shared. Receive once per transport and route validated product leases without a
source-frame copy. Subscription reference counts are by transport (two products
may share one receiver), while Health, Standby, Stack/task exclusion, device
configuration/readback and close are shared. Specify duplicate subscription,
fault/publication, metadata and retained-buffer rules before adding new channels.
Start that work with the SDK-free fixture; qualify real-camera profile and SDK
sequencing independently before wiring physical products into MEL.