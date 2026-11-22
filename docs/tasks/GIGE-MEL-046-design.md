# GIGE-MEL-046 — Control authority and bounded completion

**Disposition: `NATIVE_SDK_INTEGRATION_NOT_QUALIFIED`.** This experiment is not
installed, is not a camera backend, and does not transmit navigation data.
See the [validation record](GIGE-MEL-046-validation.md) for executed evidence.

## Baseline audit

Baseline: freshly fetched `origin/main` at `3cc38e86c5c4c7f2359237565ece3eeeb53f5831`,
the merge of PR #58. Below, source observations describe how the code is wired;
historical tests are scoped to their original fixtures. Neither is a substitute
for a physical or bounded-execution qualification.

| Surface | Source-derived ownership, serialization and shutdown | Tested scope / absent guarantee |
| --- | --- | --- |
| [`device_registers`](../../src/device_registers.hpp) | `IRegisterDevice::with_registers` synchronously lends a checked service under the adapter lock. `CheckedRegisters` validates region/alignment and connection per call, stages reads, does not retry or roll back; native write failure is uncertain. The callback owns neither controller nor device and must not reenter or retain the borrow. Close waits for callback return. | `device_register_tests` records no native entry on refusal, numeric/byte and all-or-nothing reads, write diagnostics, serialization and close ordering in a model. Arbitrary callback work, native execution, SDK retries and device-side effects are not bounded by this interface. |
| [`Aravis`](../../src/backends/aravis/aravis_backend.cpp) | Exact discovery/open and in-process reservation wrap the native `ArvDevice`. `DeviceCore::mutex` serializes adapter feature/raw I/O, acquisition transitions and close; stream wait/leases have separate lifetime locks. Synchronous `arv_device_*_register/memory` take no deadline/token. The SDK's private `ArvGvDevice` owns its socket, request mutex/IDs, control, heartbeat, retries and pending-ACK behavior; the adapter mutex does not govern that heartbeat. `control-lost` latches an atomic terminal flag used by later admissions. Close retires the run/AcquisitionStop and waits for stream activity, disconnects/drains the signal emission, then drops native refs; SDK finalization stops/joins heartbeat and releases control. | Historical Fake/loopback cases exercise operations, close and signal loss. No absolute native command bound or finite cancellation/close proof; drainage itself has no operation deadline. Stream receive cancellation is not control cancellation. SDK behavior is source-derived for pinned 0.8.36 ([upstream audit](../gvcp-single-owner.md#sdk-compatibility-evidence)). |
| [`Pleora`](../../src/backends/pleora/pleora_backend.cpp) | Reservation keyed by MAC is process-local; `PvDeviceGEV::Connect(..., PvAccessExclusive)` is the native owner path. `State::mutex` covers adapter control/feature I/O and borrowed register callbacks; the SDK owns command processing, retries and heartbeat. `IsConnected` and the latched `OnLinkDisconnected` flag govern later admission, not the completion of a pending write. Setup uses `SetStreamDestination`, `StreamEnable` and GenApi `AcquisitionStart`. Close runs `AcquisitionStop`/`StreamDisable` as applicable, stops/closes pipeline/stream, unregisters the sink, disconnects and destroys the device before releasing reservation. | Historical recording/disconnected SDK cases cover argument mapping and refusal/cleanup, not licensed connected control lifecycles. Public direct operations have no absolute deadline/token; SDK answer timeout/retry configuration is not one-operation cancellation or one wire attempt. Header/docs describe potential write-memory fallback. SDK internals are not independently measured here. |
| [`GenTL`](../../src/gentl/gentl_consumer.cpp) | `IFOpenDevice` passes requested access to the producer. Consumer `Device::mutex` guards port I/O/lifetime; `GCReadPort`/`GCWritePort` are synchronous producer calls with byte counts, not deadlines. `EventGetData` timeout/`EventKill` govern stream waits only. Child-before-parent close protects live handles; stream teardown can taint the loaded producer on uncertainty. The optional [backend](../../src/backends/gentl/gentl_backend.cpp) remains read-only and not in the production catalog. | Consumer tests use fixture producers. Controller identity, actual wire serialization, heartbeat, retry behavior and finite port/close completion depend on the CTI and are absent as qualified guarantees. Neither a requested access mode nor handle closure proves physical fencing. |
| [`Provider`](../../src/provider.cpp), [`INavigationSink`](../../include/gige_mel/backend.hpp) | The runtime operation lock pins the attached device and serializes validation/admission, backend submission when supplied, and accepted-report commit. Sink mutation reentry is prohibited; consumer response callback follows outside the lock. Detach serializes with submission and drains callbacks separately. | Provider contracts test recorded forwarding/receipt and lifetime ordering. The finite synchronous sink bound is a **requirement on the implementation**, not a timer the provider enforces. No native sink implements it. |
| [`Client`](../../src/transport/controlled_gvcp.cpp), [`wire`](../../src/transport/gvcp_wire.hpp) | One nonblocking loopback UDP socket and try-lock for the transaction, one send attempt, IDs 1–65535 then exhaustion, 2 ms maximum poll request (rounded within budget), checks around send/poll/receive/parse. Source endpoint, ID, ACK type/shape and applicable memory address are checked. Pending responses never extend the original deadline. No thread or claim; callers drain before socket destruction. | GIGE-MEL-038 parser/loopback tests cover correlation, pending/late traffic, cancellation, one-send/uncertain writes and cleanup. Endpoint matching is not authentication or across-session fencing. |
| [`ControlOwner`](../../src/transport/gvcp_control_owner.cpp) | Explicit fixture claim probes, privilege write and endpoint identity readback. One mutex and one private client across acquisition, data, scheduled heartbeat and release; local lease caps operations. Loss, ambiguous command failures and failed heartbeat readback fence; ordinary data-command pre-send timeout/cancellation alone does not. Shutdown closes admission/cancels, stops and joins heartbeat, then attempts one bounded release only with still-valid owned evidence. Socket close alone does not prove release. | GIGE-MEL-039 fixture tests cover competition, lease/loss, applied writes with missing/negative ACK, release uncertainty, exhaustion and cleanup. Marker and endpoint identity are fixture-only, not an epoch. Native coexistence and packet lifetime remain open. |

## Selected architecture and ownership

Use a synchronous **single-slot `Authority`** (no queue, arbitrary callback, or
worker). `Command` owns a small opaque fixture value; it defines no sensor opcodes
or registers. `OperationContext` owns the generation, absolute steady-clock
deadline and caller/session tokens; it lives for the invocation only. The narrow
`Executor` returns `Evidence` (pre-send proof, possible effect, or transport ACK,
plus echoed generation and qualified loss). This is intentionally not a general
register layer. Existing register and GVCP result types remain intact.

An authority adopts exactly one exclusively held executor and nonzero generation
via the test-only grant; only that authority can call the admitted executor on
its session. There is no production grant, native proxy, socket creation, or
rebind. Default construction has no session and returns `unsupported`. A future
grant must establish exclusivity at the endpoint and project level, not just mint
a local number. A new authority is not permission to repeat or build on effects
from an unresolved one. This experiment has no recovery/reconciliation API.

All control-affecting work in a future qualified session must be coordinated by
the same command authority, including acquisition start/stop, destination and
packet/stream configuration, cache-affecting GenICam I/O, heartbeat and privilege
release. No second controller may be opened beside an SDK owner, and process
isolation alone cannot prove release or rule out retries. Native SDK heartbeat,
socket, GenICam/cache policy and acquisition/stream teardown remain untouched.
Raw out-of-band writes can invalidate cached feature and payload state; a future
design needs cache rules and stream/lifecycle coordination, not an assumed
passive SDK stream. No such arrangement is qualified by the traffic audits.

### State, linearization and retirement

* One mutex protects phase, loss and uncertainty latches, and the execution and
  retirement slots. Admission and result publication each linearize under that
  mutex. While one caller executes, competing and recursive submissions are
  refused without queuing. Neither executor code nor its destructor runs under
  that mutex. Tests use reentry only as a negative control; qualified work may
  not wait for provider reentry, another submission, or shutdown.
* The independent latches `lost` and `unresolved` never clear. A matching control
  loss notification cancels the session; later submissions cannot touch its
  executor. Notifications for another generation are ignored. A loss that wins
  before result publication prevents success, while any possible effect survives
  in `Result::unresolved`. Post-publication loss does not retroactively change a
  returned ACK. An unresolved operation fences **all** further work (including
  dependent writes); an ordinary pre-send refusal does not poison the session.
* `unavailable/open → closing → retired`: shutdown closes admission before
  requesting private cancellation. It waits for the admitted call to leave the
  executor, retires the executor outside the lock, then publishes retirement.
  Waiters release the mutex while draining. Concurrent shutdown observes the same
  retirement; same-thread execution/retirement reentry is refused (no self-wait).
  Owner lifetime must span all external calls: join caller threads before
  destruction. There is no authority-owned worker, no detached work, no retry
  from shutdown, and no implication that retirement proves remote release.

| Outcome | Evidence and disposition |
| --- | --- |
| `not_attempted` | Admission refused (e.g. busy, wrong generation, already fenced, expired/cancelled) or executor proves external transport was not entered. A separate sticky flag can still refer to a previous unresolved operation. |
| `acknowledged` | Expected correlated confirmation with matching endpoint generation, published while live within deadline and without observed cancellation. Not proof of detector processing. |
| `uncertain` | External effects cannot be excluded: lost ACK, cancellation/deadline after possible send, stale/missing epoch, shutdown race, or unexpected exception. No retry or implied rejection/rollback. |
| `control_lost` | Terminal invalid authority. May also have an unresolved effect; keep both facts. |
| `unsupported` | No qualified transport admitted. In particular, a native register operation is not attempted. |

### Finite-completion admission gate

Before adding any production grant, require reviewable evidence for all of:

1. Absolute deadline preserved through every wait, transmission and response
   decision, not checked only around a blocking call.
2. Finite cancellation and retirement/cleanup under a stated execution and
   scheduling model, including their failure paths. No caller timeout followed
   by hidden native work.
3. No retained context, handle or worker accessing retired device state; drain
   and join evidence for every asynchronous resource, if any.
4. Documented retry, pending-response and matching policy with no unqualified
   duplicate side effects or deadline extension.
5. Conservative possible-effects evidence and fencing on ownership/effect
   ambiguity; qualified new-session/reconciliation policy.
6. Completion without provider or consumer reentry.

The fixture uses bounded condition-variable rendezvous, checks both cancellation
tokens and polls at most 2 ms per requested wait, capped by the original deadline.
It applies one modeled command, never retries, and leaves no outstanding worker
or context. The loopback client separately exercises real UDP deadline/cancellation;
it is **not** admitted as the executor. Neither model gives a hard real-time OS
bound. Draining necessarily depends on the qualified executor returning (and its
destructor finishing). An unbounded native call in a worker with a timed future
would only hide this dependency and is explicitly excluded.

## Cross-session stale ACKs

The loopback reproduction forces port reuse after A shuts down. A's withheld
write ACK matches B's first request (same endpoints, ID 1 and ACK shape) although
B itself was not applied. This is a negative result for ordinary GVCP fencing.
The separate synthetic fixture returns an old echoed generation (or none); the
authority refuses acknowledgement and preserves B's possible side effect. That
is a positive policy check for **declared fixture evidence**, not a camera feature.

| Candidate defense | Required evidence / limitation |
| --- | --- |
| Local session generation | Rejects stale callers/local notifications, but labeling an already matched UDP response with the current generation proves nothing about its origin. |
| Echoed protocol epoch or nonce | Could correlate a response across reuse if the endpoint binds it to the correct request/session and defines uniqueness/reset semantics. Ordinary GVCP ACK has no such automatic epoch. Fixture echo is not authentication or a production guarantee. |
| Unique local endpoint and bounded quarantine | Requires enforceable allocation/reuse rules, including restart, exhaustion and other processes; no qualified policy exists. |
| Bounded packet lifetime | Needs an enforceable maximum response lifetime (including delayed endpoint retries/generation) and binding reuse restrictions; closing a socket or waiting an arbitrary delay is not proof. |
| Device-side reconciliation / idempotent command identity | Needs authoritative, session-aware readback and duplicate semantics. ACK matching or generic register readback does not establish application-level effects, acceptance or safe replay. |

Physical ownership, on-wire generation/fencing (or another proven equivalent),
and uncertainty reconciliation remain requirements. Without them, the path does
not open. No new acquisition, wire protocol, installed API or MEL factory exists.

## Future navigation boundary (not implemented)

```text
MEL NavigationReport
  -> common provider validation
  -> existing optional INavigationSink
  -> future sensor-specific handler
  -> qualified control authority
  -> bounded, serialized transport
  -> detector-specific staging / commit / acceptance
```

Keep separate: MEL input accepted, backend accepted submission, transport command
acknowledged, sensor staged payload, sensor accepted the complete report,
navigation associated with a frame, and transfer alignment completed. A future
handler needs one whole-report budget, a device-defined encoding/order, acceptance
and identity contract, recovery and duplicate policy, and authorized hardware
evidence. No addresses, layout, CRC, commit/abort format, ACK or session token is
specified here. Frame association and alignment need their own evidence.