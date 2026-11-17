# GIGE-MEL-GENTL-001 — Generic GenTL consumer feasibility (design)

Base: fetched `main` `60e1fbd` (merge of PR #36). Branch
`feature/gige-mel-gentl-001-feasibility`. This is a feasibility and diagnostic
task: **no production backend, no MEL capability, no factory or selection
change**. The final commit/tree and executed matrix are recorded on the PR (see
[validation](GIGE-MEL-GENTL-001-validation.md)).

**Recommendation: `CONDITIONAL_GO` for Approach B (a native, project-owned
GenTL C consumer), staged.** See section 9 for conditions.

## PR #39 corrective review (GIGE-MEL-GENTL-001-R1)

R1 incorporates `main` `ae4a2377c05e7dd16becf9a5201a5a075163808b`.
The four independent-review findings are corrected without adding a backend,
capability, GenApi implementation, camera/SDK qualification or concurrency API:

* **R1-1, lost startup cancellation:** startup admission resets the previous
  run's cancellation under the event mutex exactly once. The lifecycle is
  idle → starting → running → stopping → idle (or retired after close).
  `request_stop()` latches stopping; announcement and queue iterations, event
  registration and native acquisition startup cannot reset it. A cancellation
  observed before the running transition unwinds acquisition on the control
  thread before returning stopped. A request after that transition is a running
  cancellation and retains the existing `EventKill` wakeup/explicit control-thread
  stop contract. No worker or concurrent normal lifecycle operation is added.
  Producer calls remain synchronous and have no newly invented deadline.
* **R1-2, allocation after native open:** a scoped native owner covers each of
  TLOpen/TLOpenInterface/IFOpenDevice/DevOpenDataStream, including DevGetPort.
  C++ children are inert until object and shared control-block allocation both
  succeed. Only then are handle ownership and parent counts committed, without
  another allocation. Failed construction destroys an inert child, then the
  guard closes the native exactly once; failed cleanup taints the producer and
  appends its native diagnostic to the original allocation/port failure.
  The guard's unwinding destructor does not allocate diagnostic text.
* **R1-3, uncertain teardown ownership:** teardown attempts each stop/flush/
  revoke/unregister once and keeps the first failure plus subsequent diagnostics.
  A rejected start followed by a rejected stop is resolved only by an explicit
  not-grabbing readback; otherwise native activity remains uncertain. Successful
  revokes retire their resources; unsuccessful revokes quarantine the entire
  announcement slot (both bytes and native user pointer). Failed event retirement
  retains the event identity and prevents reuse/kill/re-unregister of a possibly
  invalid handle. Failed teardown forbids restart. DSClose is attempted once:
  success conclusively retires the stream and releases its temporary quarantine;
  failure preserves original diagnostics, taints the producer and leaves uncertain
  resources in the existing process-lifetime quarantine. Thus an earlier error
  does not permanently retain a producer when native cleanup conclusively succeeds.
* **R1-4, moved-from frame:** explicit `noexcept` moves exchange the source span
  with an empty span while moving storage ownership and metadata, matching
  `backend::FrameLease`. Self-move is harmless; other outstanding copy leases
  remain valid across stop/close and actual producer unloading.

Regression coverage uses producer entry/exit barriers (including later buffer
iterations), a native-wait latch, object/control-block allocation injection and
independent teardown failure processes. No sleep-based race test is used.
See the [R1 validation matrix](GIGE-MEL-GENTL-001-validation.md#i-pr-39-corrective-qualification-r1).
The original feasibility decision and physical/vendor qualification gates remain
**CONDITIONAL_GO**, unchanged.

## 1. GenTL API dependency and ABI requirements

The only ABI source is the official EMVA **GenTL 1.6** C header, `GenTL/GenTL.h`
in `GenICam_Package_2026.07.zip` (package SHA-256 `e74b8d98…b64f`, identical to
the value EMVA publishes on its download page; verified locally). Header SHA-256
`5b656a413f4cdc347e80494b87f94b09ac925e391a9022edb7f028d996e35e4e`. Aravis
0.9.2 redistributes a byte-identical copy as `src/gentl/GenTL_v1_6.h` (`cmp`
identical), which gives a second, sub-megabyte provisioning source.

* **Not committed.** The GenICam License (v1.0, 2023) permits redistribution
  only "without modification", and this repository vendors no third-party SDK
  files. `scripts/provision_gentl_header.sh` fetches either archive, verifies the
  archive SHA-256 and the header SHA-256, and installs it outside the tree. CMake
  re-checks the header SHA-256 at configure time and refuses any other file. No
  hand-written constants, signatures or structure layouts are used anywhere.
* The header is included only by the private `src/gentl/gentl_api.hpp`, as a
  system header. No GenTL type crosses `gentl_consumer.hpp`. A preprocessor check
  requires `GenTLMajorVersion == 1 && GenTLMinorVersion == 6`.
* Function pointers use the header's own `GC_API_P` typedefs (`PGCInitLib`, ...).
  On Linux x86-64 `GC_CALLTYPE` is the default calling convention.
* ABI facts taken from the header: handles are opaque `void*`; `bool8_t` is a
  global-scope `bool` in C++ (`uint8_t` in C); every enum is an `int32_t`
  typedef; event and stacked structures are `#pragma pack(1)`; `GENTL_INFINITE`
  is `0xFFFFFFFFFFFFFFFFULL`.
* Producers are loaded only from an explicit absolute path, with
  `dlopen(RTLD_NOW | RTLD_LOCAL)`. There is no `GENICAM_GENTL64_PATH` search.
  The foundation allows one instance per canonical file per process, because
  GenTL libraries are process singletons.

## 2. Existing consumer (A) versus native consumer (B)

### Approach A — Aravis GenTL consumer (inspected at tag 0.9.2, `c1a2326`)

Aravis 0.9.0 introduced an experimental GenTL consumer (`arvgentl*.c`, about
2.5 kLOC) and a separate experimental GenTL producer (`src/gentl/`, off by
default). Only the 0.9.x development series has them; the stable 0.8.36 used by
this project has neither. The following facts come from the source and were then
confirmed by execution against the fixture (validation record, section E):

| Area | Verified behavior |
| --- | --- |
| Producer loading | Only by scanning `GENICAM_GENTL64_PATH` for `*.cti` (`G_MODULE_BIND_LAZY`); there is no API to load one explicit file. It resolves **57** entry points and silently drops any producer missing one. The fixture was dropped for lacking `IFGetInfo` until stubs were added. |
| Registration | The `GenTL` interface sits in Aravis's process-global interface table next to `GigEVision`. `arv_open_device()` searches every enabled interface in order, so GenTL cannot be selected exclusively without globally disabling the others. |
| Open | Always `DEVICE_ACCESS_CONTROL`. One shared `TL_HANDLE`/`IF_HANDLE` per producer, reference counted. |
| GenApi | Reads `GCGetPortURLInfo` index 0 (`Local:`/`File:`, zip supported) and parses it with Aravis's own GenApi (`ArvGc`) over `GCReadPort`/`GCWritePort`. Executed: GenApi `Width = 8` from the fixture XML. |
| Registers | `read_register` is a 4-byte `GCReadPort` passed through untouched. Executed: raw `0x08000000` for a big-endian 8. No port byte order is applied. |
| Streams | One thread per stream, `EventGetData(GENTL_INFINITE)`, stop by `EventKill`, `DSFlushQueue(ALL_DISCARD)`. Buffers come from `DSAllocAndAnnounceBuffer` and are copied into the ArvBuffer if the base pointer differs. A `DSStartAcquisition` failure is only logged; start still returns TRUE. |
| Timestamps | Uses `BUFFER_INFO_TIMESTAMP_NS`, else ticks divided by `DEVICE_INFO_TIMESTAMP_FREQUENCY`, **else host real time** (`g_get_real_time`), i.e. it manufactures a timestamp. This project forbids that. |
| Errors | `GCGetLastError` is resolved but never called; only the GC code name survives. |
| Independence | Usable without native GigE Vision as a library (disabling `GigEVision`/`Fake` left only producer traffic), but it still needs GLib/GObject, libxml2 and the separate `aravis-0.10` ABI series. |

The bundled Aravis **producer** (`ProducerAravis-0.10.cti`) is a GenTL facade
over native Aravis GigE Vision. It reports GenTL version **0.1**, and
`DSAnnounceBuffer`, `DSStartAcquisition`, `DevOpenDataStream`, `DevClose`,
`GCWritePort`, `TLGetInfo`, all port info and every event function return
`GC_ERR_NOT_IMPLEMENTED`. `TLGetInterfaceInfo` dereferences a NULL protocol
string and crashed the probe (`gentl_to_buf → strlen`). `IFOpenDevice` ignores
the access flag: in a loopback simulator it wrote CCP `0x0A00 = 2` (control)
for a `DEVICE_ACCESS_READONLY` open. It is useful only as a **discovery**
producer.

**Assessment of A:** useful reference code and evidence, not a foundation. Global,
environment-driven producer selection; the 57-symbol rule; the host-time
fallback; lost native error text; copy into its own buffers; and an ignored start
failure each conflict with existing provider contracts. Fixing them would mean
maintaining a fork of an experimental 0.9 development series next to the qualified
0.8.36 used by the Aravis backend. Two Aravis ABIs in one process are not
supported.
### Approach B — native GenTL C consumer (foundation implemented here)

Minimum consumer surface; every function is supplied by the producer:

| Need | GenTL calls |
| --- | --- |
| Library init/shutdown | `GCInitLib`, `GCCloseLib`, `GCGetInfo`, `GCGetLastError` |
| Transport enumeration | `TLOpen`, `TLClose`, `TLGetInfo`, `TLUpdateInterfaceList`, `TLGetNumInterfaces`, `TLGetInterfaceID`, `TLGetInterfaceInfo`, `TLOpenInterface` |
| Device enumeration | `IFUpdateDeviceList` (network discovery for GEV), `IFGetNumDevices`, `IFGetDeviceID`, `IFGetDeviceInfo`, `IFClose` |
| Exact open | `IFOpenDevice(id, access)`, `DevGetPort`, `DevGetInfo`, `DevClose` |
| Port access | `GCReadPort`, `GCWritePort`, `GCGetPortInfo` (byte order, access) |
| XML location | `GCGetNumPortURLs`, `GCGetPortURLInfo` (URL, scheme, address, size, schema, SHA-1) |
| Streams | `DevGetNumDataStreams`, `DevGetDataStreamID`, `DevOpenDataStream`, `DSGetInfo`, `DSAnnounceBuffer`, `DSQueueBuffer`, `DSStartAcquisition`, `DSStopAcquisition`, `DSFlushQueue`, `DSRevokeBuffer`, `DSGetBufferInfo`, `DSClose` |
| Waiting/cancellation | `GCRegisterEvent(EVENT_NEW_BUFFER)`, `EventGetData(timeout)`, `EventKill`, `GCUnregisterEvent` |

The foundation treats the first six rows as **25 mandatory** symbols, all
resolved before any producer call, except `GCWritePort`, which is optional. The
16 stream/event functions are an all-or-nothing optional group: a partial table
is discarded, never driven.

GenTL does **not** supply GenApi: only the XML location and raw port bytes.
`DeviceVendorName`, `Width`, `PayloadSize`, `AcquisitionStart`,
`AcquisitionFrameRateAbs` etc. need a separate GenApi implementation (section 7).
`DSStartAcquisition` starts only the host engine, never the remote device.

**Decision:** B, staged (section 9). A remains a comparison reference and a cheap
public third-party producer for discovery checks.
## 3. GenTL producer compatibility assessment

| Producer | Obtained | Executed here | Result |
| --- | --- | --- | --- |
| Euresys eGrabber Gigelink 26.10 (`gigelink.cti`) | **Not obtained.** The public Download Area lists `egrabber-linux-x86_64-26.10.0.11.tar.gz`, but the CMS download API returns `403 "File is protected, please authenticate to download."` for every Linux setup package tried (25.10, 25.12, 26.09, 26.10). Release notes and documentation download anonymously. | Nothing | The 26.10 release notes call Gigelink a "Free-of-charge library", consistent with the reported change in 25.10 (the 23.07 handbook still required a Neo licence). The blocker is an authenticated Euresys account; no credentials were used or stored. |
| Balluff Impact Acquire 3.7.2 (`libmvGenTLProducer.so.3.7.2`, linked as `mvGenTLProducer.cti` by its installer) | Public mirror `static.matrix-vision.com`; installer SHA-256 `8d54f2ca…80ca`. Payload extracted without running the installer (embedded MD5 verified). | Offline load/info/close ×2; host interface enumeration in an isolated network namespace | Reports GenTL **1.6**; exports every function used by either consumer; complete `GCGetInfo`. `TLUpdateInterfaceList` **segfaulted** inside a namespace with no `lo` link; with `lo` up it listed five U3V host controllers and no GEV interface (the shipped `ignoredInterfaces.txt` contains `GEV=I`). Its EULA requires an express Balluff grant for third-party hardware, so it did not contact the Cognex (user decision B). |
| Aravis 0.9.2 producer (`ProducerAravis-0.10.cti`) | Built from the release tarball (SHA-256 `3bf0e79e…af86`) with `-Dgentl-producer=true`. | Offline load ×3; interfaces (namespace); read-only GVCP discovery of the Cognex; open against a loopback simulator | See section 2: discovery only; stream and close entry points unimplemented; open writes CCP. |
| Fixture (`tests/support/gentl_mock_producer.cpp`) | Built in-tree from the verified header. | All offline contracts and sanitizers | Controlled fixture; qualifies nothing physical. |

Producer crashes during optional or enumeration calls (Aravis
`TLGetInterfaceInfo`, Balluff `TLUpdateInterfaceList` without loopback) mean a
production consumer must run producer-facing code where a fault cannot corrupt the
MEL process, or must pin qualified producer versions. Section 9 makes this a
gate.

## 4. Mapping to existing backend interfaces

No new abstraction is needed for the image path. A future `gentl` adapter:

| Interface | GenTL realization |
| --- | --- |
| `ICameraBackend::id()` | `"gentl"`, constructed with an explicit producer path. Never registered by default. |
| `enumerate()` | `TLUpdateInterfaceList`, then per interface `IFUpdateDeviceList` and `IFGetDeviceInfo`. `device_id` is the composite identity of section 5. |
| `open(id)` | Exact composite match; `IFOpenDevice(CONTROL)`; `DevGetPort`; XML load and GenApi node map; then `profile->open(features)`. Partial failures close in reverse order (implemented). |
| `IDeviceFeatures` | GenApi node map over `GCReadPort`/`GCWritePort` (section 7), with the same type/access/bound checks as the Aravis and Pleora adapters. |
| `configuration/apply` | Unchanged profile algorithms over `IDeviceFeatures`, with truthful readback. |
| `create_stream` | `DevOpenDataStream`. Payload from GenApi `PayloadSize`, or `STREAM_INFO_PAYLOAD_SIZE` when `DEFINES_PAYLOADSIZE` is true. |
| `IFrameStream::start` | Announce consumer memory, queue, register `NEW_BUFFER`, `DSStartAcquisition`, then GenApi `AcquisitionStart`, with the Aravis adapter's command-uncertainty rules. |
| `receive` | `EventGetData` in short slices (like Pleora's 10 ms poll), so cancellation stays prompt even if a producer's `EventKill` is unreliable. `NativeFrameFacts` from `DSGetBufferInfo`; copy into the existing lease pool; requeue. |
| `request_stop/stop` | `EventKill`; `AcquisitionStop`; `DSStopAcquisition(KILL)`; `DSFlushQueue`; revoke; unregister. |
| `IRegisterDevice` | `with_registers` over `GCReadPort`/`GCWritePort` under the device mutex, reusing `CheckedRegisters` (implemented). |
| `ICameraProfile` | **Unchanged.** Profiles see only `IDeviceFeatures` and `NativeFrameFacts`. |

### Proposed contract extensions (documented only; none implemented)

* **E1 — explicit producer path.** A construction option, like
  `pleora::Options`. No interface change. Required.
* **E2 — timestamp provenance.** `BUFFER_INFO_TIMESTAMP(_NS)` lies in a
  producer/device-defined domain. `ClockDomain::device` fits only after
  per-producer qualification; until then timestamps stay invalid. No change now.
* **E3 — profile module compatibility bit.** `module::gentl = 4` would change
  descriptor validation in `profile_loader.cpp` and needs its own review.
* **E4 — producer tuning** (packet size, resend, socket buffers) belongs to the
  TL/stream node maps, not the remote device. Profiles must not touch it; the
  adapter needs its own explicit, reviewed policy.
## 5. Device identity and selection strategy

GenTL device IDs are unique only **within one interface of one producer**.
Observed: the Aravis producer reports `COGNEX-CIC-5000R-21874533` on interface
`GigEVision`; Balluff interface IDs are sysfs paths (`/devices/pci0000:00/.../usb1`).
The same camera therefore has different IDs through native Aravis, Pleora
(lowercase MAC), the Aravis producer and Balluff.

* Selection stays exact, per the existing runtime-selection rule. A `gentl`
  device ID is the composite `<interface-id>/<device-id>` of one explicitly
  constructed producer. The foundation already refuses prefix, case-folded,
  serial-number and unlisted IDs without calling `IFOpenDevice` (tested).
* Discovery is explicit (`IFUpdateDeviceList`). Nothing is opened implicitly, and
  `open` never refreshes an empty, pre-discovery list.
* Cross-backend physical identity (MAC/serial equivalence) stays a profile or
  operator concern. The Cognex profile checks vendor, model and firmware after
  open, not the ID.

## 6. Library and buffer ownership model (implemented and tested)

```
Producer (dlopen + GCInitLib) <- System (TL) <- Interface (IF) <- Device (DEV + port) <- Stream (DS)
```

* Each child holds a `shared_ptr` to its parent, so the library cannot be
  unloaded while any handle, registered event or announced buffer exists.
  `close()` is idempotent and is refused, without a native call, while children
  are open. Destructors close leftovers in reverse order. Observed order:
  `DSClose`, `DevClose`, `IFClose`, `TLClose`, `GCCloseLib`, then the library's
  ELF destructor.
* Mandatory symbols are validated **before** any producer call. A `GCInitLib`
  failure never calls `GCCloseLib`. A second instance of one canonical file is
  refused while one is loaded or retained.
* If native close remains unresolved, or `GCCloseLib` fails, the producer is
  **tainted**: its library and any memory it may reference are retained (never
  `dlclose`d) for the rest of the process, and the file cannot be reloaded. A
  bounded leak is preferable to unmapping code a producer thread may still run.
  The probe reports retention explicitly.
* Buffers are consumer-allocated (`DSAnnounceBuffer`). A buffer whose
  `DSRevokeBuffer` fails is quarantined until conclusive stream close (otherwise
  process-lifetime retention). Frames are
  copied into a separate bounded lease pool (ADR 0002), so leases outlive stop,
  close and real unload (tested). Restart is refused while a lease is held. A full
  lease pool drops the frame and requeues the native buffer rather than
  overwriting held bytes. Announced plus lease memory is bounded before allocation.
* Cancellation: `request_stop()` latches cancellation and calls `EventKill`
  under the mutex protecting event publication and retirement. A blocked
  `EventGetData` returned `GC_ERR_ABORT` (tested
  under TSan). A production adapter should also bound each wait (section 4).
## 7. GenApi integration strategy

GenTL needs a separate GenApi implementation for every feature operation:

| Option | Assessment |
| --- | --- |
| EMVA reference GenApi 3.5.1 runtime/SDK (same 2026.07 package) | Standard and producer-independent, but a binary-only C++ ABI (`gcc48` builds) with GenICam licence and redistribution conditions. The project already carries eBUS's GenApi 3.4 and its [known lock-order issue](../genapi-tsan-lock-order.md); two GenApi versions in one process need namespace isolation. Best long-term option; needs a licence and packaging decision. |
| Aravis 0.8.36 `ArvGc` over a custom `ArvDevice` subclass | `ArvDeviceClass` is public and derivable (`read_memory`/`write_memory` virtuals; `arv_gc_new(device, xml, size)`), so it reuses the already qualified Aravis dependency and needs no new licence. Aravis applies its GigE legacy-endianness heuristic only to `ArvGvDevice`, so a GenTL subclass bypasses it while per-register `<Endianess>` still applies. The Cognex XML is schema **1.0.1** with 937 BigEndian and 24 LittleEndian registers, which exercises exactly this path. **Not executed in this task.** |
| Producer-bundled GenApi (Balluff, Euresys) | Producer-specific and C++-ABI-coupled; defeats a generic consumer. Rejected. |

Recommendation: GIGE-MEL-GENTL-002 starts with **Aravis 0.8.36 `ArvGc` over a
GenTL-backed `ArvDevice`**, behind the existing `IDeviceFeatures` interface, and
treats the EMVA runtime as a separate licence/packaging decision. XML retrieval
(`Local:` URL parsing, raw read, zip detection) is implemented and tested; zip
extraction, `File:`/`http:` schemes and SHA-1 verification are not.

## 8. Raw port access and navigation limitations

Implemented: `Device::with_registers` lends the **existing**
`device_registers::IDeviceRegisters` through `CheckedRegisters`, so the shared
rules are unchanged: 1..4096-byte transfers, mod-4 address and length, the 32-bit
window and overflow checks, `WriteCompletion::not_attempted` before native entry
and `uncertain` after it, all-or-nothing reads, no retries, no rollback.

* **Byte order.** `GCReadPort`/`GCWritePort` move raw bytes. Words are converted
  only using the port's **declared** `PORT_INFO_LITTLE_ENDIAN` or
  `PORT_INFO_BIG_ENDIAN`. Undeclared or contradictory declarations refuse the
  service before any port call (tested both ways). Aravis's consumer passes words
  through unconverted, and the Aravis producer declares nothing at all.
* **Errors.** Native `GC_ERROR` codes and `GCGetLastError` text are preserved. A
  fixture `GCWritePort` that applied the bytes and then reported `GC_ERR_TIMEOUT`
  produced `uncertain` with `ErrorCode::timeout`, and the bytes were read back.
* **Not established (unqualified):** GenTL defines no timeout argument for port
  calls. A successful `GCWritePort` does not establish packet count, the absence
  of producer-internal splitting or retransmission, pending-ACK handling, bounded
  completion or sensor-side acceptance. Stacked writes are not atomic by
  specification. Any GVCP retry policy is a producer-specific node-map feature,
  if it is exposed at all.
* **NavigationReport:** GenTL can carry the bytes but cannot yet satisfy the
  production requirement of a guaranteed finite call bound and a no-retry wire
  policy, the same open gate as GIGE-MEL-037 for Aravis and Pleora.
  `INavigationSink` is not implemented.

## 9. Recommendation and incremental production tasks

**`CONDITIONAL_GO` for Approach B.** The native consumer foundation is small
(about 1.5 kLOC including its header), uses only the verified standard header, keeps every existing
contract, and maps onto the existing interfaces without changing camera profiles.
Two of three third-party producers loaded, initialized and closed cleanly
offline. The conditions:

1. **C1 — a real, full-featured producer must be qualified.** Gigelink 26.10
   needs an authenticated Euresys download. Balluff needs a third-party-hardware
   grant before it contacts the Cognex. The Aravis producer cannot stream.
2. **C2 — GenApi decision** (section 7), with one executed feature read through it.
3. **C3 — fault containment.** Two producers crashed on legal call sequences.
   Either pin and qualify producer versions, or run producer calls in a separate
   process. That process boundary is an architecture decision, not part of 002.
4. **C4 — navigation stays gated** exactly as in GIGE-MEL-037/038.

Proposed tasks, each with its own approval:

| Task | Scope | Acceptance |
| --- | --- | --- |
| GENTL-002 Backend foundation | Internal `gentl` `ICameraBackend` (explicit producer path, E1), composite IDs, `IDeviceFeatures` via `ArvGc`-over-GenTL, read-only open/inspect; not in the default catalog | Fixture + Cognex profile `open` through the fixture's GenApi XML; offline only |
| GENTL-003 Producer qualification | Read-only open/inspect of the CIC-5000R through Gigelink (and Balluff if licensed) | Identity, schema, port byte order, XML SHA-1 and feature readback match Aravis/Pleora evidence |
| GENTL-004 Stream | `IFrameStream` with sliced waits, PayloadSize/AcquisitionStart, timestamps invalid | Fixture contracts; then authorized Mono8 10 Hz with counts, stop/reopen, lease retention |
| GENTL-005 Selection opt-in | Explicit `gentl` catalog entry plus module bit E3, behind a default-OFF option | Selection contracts; no factory change |

**Same Cognex profile across Aravis, Pleora and GenTL:** *plausible, not
demonstrated.* The profile depends only on `IDeviceFeatures` and
`NativeFrameFacts`, which GenTL can supply with no profile change, provided the
GenApi layer reproduces the same node semantics (schema 1.0.1 big-endian
registers, `AcquisitionFrameRateAbs` and `ExposureTimeAbs` units) and the producer
exposes the remote device unmodified. No GenTL path has executed the profile yet.
