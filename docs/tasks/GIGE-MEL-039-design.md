# GIGE-MEL-039 pre-implementation design

Baseline: fetched `origin/main` commit `26e141eb11106ac680d5fea3eddd5ee82ff9486a`,
tree `0bc7cffd4bcbceb50e657637e897966a155d37ad`. PR #32 is merged.

## Boundary and evidence

Keep the 038 client unchanged. A new uninstalled owner privately owns exactly one
client and one joined scheduler. No native adapter, installed interface, profile,
navigation, physical endpoint or factory is connected. Endpoint validation retains
038's numeric loopback/ephemeral-port restrictions. Before claiming, require the
fixture protocol magic at `0xffff0000` (`0x534f0039`). This is an accidental-target
guard, not authentication. The fixture-only `0xffff0004` query returns 1 iff the
request source IP/port is the current controller. Neither query is a GigE Vision
bootstrap register. No physical-camera ownership inference is supported.

Pinned Aravis 0.8.36 `arvgvcpprivate.h` lines 122–124 defines privilege at
`0x00000a00`, control as `1 << 1`; `arv_gv_device_leave_control` writes zero.
The new fixture independently implements these literal values. It atomically
claims only when unowned and enforces source endpoint on writes/release. Only a
successful owner privilege read refreshes its lease, not ordinary I/O or identity
queries. Its monotonic expiry runs independently of delayed ACKs.

## Authority and states

`NotStarted -> ClaimingControl -> Owned`; failure ends in `NotOwned` (no claim
transmitted), `ControlLost` (successful readback demonstrating revocation or expired
local evidence), or `OutcomeUnresolved` (ambiguous command/heartbeat failure or
transmitted transition without definitive completion/identity). Negative device
ACKs alone never establish loss or prove an unapplied claim. No implicit reacquisition.
`Closing -> Closed` retires resources; a separate persistent close outcome retains
unresolved acquisition/release rather than laundering it into clean shutdown.

One owner mutex covers each complete operation and heartbeat; concurrent ordinary
calls return busy without transmission (no queue or reset admission budget).
Qualification refinement: permit a bounded 2 ms mutex admission window, capped by
the original deadline, so scheduler wake/relock bookkeeping does not spuriously
reject otherwise uncontended traffic. An active pending command still returns busy.
Caller cancellation and owner shutdown cancellation are combined. Every admitted
command uses the original absolute deadline, capped by a finite policy budget and
the conservative lease deadline. Acquisition's multi-command deadline never resets.
Application register/memory operations cannot overlap privilege or fixture identity
addresses. No client/native handle escapes the owner.

## Scheduling and retirement

One owned scheduler performs periodic privilege + fixture identity checks through
the same client/mutex/ID allocator. Lease evidence starts at the **send-side start**
of a successful refresh, not ACK arrival. Ordinary operations are capped by that
lease; delayed/pending traffic cannot postpone loss detection indefinitely. Missing
heartbeat completion fences the session, never retries. OS scheduling is not a
physical heartbeat guarantee. A busy ordinary transaction may consume the remaining
lease and deliberately fail the session instead of extending ownership.

Close first atomically stops admission and signals cancellation, stops/joins the
scheduler, drains the bounded operation, then releases once only if ownership
evidence remains valid. Release uses a finite separate cleanup budget capped by
the existing lease. Lost release ACK remains unresolved; never retry. Socket closes
after cleanup. Concurrent/repeated close is locally idempotent. Callers must retain
the owner through calls; destruction is not a substitute for joining external users.

## Qualification and SDK investigation plan

Independent byte-codec UDP server records endpoints, IDs, counts, side effects,
lease expiry and refusal. Exercise competing owners, transition ACK loss,
heartbeat/ordinary pending traffic, cancellation, concurrent close, stale packets,
real allocator exhaustion and resource retirement. Run all nine requested compiler/
SDK/sanitizer lanes serially, existing scoped GenApi audit and publication/package
checks. Record limitations rather than disabling regressions.

Aravis inspection already shows private `ArvGvDeviceIOData` socket/mutex/packet ID,
native heartbeat `_read_register`, construction-time take-control and finalize-time
release. Public stream creation requires `ArvGvDevice`; stream construction itself
reads/writes features and teardown writes channel registers. Investigate precise
public passive support rather than treating the private stream constructor as it.
Pleora headers expose independent `PvStreamGEV::Open` and explicit multicast slave
documentation, but also GVCP reachability/resend traffic. Investigate that distinction
and public transport delegation before recommending an integration path.

Cross-session local port reuse can admit an old ACK after IDs restart. Neither
endpoint matching nor the fixture endpoint identity solves that. Do not recreate
the client on exhaustion or replay uncertain work. A production owner needs a
sensor-defined generation/token, provable packet lifetime/port quarantine, or
another justified lifetime mechanism; none is claimed here.