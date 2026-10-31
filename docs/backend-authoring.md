# Adding a camera backend

## Starting point

The internal source contract is [`include/gige_mel/backend.hpp`](../include/gige_mel/backend.hpp), in namespace `gige_mel::backend`. The first implementation is [`src/backends/synthetic.cpp`](../src/backends/synthetic.cpp).

There are three distinct levels of test evidence:

| Level | Where | What it establishes |
| --- | --- | --- |
| Synthetic-specific backend tests | [`tests/backend_tests.cpp`](../tests/backend_tests.cpp) (`backend_contract`) | The synthetic backend's own discovery, validation, pool, cancellation and lease behavior. Its numbers (pool of four, IDs from 1, 640 × 480) are not adapter requirements |
| Shared MEL-provider portability scenarios | [`tests/provider_backend_contract_tests.cpp`](../tests/provider_backend_contract_tests.cpp) (`provider_backend_portability`) | The same MEL-facing scenarios pass against the synthetic backend **and** an independent test-only fixture backend with different identities, limits, padded rows, frame IDs, clock and apply semantics. See [backend portability](backend-portability.md) |
| Future SDK/hardware qualification | Not yet available | Real SDK, camera, transport, timing and disconnection behavior; nothing in the first two levels substitutes for it |

A new adapter adds a `BackendCase` to the portability suite and writes its own
direct contract checks. It does not edit the shared scenarios to accommodate
itself.

The backend interface contains project-owned C++ values. It must not contain MEL classes, Aravis buffers, pylon grab results, eBUS handles, `GenApi::INodeMap`, vendor exception classes, or platform-specific SDK structures. Keep those inside the adapter implementation. Build the adapter with the provider; this interface is not a stable binary plugin ABI.

No real SDK adapter is included or selected by this bootstrap. Aravis, Pleora eBUS, a vendor SDK such as pylon, and a GenTL consumer are candidates to evaluate. An adapter name in a design document is not a supported CMake option, a working driver, or hardware qualification.

## The three interfaces

| Interface | Current methods | Responsibility |
| --- | --- | --- |
| `ICameraBackend` | `id`, `enumerate`, `open` | Library/backend identity, discovery, stable device selection, SDK-global resource ownership |
| `ICameraDevice` | `capabilities`, `configuration`, `apply`, `create_stream`, `close` | One selected device, actual configuration, feature application/readback, and logical close |
| `IFrameStream` | `start`, `receive`, `request_stop`, `stop`, `statistics` | One acquisition stream, bounded image delivery, wakeable stop, and source statistics |

Use `Result<T>` for values or project-owned `Error`; use `Status` for operations without a value. The current `ErrorCode` values are `invalid_argument`, `invalid_state`, `not_found`, `busy`, `unsupported`, `resource_exhausted`, `timeout`, `stopped`, `disconnected`, and `io_error`.

Ordinary SDK failures must be translated into those values with useful diagnostic text. Do not leak a vendor exception across the interface. The bootstrap does not promise allocation-free error reporting: allocation while constructing a result, string, backend, or shared owner may still throw. Catch/translate exceptions at the owning provider boundary as appropriate. Nonthrowing stop/destructor/release paths must actually remain nonthrowing.

### Backend identity and selection

`DeviceInfo` has `backend_id`, `device_id`, and `display_name`. A device ID must remain stable under enumeration reorder. Do not use “camera index zero” as a persistent identity. Record enough additional deployment information outside this small initial structure to diagnose camera serial/model/firmware, interface, SDK version, and transport selection.

Opening an unknown device should return `not_found`; a device already exclusively owned should have a defined `busy` behavior. Decide whether one SDK-global context can support more than one device and keep that ownership in the adapter, rather than using uncoordinated global initialization/shutdown calls.

The synthetic factory is `make_synthetic_backend(SyntheticOptions)`. Its backend/device identifiers are `synthetic` and `synthetic-0`. Invalid options are reported by `open`; constructing the factory does not perform camera I/O.

## Configuration contract

The common provider uses the same transaction for typed CameraCommand and C2
ConfigSet schema v1. The [configuration profile](configuration.md) separates JSON
representation validation from backend-specific limits and documents readback,
partial application, error/status mapping and disabled-acquisition requirements.
Adapters need no JSON dependency and must not impose synthetic-only limits in
the common parser or facade.

The current `CameraConfiguration` is a complete typed snapshot:

| Field | Meaning in the initial profile |
| --- | --- |
| `width`, `height` | Image ROI dimensions in pixels |
| `offset_x`, `offset_y` | ROI origin within the sensor |
| `frame_rate_hz` | Integer acquisition rate in hertz |
| `exposure` | Integration duration as `std::chrono::nanoseconds` |

The profile is staring Mono8, one band, with no external trigger. Do not silently add Bayer conversion, free-running/trigger switching, fractional-rate rounding, auto-exposure, or arbitrary gain controls under the same semantics.

`CameraCapabilities` exposes sensor bounds, ROI increments, rate limits, minimum exposure, and booleans for Mono8, external trigger, and external-buffer acquisition. Some future cameras need a richer capability model: coupled ROI limits, dynamic feature availability, fractional frame rates, multiple pixel formats, or exposure increments. Extend the model with a concrete second-backend use case and tests; do not publish fictitious uniform limits just to fit the current struct.

`apply` requires a stopped device/stream and returns the actual readback. Perform all possible validation before writes. For real hardware, writes to several features are not automatically atomic. If a later write fails, report the failure and preserve access to the real resulting configuration. The provider must decide whether to restore the previous settings or leave the device disabled; never return the requested snapshot as if it were verified readback.

The synthetic implementation validates the whole request before changing state. Its initial image is 64 × 48 within a 640 × 480 synthetic sensor, with 1 ms exposure at 30 Hz. Its supported integer rate range is 1–120 Hz, and exposure must fit the frame period. These synthetic limits are test fixtures, not limits promised for a real camera.

### Keep camera profiles separate from SDK mechanics

For the first real SDK, factor feature translation into a small camera-profile helper under `ICameraDevice::apply`:

| Shared intent | Typical profile decision | SDK adapter's job |
| --- | --- | --- |
| Width/height/offset | Feature names, limits/increments, order of updates, acquisition-stop requirement | Read/write typed native features and report actual values |
| Integration time | Which exposure feature exists; nanoseconds versus its native unit; allowed rounding | Convert with overflow checks, set, read back, and report the actual duration |
| Frame rate | Whether rate control is enabled; allowable rate; coupling to exposure and trigger mode | Perform native operations without hidden unrelated changes |
| Mono8 payload | Feature availability, payload/image offset, whether chunks are enabled | Request the selected format and describe the actual returned image |
| Camera status | Which events or polling values describe connection/acquisition faults | Translate vendor events into project errors/state |

SFNC is useful naming guidance, but SDK convenience methods can have broader effects than a single feature write. For example, Aravis documents its exposure API in microseconds and states that its frame-rate setter disables triggers. The adapter/profile must account for those behaviors; it cannot assume an exposure number is already nanoseconds or that a frame-rate update preserves trigger configuration. See the official [Aravis camera API][aravis-camera].

The bootstrap has no generic feature-tree API and no configurable profile registry. Add them when justified. If a generic feature interface is introduced, preserve integer/float/bool/enum/string/command types, availability/writability, units, bounds/increments, and error provenance. Avoid a bare string-to-string map as the only control contract.

## Frame lease contract

`FrameLease` is move-only. It contains a descriptor, a read-only host byte span, and an opaque shared owner. The span remains valid and unchanged until the lease is destroyed or moved from, including after stream stop or logical device close.

The owner must retain **everything needed to release the image safely**. Depending on the SDK, that may include a grab result, stream, camera session, callback state, or global SDK context. A shared owner of the bytes alone is insufficient if final release calls into an already-destroyed stream. Release/requeue must be nonthrowing. Avoid custom deleters that capture a raw pointer to an object with shorter lifetime.

The initial contract represents host-readable bytes. GPU pointers and RDMA registrations require an explicit extension with matching ownership and synchronization semantics; do not cast them to a host span.

### Required descriptor meaning

| Descriptor field | Adapter obligation |
| --- | --- |
| `width`, `height`, `row_stride_bytes` | Actual image layout, with padding represented in stride |
| `storage_bits_per_pixel`, `significant_bits_per_pixel` | Distinguish storage representation from sensor dynamic range |
| `bands`, `pixel_format`, `byte_order` | Actual data organization; the first contract accepts only `PixelFormat::mono8` |
| `status` | Mark incomplete images truthfully; a received buffer is not necessarily a complete image |
| `native_frame_id` | Preserve the native ID in its available width; MEL conversion/wrap policy belongs in the shared runtime |
| `stream_generation` | Distinguish stream restarts and stale deliveries |
| `configuration_generation` | Identify the configuration under which pixels were acquired |
| `configuration` | Immutable effective camera snapshot for this frame |
| `timestamp` | Value plus clock domain, event, validity, and clock-generation context |

The byte span must cover the valid image layout. Do not claim chunk trailers, compressed payload data, transport headers, or unfilled allocation capacity are image pixels. The shared runtime validates the layout before copying; the adapter must still report it correctly.

`FrameTimestamp::value` uses nanoseconds. Convert device ticks using a documented frequency and checked arithmetic. `ClockDomain` distinguishes unspecified, host monotonic, device, and UTC; `TimestampEvent` distinguishes unspecified, integration start, and reception. These enums express known facts, not wishes. A synchronized camera must still have its epoch and event meaning established. Increment/track `clock_generation` when continuity is broken by reset or a comparable discontinuity.

The synthetic frame timestamp is valid host-monotonic integration-start time within the synthetic model. That is useful for deterministic interface/lifecycle testing but establishes neither UTC nor physical exposure timing.

## Acquisition and stop

The current stream contract permits **one receiving thread per stream**. `request_stop`, `stop`, and `statistics` may run concurrently with receive. The caller joins its receiving thread before destroying the stream object.

`receive(timeout)` must return within its bound subject to normal scheduler execution, or be interrupted promptly by stop/close. Do not hide an infinite native wait behind this method. Prefer native cancellation or finite wait slices, and test the actual SDK behavior. `request_stop()` is nonthrowing and wakes receive. `stop()` is nonthrowing and is safe for lifecycle cleanup.

Return `timeout` for a transient wait expiration; the provider continues receiving
without recording a terminal failure. Return `stopped` for terminal acquisition
termination, with a useful diagnostic. A backend `stopped` result alone does not
prove that the MEL consumer requested shutdown. The provider classifies the
failed receive using its cancellation flag at handling time: cancellation already
observed means normal shutdown; otherwise `stopped` is an acquisition fault, like
other terminal receive errors. Explicit disable and cleanup are required before
a fresh acquisition; there is no automatic reconnect or restart. Outstanding
MEL buffers still require consumer release, and recovery retains diagnostic
history. Test both orderings deterministically rather than assigning a fixed
outcome to an unordered simultaneous stop and fault.

A receive committed before a stop request can still return a lease. The provider gates publication using its own state and generation. Adapters must not rely on “we requested stop” to imply no late native callback or result can exist.

`ICameraDevice::close` is logical close: stop new I/O, wake receive, and preserve outstanding frame leases. Physical native resource destruction may have to wait for those leases. The adapter must document and test the last-lease path, including which thread can execute it.

For SDKs that deliver frames on an SDK-owned callback thread, keep the callback short. Retain/transfer ownership into a bounded internal queue and return. Do not invoke arbitrary MEL consumer code from the SDK's callback while its native locks are held. Aravis explicitly documents callback/thread-safety restrictions in its [thread-safety notes][aravis-thread].

## Bounded resources and statistics

The synthetic source pool defaults to four frames. Holding all four leases causes pool pressure; it does not cause unbounded image allocation. The MEL destination pool is a separate resource limit.

The initial `StreamStatistics` reports delivered frames, incomplete frames, timeout count, and pool-starvation events. Define the exact counting event for a new adapter. A timeout waiting for a trigger, an incomplete transport image, and a dropped complete image are different events. Add counters if the distinction matters rather than reporting one ambiguous “dropped” total.

A future adapter should expose enough diagnostics to identify source pool pressure, destination pool pressure, camera disconnection, and packet/payload failure separately. The upstream MEL `InstrumentationReport` is not a generic metrics map; diagnostics need an intentional mapping.

## Two memory paths

### Portable path: native source → registered MEL host destination

1. Acquire an SDK-owned or SDK-managed source buffer normally.
2. Return it as a backend lease with accurate metadata.
3. The provider copies/repackages only valid Mono8 rows into a free registered MEL buffer.
4. Release the source lease independently of the consumer's destination lease.

This is the required first implementation path. It avoids assuming every SDK accepts an arbitrary consumer allocation and lets source layouts differ from the published tight layout.

### Optional path: acquire directly into registered MEL storage

The current interface has no direct-buffer registration methods. The `external_buffer_acquisition` capability alone is not an implementation. Add an explicit negotiated contract when a qualified SDK/camera requires it.

Before enabling it, establish memory alignment, payload capacity, minimum announced buffers, image offset, stride, chunk policy, mutability/registration interval, cancellation, revoke ordering, and lifetime after stop/disconnect. Verify whether the SDK can reuse or revoke a buffer while a Skill holds it. If not, keep the necessary native resources alive until release. See [ADR 0002](adr/0002-copy-first-memory.md).

Retaining an SDK-native result can be zero-copy inside the adapter and still fail the MEL requirement that the image live in a registered consumer allocation. Keep that distinction explicit in performance claims.

## If the backend uses GenTL

A GenTL-based adapter would be a **GenTL consumer**, loading one or more vendor **GenTL producers** (`.cti` libraries). The project would still be a **MEL provider** toward the Skill. These are different roles on opposite sides of the adapter.

GenTL standardizes the transport-layer producer boundary; it does not remove camera feature/profile work, provide every SDK as a CTI, or make buffer lifetimes trivial. Announce/queue/revoke, event cancellation, device/module ownership, and producer unloading still require qualification. Feature interpretation typically involves GenApi and the camera's feature descriptions. Use the official [EMVA GenICam material][genicam] and [GenTL specification][gentl] for the pinned implementation version.

Do not force every backend through GenTL. An SDK with a well-supported native API can implement `ICameraBackend` directly. GenTL is one useful adapter implementation, not the provider's universal internal ABI.

## Current real-SDK adapter: Aravis control and source acquisition

The optional `src/backends/aravis/` adapter (GIGE-MEL-008, Aravis 0.8.36) is the
first code that follows this guide against a real SDK. It writes GenICam nodes
directly instead of using the `ArvCamera` convenience setters described above.
It keeps the profile translation in an SDK-free `camera_profile.cpp`. Its
direct tests (`aravis_adapter`) cover the unknown-device, exclusive-open,
validation, readback and close items from the checklist below, using Aravis's
in-process Fake device. GIGE-MEL-009 adds `IFrameStream`: SDK-buffer-backed
leases that survive stop and close, a bounded pool, receive timeout, stop and
close wakeup, restart and failure cleanup, and (GIGE-MEL-009-R1) an uncertain
acquisition-command outcome that blocks reuse until an explicit stop or close
resolves it, plus a fresh read-only acquisition preflight
(`aravis_stream_contract`). It is
**not yet a supported adapter**: timestamps are deliberately invalid (no time
mapping), so the common provider rejects its frames; disconnection handling, a
portability `BackendCase` and any hardware qualification are pending. See
[Aravis integration](aravis-integration.md).

## Adapter acceptance checklist

Before an adapter is called supported:

- Record SDK version, supported platform/compiler, licensing/redistribution requirements, camera models/firmware, and feature profile.
- Make SDK dependency discovery optional; the synthetic/core build and tests must still work without that SDK installed.
- Add a `BackendCase` for the adapter and pass the shared MEL-provider portability scenarios ([backend portability](backend-portability.md)) with its own identities, capabilities, configurations and documented frame/time facts.
- Write adapter-specific direct contract tests for unknown device, exclusive/open behavior, full configuration validation, readback, receive timeout, stop wakeup, and close. The synthetic `backend_contract` suite is synthetic-specific and is not reusable as-is.
- Hold a source lease across additional frames, pool exhaustion, stream stop, and logical device close; verify immutable bytes and valid final release.
- Verify source-pool bounds and recovery after release, including release from another allowed thread.
- Verify frame completion status, stride/payload validation, native ID behavior, generation boundaries, and time conversion.
- Test disconnection during receive and control, native error translation, late callbacks, and teardown with queued/held results.
- Load the real MEL provider using the same native client used for synthetic, receive registered-buffer images, issue both C2 and Image camera commands, and stop/restart.
- Record actual hardware results separately from synthetic/unit test results. Do not infer network throughput, packet-loss recovery, PTP accuracy, or latency from a fake camera.

When the second adapter requires a backend-specific condition in the MEL facade, first ask whether the missing distinction belongs in a typed capability, a normalized frame field, or the camera profile. A vendor `if` in the facade is usually evidence that the boundary needs work.

[aravis-camera]: https://aravisproject.github.io/docs/aravis-0.8/ArvCamera.html
[aravis-thread]: https://aravisproject.github.io/docs/aravis-0.8/thread-safety.html
[genicam]: https://www.emva.org/standards-technology/genicam/introduction-new/
[gentl]: https://www.emva.org/wp-content/uploads/GenICam_GenTL_1_6.pdf
