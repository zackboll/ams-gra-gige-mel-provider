# Controlled GVCP feasibility transport (GIGE-MEL-038)

GIGE-MEL-039 adds an independent [single-owner session](gvcp-single-owner.md)
above this unchanged client: fixture-only explicit acquisition, shared-socket
heartbeat, fenced uncertainty and joined shutdown. Physical SDK ownership and
cross-session stale packets remain gates; see its SDK feasibility decision.

**Experimental loopback artifact, not a production device service.** The existing
[GIGE-MEL-037 service](device-register-transport.md) remains unchanged. No SDK,
MEL, profile ABI v1, Cognex policy, navigation sink, frame timestamp, acquisition
or heartbeat behavior changes. No physical camera was contacted.

## Build and source boundary

The uninstalled `gige_mel_controlled_gvcp` static target contains project-owned
C++20/Linux UDP code and no SDK dependencies. Only its tests link it; neither
the provider nor native adapters do. Normal offline CMake builds need no new
dependency. Internal sources:

* [Interface](../src/transport/controlled_gvcp.hpp)
* [Transaction engine](../src/transport/controlled_gvcp.cpp)
* [Wire parser](../src/transport/gvcp_wire.hpp)
* [Independent UDP device and tests](../tests/controlled_gvcp_tests.cpp)
* [Pre-implementation design](tasks/GIGE-MEL-038-design.md)
* [Qualification record](tasks/GIGE-MEL-038-validation.md)

Run `ctest --test-dir build/dev -R controlled_gvcp -V` after the ordinary dev
configure/build. Tests bind only 127.0.0.1 ephemeral ports. The client rejects
non-numeric IPv4, non-127/8, port zero and 3956 (avoids accidentally addressing
the old Aravis simulator). No destination discovery, DNS, device open, ownership
register, control request, broadcast, wildcard bind, SDK or detached worker exists.

## Transaction contract

`IControlledRegisters` is separate from both installed interfaces and
`IRegisterDevice`. It offers numeric 32-bit register read/write and exact-byte
memory read/write. Each call requires a finite absolute `steady_clock` deadline
and accepts a stop token. Address and size must be mod four, size nonzero and
at most 508 bytes, with the entire interval in the 32-bit address window.
Validation failures send nothing. Memory reads update caller storage only after
complete success. Host spans require valid live storage throughout the call.

The maximum packet is 8 header + 4 address + 508 data = 520 UDP payload bytes,
548 including IPv4/UDP headers. This stays below a conservative 1500-byte MTU
envelope (the actual Linux loopback MTU is normally larger). A 336-byte payload
is one command. No fragmentation into multiple GVCP commands, tail padding,
4 KiB assumption or fallback to register writes. Numeric words are network
byte order; raw memory bytes are copied unchanged, independently of word order.

The engine owns exactly one nonblocking, close-on-exec UDP socket, bound to
127.0.0.1. A try-lock covers the entire ID/send/receive transaction. Concurrent
callers return typed `busy` immediately, rather than queue outside their budgets.
The client creates no thread. Caller lifetime rules: keep it alive through every
call and join callers before destruction; destruction closes its descriptor.
Cancellation is cooperative, not safe concurrent destruction.

### Deadline, cancellation and transmission policy

There is exactly one nonblocking `sendto` attempt per admitted transaction, for
reads **and** writes. Even EAGAIN/EINTR on send returns I/O failure, never a retry.
A successful send records `transmitted=true`: bytes were admitted to the local
kernel, not proven received by a device. No rollback, Abort, retry, discovery or
control command is generated on failure.

The original deadline is checked before transmission and before/after polling,
receiving and parsing. All receives are nonblocking. `poll` uses the lesser of
remaining budget (rounded up to milliseconds) and 2 ms; the deadline is checked
again before accepting an ACK. EINTR, stale traffic, malformed packets, pending
ACKs and advertised intervals never reset that deadline. Stop is checked at each
loop and before sending. Cancellation between the last check and the send may
race with transmission; the result reports that transmission truthfully.

The measured behavior establishes bounded **user-space waits under controlled
scheduling**, not a universal hard real-time bound. Scheduler stalls, allocation,
kernel execution and caller scheduling are not hard real-time qualified. No
thread is detached to conceal native work. No blocking native SDK call is wrapped.

Failures have typed categories: invalid argument, busy, timeout, cancelled, I/O,
device error, exhausted IDs. They include transmitted state, write completion and
the original known device status when applicable. Write success is
`acknowledged`; pre-send failure is `not_attempted`; **every** post-send write
failure is `uncertain`, even a negative ACK, because transport does not establish
atomic device execution or rollback. A missing ACK does not mean rejection.

### Packet validation and stale responses

Reference: Aravis 0.8.36 commit
[`97ff5e330919d7a44b871df4eda669feeafdf342`](https://github.com/AravisProject/aravis/tree/97ff5e330919d7a44b871df4eda669feeafdf342/src),
`arvgvcpprivate.h` and `arvgvcp.c`, studied without an SDK runtime dependency.
The engine supports only READREG/WRITEREG/READMEM/WRITEMEM commands 0x80/82/84/86
and their paired ACKs 0x81/83/85/87. Commands use key 0x42 and ACK-required flag 1.
Fields are explicit byte loads/stores, not unaligned native packet casts.

Accept only the configured sender IP **and port**, nonzero matching ID, matching
ACK family, success status 0, and exact header-declared and operation-specific
datagram lengths. Memory ACKs must echo the address; single-register write ACK
index/reserved word must be 1, as in the pinned simulator. READREG returns one
word; READMEM returns address plus exactly the requested bytes. `MSG_TRUNC`
detects datagrams larger than the receive buffer, which are discarded.

Known pinned negative statuses 0x8001..0x8017, generic 0x80ff and generic unknown
error type 0x8fff are accepted only as header-only error ACKs, with correlated
command and ID, and return an explicit device error. Unknown/malformed values,
unexpected error payloads and all other packet types are ignored, not interpreted
as success. This is a deliberately narrow supported error envelope; unqualified
extensions time out rather than being assigned an invented meaning.

Pending ACK 0x89 requires matching ID, success status and exactly four bytes of
payload. Its interval is not trusted as execution permission or additional time.
All ignored or pending packets remain within the original budget; no drain loop
can run without checking cancellation/deadline.

IDs are issued once, 1..65535, under the same mutex. They never wrap. Exhaustion
permanently refuses further sends on that client. Late ACKs and duplicates from
completed operations cannot satisfy subsequent IDs on the same socket. Unsolicited
queued packets are validated when a later transaction receives them. No background
receiver runs after return. **Cross-client OS port reuse is not session fencing**:
production recreation requires a qualified stale-packet/session strategy, not
blindly cycling clients after exhaustion. Loopback tests prove same-client safety,
not arbitrary old-network-packet lifetime bounds or authenticated peer identity.

## Native camera ownership investigation

### Evidence and limits

Aravis `arvgvdevice.c` owns a control socket, `packet_id`, I/O mutex and receive
buffer. Construction loads GenICam, takes control and starts its heartbeat. The
heartbeat uses the same register transport; failed privilege checks set
`is_controller=false` and emit control loss. Finalization cancels/joins heartbeat,
leaves control and retires the socket. Its ordinary I/O retry loop and pending
ACK deadline reset do not satisfy this prototype's policy. Adapter operation
locks cannot synchronize a second socket with SDK-internal heartbeat by themselves.
The Fake simulator even compares controller IP **and port**; its behavior is not
a specification for real controller identity or same-host multi-socket admission.

Available eBUS 6.5.3.7155 declarations expose `Connect(..., PvAccessType)`,
`GetAccessType`, direct register/memory calls, communication parameters, disconnect,
heartbeat priority and link events. `PvDeviceEnums.h` documents control mode as
one read/write main controller with other devices read-only; exclusive denies
other requests except discovery/resend. Direct APIs expose no per-call deadline,
stop token, owned-socket transfer or shared-ID/receive hook. Memory access bypasses
GenICam caches; WriteMemory can fall back to multiple register commands. Public
declarations do not establish internal socket/ID/locking rules or safe raw socket
coexistence. Proprietary SDK implementations were not inspected or reproduced.

| Architecture | Aravis | Pleora | Assessment |
| --- | --- | --- | --- |
| A: sideband socket while SDK owns control | No public coordinated control-socket/ID hook demonstrated; heartbeat/control-loss/teardown races | No documented sideband read/write controller permission or delegated transport | **Unproven and unsafe to deploy by assumption for both**; loopback cannot qualify it |
| B: shared/owned command boundary | Requires an upstream-supported hook or maintained SDK transport changes covering GenICam, bootstrap, heartbeat, configuration and stream-control commands, not just raw APIs | Requires vendor-supported transport delegation/deadline hooks or different SDK architecture; public direct methods insufficient | Most direct semantic solution if both SDKs support it; no such common hook established |
| C: isolated owner/component or process | Could host coordinated control, heartbeat and commands with a qualified SDK hook or passive stream arrangement | Same, plus vendor-supported read-only/stream-only attachment and cache/lifecycle rules | Most realistic **next architecture investigation**, but process isolation alone does not grant ownership or one-transmission semantics |

Recommendation: investigate C as the single lifecycle/command authority, with B
inside that owner wherever a native SDK remains involved. All control-affecting
paths must serialize through the authority: acquisition start/stop, stream
destination, configuration, raw writes, heartbeat and privilege release. A process
boundary can contain a blocking SDK failure and bound a caller's wait, but killing
it leaves transmitted writes uncertain and may leave privilege until heartbeat
expiry. It cannot make SDK-internal retries satisfy one-transmission policy.
An external controlled owner plus SDK passive receiver is only a hypothesis until
both SDKs and the target device explicitly support that arrangement.

Before integration, qualify controller identity/access modes on each SDK/device,
exclusive access, heartbeat scheduling under command load, shared ID/receive
ownership, cache invalidation, operation-lock ordering, acquisition interaction,
disconnect/control-loss invalidation, stale sessions and shutdown ordering.
Shutdown must stop admission, cancel/drain operations, stop acquisition, coordinate
stream retirement and release control exactly once without racing SDK heartbeat.
No SDK changes, sideband physical socket or claim of supported coexistence is made
here. Ownership compatibility remains a **production-integration blocker**, not
a failed requirement of this independent loopback feasibility artifact.

## Future navigation consumption (not implemented)

```text
MEL NavigationReport
  -> existing validated project-owned NavigationReport
  -> optional camera-profile-owned navigation handler
  -> sensor-specific serialization and transaction policy
  -> controlled register transport
  -> sensor staging / commit / acknowledgement
```

MEL acceptance, transport ACK, sensor staging completion, sensor report acceptance
and transfer-alignment processing are separate states. A valid GVCP ACK establishes
only the transport boundary. It does not prove navigation processing or alignment.
The current MEL response semantics and optional sink behavior are unchanged.

A later profile needs sensor-defined session/generation and report/commit identity,
duplicate handling and idempotence, staging integrity, explicit acceptance query,
timeout policy and a bounded whole-report budget (not a fresh full budget per field).
An unresolved payload/Commit write must fence the session or reconcile via a
qualified sensor query before any dependent submission. Do not infer rejection,
rollback or safe retry. A session/control-loss change invalidates in-flight state;
duplicate Commit must be rejected or resolved by sensor identity, never blindly
resent. Abort also requires its own qualified effects and uncertainty policy.

The separate experimental pleora-navigation-receiver host/receiver work was
reviewed for context. Its report/CRC-commit sequence lacks a qualified production
session/duplicate protocol; its 320-byte encoding and addresses are not adopted.
The 336-byte pattern here is opaque transport evidence, **not** navigation wire
format. No Begin/Commit/Abort protocol or automatic application retry exists here.
