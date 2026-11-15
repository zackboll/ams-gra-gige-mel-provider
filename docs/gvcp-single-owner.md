# Experimental single-owner GVCP session (GIGE-MEL-039)

**Loopback feasibility only. No physical-camera or production integration.**
The uninstalled [owner](../src/transport/gvcp_control_owner.hpp) uses the unchanged
[038 client](controlled-gvcp-transport.md). Only tests link its static target;
no SDK adapter, MEL factory, navigation sink or profile module exposes it.

## Authority and fixture model

Create validates a numeric 127/8 destination, nonzero port other than 3956, and
finite positive policy durations. It constructs exactly one client/socket but
does not transmit or claim. Explicit `acquire` reads fixture magic `0x534f0039`
at `0xffff0000`, reads privilege `0xa00`, refuses existing privilege, writes
control value 2, requires the write ACK, then queries fixture identity at
`0xffff0004`. Identity 1 means the request's source IP and UDP port match the
current fixture controller. This is a simulator extension, not a universal GigE
Vision identity mechanism or authentication protocol. Privilege address,
control bit 2 and release zero match pinned Aravis 0.8.36 wire definitions.

The [independent UDP device](../tests/gvcp_control_owner_tests.cpp) uses literal
wire values and its own codec. Atomic server-side acquisition—not the earlier
global read—decides competing claims. Every ordinary write and release checks
source identity. Only an owner privilege read refreshes the configured lease;
identity queries and ordinary reads/writes do not. Acquisition initially starts
the lease. Expiry continues while responses are pending/delayed. The server
records endpoint, command, request ID, acceptance and actual side effects, plus
claim/release/refusal/refresh/loss counts. Lease configuration is explicit on both
sides, not negotiated or inferred from hardware.

```text
NotStarted -> ClaimingControl -> Owned
                    |-> NotOwned (no claim transmitted; terminal)
                    |              |-> ControlLost
                    |              |-> OutcomeUnresolved
                    +--------------+
any live state -> Closing -> Closed + persistent CloseOutcome
```

Failed acquisition is terminal except cancellation/deadline refusal before any
acquisition procedure begins. No silent reacquisition, transition retry, socket
recreation, ID reset or automatic replay. A successful write ACK alone never
establishes owned state. A global privilege bit cannot identify a controller.
Closed means locally retired; unresolved close is **not** clean release.

`ControlLost` requires successful privilege/identity readback demonstrating loss,
or the defined local lease-expiry transition. A correlated negative device ACK
does not prove controller identity, privilege loss, or absence of side effects.
Ordinary command failures and failed heartbeat reads fence the session as
`OutcomeUnresolved`; the explicit heartbeat wrapper preserves that classification.
Any transmitted claim failure, including a negative ACK, is unresolved: the server
may have applied the claim. No retry, reacquisition or release follows uncertainty,
and repeated shutdown retains `unresolved`, not `control_lost` or `not_owned`.
Pre-claim failure (or a claim never transmitted) is terminal `NotOwned` with close
outcome `not_owned`, not a claim that previously established control was lost.

## Serialization, heartbeat and deadlines

The private client is never lent to callers. One mutex serializes whole acquisition,
ordinary register/memory transactions, heartbeat and cleanup. Concurrent ordinary
calls allow at most 2 ms for admission (capped by the caller's original deadline),
then return busy without transmission; there is no work queue. Each admitted command
retains 038's one-send policy and monotonically allocated nonzero ID. Ordinary
access overlapping privilege or the fixture identity window is rejected, including
memory-range overlap. No bypass API is exposed on the owner.

After acquisition, one owned `jthread` schedules privilege + identity checks on
that same client. No separate heartbeat socket or command engine. Heartbeat
failure fences the session. Success refreshes conservative local lease evidence
from before the privilege request, never ACK arrival. Operations use the minimum
of their original caller deadline, command-policy budget and local lease end.
Acquisition/heartbeat sequences share one unchanged absolute deadline. Pending
ACKs cannot extend it. Caller and shutdown stop tokens are combined.

An ordinary operation can occupy most of the remaining heartbeat budget. If it
does not complete, the session fails closed; heartbeat cannot repair uncertainty
behind its back. Idle scheduling continuity and pending ordinary I/O with actual
server expiry are separately tested. Invalid calls and pre-send cancellation
remain not-attempted; transmitted writes without definitive ACK remain uncertain,
with device side effects retained. Unexpected failures fence future work;
confirmed identity/privilege loss forbids release to another controller. Actual
owned-session exhaustion observes all 65535 request IDs without socket replacement.

## Retirement

Shutdown atomically closes admission and requests cancellation before waiting for
the operation mutex. It requests scheduler stop, wakes and joins it after bounded
in-flight work, then attempts one release only with still-valid owned evidence.
Cleanup has a finite separate deadline capped by the existing lease. Release ACK
loss stays unresolved; repeated/concurrent shutdown cannot retry. Finally it
destroys the client/socket and records Closed. Destruction invokes this path and
contains teardown exceptions. External callers must keep the owner alive through
calls and join before destruction; concurrent shutdown is supported, use after
destruction is not. No detached thread or unbounded queue exists.

These are cooperative user-space deadline/poll bounds, not hard-real-time OS
scheduling guarantees. Cancellation cannot undo effects. Expired evidence, lost
acquisition or missing release ACK cannot prove remote release because local
descriptors closed.

## Cross-session stale ACKs and recovery

038 never reuses IDs within a client; the owner never replaces it. However, a new
UDP socket may receive the old OS-assigned local port and restart IDs at 1. An old
ACK can then match endpoint, command and ID of a new request; register ACKs do
not echo address. Fixture identity also identifies only the reused endpoint,
not a session epoch. Magic is constant and cannot fence old sessions. Cross-session
fencing is **not solved**, and uncertain operations are never replayed.

Without sensor-defined identity/idempotence/reconciliation, readback cannot prove
which generation owns control, whether an uncertain write applied, or whether a
report/commit was processed. Production needs a provable generation/lifetime
mechanism or justified packet-lifetime/port quarantine. The stale/unknown tests
qualify rejection within one non-reusing session, not across port reuse.

## SDK compatibility evidence

### Aravis 0.8.36

Inspected pinned upstream:
[device](https://github.com/AravisProject/aravis/blob/0.8.36/src/arvgvdevice.c),
[stream](https://github.com/AravisProject/aravis/blob/0.8.36/src/arvgvstream.c),
[public header](https://github.com/AravisProject/aravis/blob/0.8.36/src/arvgvstream.h),
[wire definitions](https://github.com/AravisProject/aravis/blob/0.8.36/src/arvgvcpprivate.h).

* Private `ArvGvDeviceIOData` owns control socket, mutex and packet ID.
  `_send_cmd_and_receive_ack` locks/allocates IDs (179–181), with native retry
  and pending policy distinct from this experiment.
* Native heartbeat calls `_read_register` through that engine (442–500), not a
  public external-heartbeat delegation seam.
* Construction loads bootstrap/GenICam, takes control (2049), starts heartbeat.
  Finalize cancels/joins heartbeat, releases, closes socket (2092–2136). Public
  take/leave-control do not transfer socket or allocator.
* Public stream creation refuses non-controller state (1874–1879).
  `arv_gv_stream_new` is private, absent from the public header. Construction
  reads channel, timestamp frequency, packet size, interface/source stream port;
  writes packet size if needed and destination/host port (1590–1675). Teardown
  writes stream-channel register (1730).
* Public packet-resend NEVER does not remove ownership, bootstrap accesses,
  destination writes or teardown writes. Resends themselves use stream transport.

No supported public independent passive GVSP receiver without `ArvGvDevice`, nor
control socket/transport delegation, was established. A generic `ArvDevice`
subclass does not make final native `ArvGvStream` accept another owner. Maintained
SDK changes/new supported receiver APIs would need to separate configuration,
destination, teardown and resend authority. No SDK is modified here.

### Pleora eBUS 6.5.3.7155

Inspected installed declarations and shipped documentation/examples; proprietary
header contents are not copied into this repository.

| Question | Exact API evidence and limitation |
| --- | --- |
| Stream-only connection | `PvStreamGEV::Open(const PvString&, uint16_t, uint16_t, const PvString&, uint32_t)` and multicast overload taking address/data port need no PvDevice. Shipped MulticastSlave example opens a stream, not control. |
| Device controlled elsewhere | Multicast Open docs explicitly describe controller/master and receiver/slave. Intended multi-receiver API, not arbitrary firmware qualification or GVCP silence. |
| External destination | GetLocalIPAddress/GetLocalPort expose destination; Open docs require separate SCDA configuration, suggesting `PvDeviceGEV::SetStreamDestination`. A sole external owner could perform equivalent qualified writes without constructing PvDeviceGEV. |
| Receiver control traffic | Multicast Open docs explicitly use GVCP reachability and resend. Example sets stream RequestMissingPackets=false. Absence of all additional GVCP sockets/commands is not proven. Strict one-socket integration remains unqualified. |
| Independent lifetime | `PvDevice::Disconnect()` and `PvStream::Close()` are distinct objects/APIs. This does not make a connected native control object safe beside an external controller. |
| Access transitions | `PvDeviceGEV::Connect(..., PvAccessType)` selects access. PvDeviceEnums.h documents alteration through GenICam GevCCP while connected. No socket/heartbeat handoff follows; ReadOnly does not authorize arbitrary writes. |
| Engine/heartbeat delegation | No supported socket injection, request-ID delegation or coordinated external heartbeat transport established in PvDevice/PvDeviceGEV/PvStreamGEV. SetHeartbeatThreadPriority adjusts scheduling, not authority. |
| Deadlines | GetCommunicationParameters exposes documented AnswerTimeout, CommandRetryCount, HeartbeatInterval, DefaultHeartbeatTimeout. SDK policy, not absolute per-command deadline/cancellation; direct Read/WriteMemory/Register lack such arguments. WaitForMessagingChannelIdle(timeout) is not a command deadline. |

The shipped devicecommunicationgev.html documents AnswerTimeout as response wait,
CommandRetryCount as attempts, and DefaultHeartbeatTimeout applied at connection
(then GevHeartbeatTimeout changes device timeout). These do not justify changing
production policy to claim one transmission or bounded whole-operation completion.

Evidence supports a Pleora-only stream-only investigation, especially multicast
with resends disabled. Still needed: vendor-confirmed socket/traffic model, packet
capture for open/run/close/faults, external destination/acquisition and payload
configuration, loss recovery, firmware access policy, licensing and physical
qualification. Header compilation/disconnected fixtures cannot supply that.

## Architecture decision

| Criterion | A: external owner + passive SDK stream | B: engine in SDK-compatible owner | C: isolated sole-owner process |
| --- | --- | --- | --- |
| One authority/no competing sockets | Pleora plausible but traffic gate open; Aravis public path unavailable | Neither inspected SDK exposes native engine hook; maintained changes needed | Contains an owner but still needs A/B/native SDK; boundary cannot remove sockets |
| Heartbeat | External scheduler; receiver must not interfere | Unified if all native paths replaced | Owned lifecycle; CPU scheduling still not guaranteed |
| Configuration/native receive | External bootstrap/XML/profile/acquisition/destination required; Aravis blocker | Preserve feature/stream assumptions with substantial SDK work | Preserves chosen implementation; IPC adds no camera semantics |
| Deadlines/profile policy | Controlled engine bounds waits; SDK-free policy | All native control must route through engine | IPC wait/process kill can bound caller, not native retries |
| Loss/shutdown | Fence and retire receiver without unauthorized writes | Adapt heartbeat/cache/teardown coherently | Contains hangs; death leaves effects uncertain/control until expiry |
| Cross-session stale packets | Needs additional identity/lifetime | Same requirement | New process does not prevent port reuse |
| Effort/maintainability | Narrowest Pleora-only investigation, not common two-SDK solution | Highest effort/vendor cooperation | Useful containment, added IPC/lifecycle cost; complements A/B |

Recommendation: preserve production native ownership. Investigate A only for
Pleora with audited receiver traffic. If literally one GVCP socket is required
across all receiver activity, no inspected supported SDK arrangement is qualified.
For Aravis seek upstream passive/transport support first; do not fork merely to
claim success. C can bound caller waits and contain a future qualified owner but
does not solve retries, ownership or stale sessions. B is not a narrow public-API
integration today. No demonstrated common production route meets all constraints.

See [design](tasks/GIGE-MEL-039-design.md) and
[qualification](tasks/GIGE-MEL-039-validation.md). No actual camera was contacted.