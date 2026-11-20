# Internal device register and memory transport (GIGE-MEL-037)

## Boundary, ownership and discovery

[`device_registers.hpp`](../src/device_registers.hpp) is an uninstalled C++20
source interface using project-owned values only. Explicitly opened Aravis and
Pleora devices implement `device_registers::IRegisterDevice` alongside the
unchanged installed `ICameraDevice`. Discover with `dynamic_cast`; absence means
unavailable. Discovery, selection and service destruction perform no I/O.

`with_registers(callback)` lends `IDeviceRegisters&` synchronously. The adapter
holds its existing native-device operation mutex from admission through callback
return, including exception unwinding. The callback must not reenter device,
provider or consumer methods, wait for work needing that lock, or retain the
service. The caller keeps its owning device alive through return. Existing
adapter core/state owns the native device; close cannot retire it during the
callback. No new device, native pointer, worker or queue crosses this boundary.

A future profile handler may receive this borrowed reference through a separately
designed invocation boundary. No profile interface, module v1 descriptor or
matched-build compatibility identity changes: their interface closure is intact.
Dynamic profile states must never retain borrowed services or native pointers.

## Operations and validation

| Operation | Meaning |
| --- | --- |
| `read_register(uint64_t)` | One numeric host-side uint32 value, only on success |
| `write_register(uint64_t, uint32_t)` | Numeric word; SDK owns register conversion |
| `read_memory(uint64_t, span<byte>)` | Exact bytes, caller updated only after complete success |
| `write_memory(uint64_t, span<const byte>)` | Exact bytes, no swapping, padding or encoding |

Every operation validates before native entry. Addresses and memory lengths are
multiples of four; memory length is nonzero and at most 4096 bytes. Check address
overflow before computing the last byte. The whole interval must fit
`0..UINT32_MAX`, the common qualified GigE Vision window. Aravis accepts guint64
but its pinned GVCP construction narrows addresses to uint32. Pleora takes
signed int64 addresses/lengths; all casts follow the shared narrower-window check.
Higher addresses are rejected, not truncated or advertised as extended support.

Installed Pleora PvDevice.h explicitly requires mod-four addresses and nonzero
mod-four lengths. Aravis rounds GVCP lengths to words; refusing tails prevents
unintended padding/access. Host byte pointers need no word alignment. Valid live
storage is a C++ span precondition; arbitrary dangling pointers cannot be
diagnosed. Empty spans reject without dereferencing. Reads use a bounded stack
temporary and never expose partial native output as successful data.

Sensor addresses, encoding, wire byte order, Begin/Commit/Abort, CRC and sensor
acknowledgement belong to a separately qualified camera protocol. This service
does not serialize NavigationReport, implement INavigationSink, change MEL
admission/responses or timestamps, or associate navigation with frames.

## Errors and write completion

Transport-local Result/Status retain backend Error in `Failure::diagnostic`:

* `WriteCompletion::not_attempted`: validation, closed/disconnected admission or
  another refusal before the native write call. No service write was attempted
  for that operation, not necessarily for earlier operations in the callback.
* `WriteCompletion::uncertain`: native write entered and failed. The device may
  have received none, some or all. Even SDK unsupported failures are conservative:
  a memory operation may contain multiple native transactions. Timeout is never
  definite rejection. Earlier successful operations are not rolled back.

Read failures produce no register value and leave caller memory untouched; their
write classification is not_attempted. Callback code must propagate errors; the
wrapper cannot infer success for ignored failures. Native exceptions propagate
with lock unwinding; exceptions after write entry may also have side effects.

Aravis retains original GError domain, numeric code, text and classification;
native NOT_IMPLEMENTED maps to unsupported. FALSE without GError becomes explicit
I/O error, never fabricated success. Pleora uses existing PvResult translation,
retaining numeric code, code string and description. Connection/control loss is
checked before every service call. There are no service retries or rollback.
Successful SDK return establishes transport completion only, not sensor processing.

## Streaming, close, cancellation and boundedness

All four methods may execute while streaming. The service changes no acquisition,
configuration, feature cache, stream, frame lease or callback behavior. Raw writes
can of course alter camera state: a future sensor profile must choose safe
addresses and must not change cached GenApi configuration behind existing policy.

The entire callback excludes configuration, acquisition transitions, stop and
close. Aravis receive may overlap; Pleora retains existing shared state/buffer
synchronization. Receive cancellation/pool/lease behavior is unchanged. Logical
close rejects subsequent callbacks; link loss observed between calls refuses
the next call without dereferencing retired state.

**Pinned-SDK qualification limitation:** no hard service deadline or in-flight
cancellation API exists. Callbacks must bound their operation count and work;
close waits for admitted transactions. Aravis 0.8.36 privately retries GVCP and
accepts pending-ACK extensions, without public per-call controls. Memory calls
split into blocks. Pleora direct APIs have no timeout argument and document
WriteMemory fallback to multiple register writes. One SDK call is NOT one packet,
atomic delivery, proof of no retransmission, or a hard wall-clock upper bound.
This implementation adds no retries but cannot promise those properties. Do not
compose into a production bounded INavigationSink until these gates are resolved.
No global SDK communication/acquisition policy was changed to conceal this.
The opt-in [control-authority experiment](tasks/GIGE-MEL-046-design.md) does not
admit this service or wrap these calls in a timed future:
`NATIVE_SDK_INTEGRATION_NOT_QUALIFIED`.

### Development-only merge scope versus production qualification

The service controls one application-level SDK call per valid operation, not
SDK-internal packet splitting/retransmission or sensor protocol acceptance.
The pinned public APIs do not establish a hard completion deadline. In particular,
this service **cannot yet satisfy a production INavigationSink requirement for a
guaranteed finite call bound**; limiting callback operation count is insufficient.

These limits can technically be deferred for an explicitly development-only
transport-foundation merge: the service is internal, no navigation sink is added,
and installed MEL/profile capabilities remain unchanged. That scope requires
explicit maintainer acceptance; it is not granted by passing fixture tests or
this document. Until accepted, disposition remains QUALIFICATION_GAP. If strict
no-wire-retry and hard execution bounds are mandatory for PR #31, it stays blocked.
A separate transport architecture must provide explicit wire-attempt policy,
bounded pending-ACK handling, deadline/cancellation and ownership-safe teardown
(or a suitably controlled SDK), then qualify those properties independently.
Sensor encoding/acknowledgement needs separate protocol and hardware acceptance.
No unsupported SDK timeout/retry argument or global policy change is introduced.

Sources: pinned [Aravis implementation](https://github.com/AravisProject/aravis/blob/0.8.36/src/arvgvdevice.c),
[GVCP construction](https://github.com/AravisProject/aravis/blob/0.8.36/src/arvgvcp.c),
and installed eBUS 6.5.3.7155 PvDeviceGEV.h/PvDevice.h. Proprietary headers remain
external and are not reproduced.

## Evidence boundaries

See [design](tasks/GIGE-MEL-037-design.md) and
[qualification](tasks/GIGE-MEL-037-validation.md). Recording tests prove shared
validation, non-symmetric byte patterns, all-or-nothing reads, original errors,
no service retries/rollback and transaction ordering. Actual Aravis Fake APIs
prove numeric conversion, bytes, native refusal without GError, concurrent close
and confirmed control loss. Existing loopback additionally executes 4 KiB raw
transfers through production ArvGvDevice/GVCP and register access while receiving
GVSP frames, with existing simulator wildcard-listener limitations unchanged.

Pleora compiles the narrow production-call template against actual installed
declarations. Recording calls verify address/size arguments, acknowledge=true,
bytes, real PvResult translation and uncertain writes. Actual disconnected
PvDeviceGEV calls in `pleora_sdk_contract` verify genuine refusal/cleanup without
discovery; `pleora_register_contract` retains all recording boundary checks with
ordinary unsuppressed sanitizer behavior. The existing SDK-process audit requires
completion of the disconnected register checks in both phases, as well as all
eight original groups. See [R1 investigation](tasks/GIGE-MEL-037-R1.md). This does
not qualify connected-device concurrency, camera acceptance, packetization or
SDK-internal retry policy. Existing GenApi and buffer tests remain enabled.
No physical camera was contacted, configured or captured.
