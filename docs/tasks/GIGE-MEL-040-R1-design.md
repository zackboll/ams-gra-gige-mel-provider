# GIGE-MEL-040-R1 pre-implementation design

Base: fetched `main` commit `72a575102995e9a23bc615008300a8c2db22eea2`,
tree `6ce89e16889cf6fe2cd69af50378d0398b50123e`. GitHub reports PR #34
MERGED at that commit. Branch: `feature/gige-mel-040-r1-veth-receiver`.
Written before implementation; the preliminary namespace/veth capability probe
succeeded. This is not yet native receiver evidence.

## Scope and reviewed contract

Reviewed the prior executable, observer, tests, 040 design/validation, single-owner
decision, controlled transport/owner implementations, README, implementation
profile and architecture. Installed eBUS 6.5.3.7155 headers, receivingdata,
streamparametersgev and MulticastSlave were inspected externally. No SDK source,
binary, license, or raw runtime capture is publication input.

The string unicast Open overload explicitly documents transmitter IP, local UDP
port (zero = automatic), transmitter channel, local receiver IP (empty = automatic),
and maximum simultaneously stored buffers. The device-info overload's deprecated
local-IP parameter must not be confused with this declaration. Same-subnet
operation is documented. Multicast takes transmitter IP, multicast group, data
port, channel, local IP and capacity. The shipped sample selects a device first;
do not execute its discovery selector. No application PvDevice will be created.

## Hypotheses and controlled comparisons

| Hypothesis | Discriminating observation | Limitation |
| --- | --- | --- |
| H1: loopback is unsuitable | Repeat five-argument Open with fixed port/capacity/channel on Ethernet veth, absent and minimal responder | A change in result alone does not prove the SDK's internal NIC policy |
| H2: discovery bootstrap is insufficient | Compare minimal and independently checked endpoint-consistent 248-byte ACK on the same veth | Wire validity is not SDK acceptance |
| H1 + H2 | Veth alone fails, improved ACK succeeds | Need packet timing and subsequent initialization evidence |
| SDK/runtime limitation or further protocol gap | Complete ACK and independent Aravis software device still fail | Narrow failure stage with diagnostics; do not infer license or driver rejection from NOT_FOUND alone |

The old bootstrap matches Aravis's offsets and length, but loopback has no matching
Ethernet MAC. Its all-zero arbitrary register read responses are not a meaningful
device capability model. Preserve the original fixture/evidence unchanged. A new
responder uses project-owned identity and the actual namespace MAC/IP/mask, no
gateway, persistent static IP configuration, and rejects unsupported writes/reads.
Add further query behavior only when grounded in pinned definitions/device state.

## Isolation and evidence design

An unprivileged `unshare --user --map-root-user --net` coordinator owns the receiver
namespace. A directly owned child creates a second network namespace in the same
user namespace. Move only one newly created veth peer to that child. No named
namespace, host NIC, host IP/route changes, default routes, or physical discovery.
Use 10.64.40.1/30 and 10.64.40.2/30, locally administered synthetic MAC addresses,
MTU 1500. Refuse nonempty initial interface inventories other than lo. Inventory
both ends and independently test bidirectional ICMP before loading the SDK.

Reuse the unfiltered AF_PACKET observer in both namespaces. Retain both raw
observations, but count transmitted Ethernet frames only at PACKET_OUTGOING on
their originating endpoint; receiving-end records corroborate delivery, not extra
commands. Keep responder receive/ACK records separate. Record MAC/IP/protocol,
ports, lengths, request IDs, direction, native barriers, maps/hashes and process
identities. Observers must precede native process construction and outlive its
destruction. No transient-socket absence claim from snapshots. Use observational
syscall tracing only if available, with no interposition or SDK modification.

All child lifetimes are owned/reaped and bounded, packet threads/sockets explicitly
closed. Verify peer removal, child retirement and namespace disappearance from the
outer launcher. Any namespace/capture/cleanup/unknown-traffic uncertainty fails
the relevant gate. Keep evidence in an exclusive private directory outside Git.

## Execution and gates

Run absent, minimal, validated and (if necessary) pinned ArvGvFakeCamera cases;
compare fixed/automatic ports and a separately documented overload. Do not change
ordinary CTest or enable native audits automatically. Reuse the existing OFF option
and uninstalled executable; production adapters and old fixture stay unchanged.

Only successful Open permits parameter/resend/pipeline testing. API lifecycle
success is not genuine PvBuffer delivery. Coexistence requires an independently
validated privilege model and a real operating stream, not the 039 identity fixture.
Gate A requires successful native Open/Close, B comprehensive running evidence of
no unauthorized control, C strictly zero additional GVCP. Read-only discovery
still fails C; retain prior 0x0002 / 0xb001 evidence. Unsuppressed native TSan first;
no new exception, no global broadening. Environment failure is ENVIRONMENT_BLOCKED,
not a host-network fallback. Unreached reception remains NATIVE_RECEIVE_PENDING.

Run the requested nine regression lanes on final HEAD, serial network suites,
scoped GenApi gate, consumers/isolation, integrity/publication/link/whitespace
checks. Report actual counts and final commit/tree in the PR without a
self-referential committed hash. Push one non-draft PR, never merge/approve.