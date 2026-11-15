# GIGE-MEL-040-R1 validation and disposition

**AUDIT_INCOMPLETE / NATIVE_RECEIVE_PENDING.** Successful stream-only Open/Close
is now directly observed; licensed image delivery, resend control, fault behavior
and controller coexistence are not qualified. Native TSan has an additional fatal
runtime finding. No production integration approval follows from this diagnostic.

## Source and execution environment

Fetched main `72a575102995e9a23bc615008300a8c2db22eea2`, tree
`6ce89e16889cf6fe2cd69af50378d0398b50123e`; GitHub PR #34 MERGED at this commit.
Branch `feature/gige-mel-040-r1-veth-receiver`. Final exact commit/tree and final-HEAD
regression results belong in the delivery PR, not a self-referential source hash.
See the [initial design/hypothesis table](GIGE-MEL-040-R1-design.md).

Workstation Linux 6.12.116 x86-64, GCC 14.2, Clang 19.1.7. eBUS **6.5.3.7155**
at `/home/zboll/sdk/pleora/ebus_sdk`, matching external GenApi 3.4.1.1 headers at
`/home/zboll/sdk/gige-mel-027-genapi/headers/library/CPP/include`. Runtime maps and
SHA-256 inventories independently establish loaded libraries, not just ldd intent.
The four GenApi/logging hashes match the prior [040 record](GIGE-MEL-040-validation.md).
No entitlement claim: the synthetic frame operation actually reports **1538
NO_LICENSE**. No licenses installed/changed, no binary patching/interposition,
no watermark removal. Returned bytes are preserved unchanged privately.

Aravis **0.8.36** from `/home/zboll/sdk/aravis-0.8.36/lib/libaravis-0.8.so` is
checked by its runtime version functions and independently hashed. The existing
release archive matches pinned SHA-256
`246deaa0042a387ff1bc00332d0fb80537ce14abde2c28d1a54c91f17adc51bf`.
Bootstrap/GVCP/GVSP definitions were checked against this release's source;
source copies stay external. No production Aravis adapter or fixed-port loopback
fixture change. The genuine ArvGvFakeCamera runs in its own namespace with its
actual modeled bootstrap memory configured to project-owned identity and actual
endpoint. It retains its real one-stream-channel model. Deterministic GVSP packets
are from a separately logged test transmitter, **not** claimed as Aravis-generated
acquisition. No device control connection/acquisition commands were used.

## Isolation and resource evidence

The explicit Python launcher starts an unprivileged user/network namespace;
its directly owned child starts a second network namespace in the same user
namespace. A new veth peer is moved only between these two private namespaces.
Receiver `veth-rx`: `10.64.40.1/30`, MAC `02:00:00:40:01:01`; camera `veth-cam`:
`10.64.40.2/30`, MAC `02:00:00:40:01:02`. MTU **1500**, both links UP. Only lo
and the respective veth endpoint exist. Both begin with only lo. No physical NIC,
default route, gateway, host interface mutation, camera-subnet route, or named
permanent namespace. IPv6 link-local/MLD and multicast traffic stay on this pair.

Each run records namespace/user IDs, PIDs, interface kinds/index/flags/MAC/MTU,
addresses and all route tables. Independent ping in **each direction**, two
requests/replies with zero loss, precedes native construction. The raw host
inventories are retained before/after; comparison excludes only ticking address
lifetimes and bridge gc/hello timers, not configuration. An initial development
run incorrectly compared those countdowns byte-for-byte and failed closed; no
host configuration change occurred.

Both unfiltered ETH_P_ALL AF_PACKET observers precede native loading and outlive
native destruction. They preserve every frame including ARP, ICMP, IPv6 control,
IGMP, loopback SDK syslog and local wakeups. Transmit counts use the originating
PACKET_OUTGOING observation only, not the receiving copy. Dual-end GVCP bytes/counts
are compared; responder application receives/ACKs are separate counts. Kernel
drops, capture exceptions, premature termination and unidentified outgoing frames
fail the observation gate. Early multicast diagnostics failed until four observed
IGMPv3 membership records were identified; those failures are retained, not erased.

Children are directly owned and reaped, with bounded waits and parent-death
signals. Observational strace 6.13 was downloaded/extracted under the private
evidence prefix, **not installed on the host**. `--kill-on-exit` protects its
tracee. No tcpdump process is launched. Closure records show packet socket table
empty, peer disappearance leaving only lo, owned child retirement and namespace
IDs absent from outer `lsns`. No temporary route or veth remains in the host.
The old 040 tcpdump PID 1656010 was absent at initial R1 inspection; R1 did not
signal it or unrelated processes. Snapshot absence alone is never used to claim
the SDK created no transient socket; syscall traces show its actual short-lived
sockets and binds.

All raw pcaps/JSONL, native diagnostics, returned synthetic image bytes, maps,
hashes, syscall traces and cleanup records are private under
`/tmp/gige040r1-evidence/`, outside Git. Do not publish this directory or SDK
material. Development runs are separate from the final-source repetitions and
final-HEAD regressions attached to the PR.

## Measured diagnostic matrix

Every successful Open below returned **0 OK**, empty description; Close **0 OK**,
empty description. Absent/old-loopback Open: **25 NOT_FOUND**, description
`Unable to open the stream. Unable to find a suitable data receiver.` Close:
**23 NOT_CONNECTED**, `Unable to close stream. Not connected.` No PvDevice object.

| Scenario | Native Open / Close | Additional SDK GVCP transmissions | Native receive | Conclusion |
| --- | --- | --- | --- | --- |
| Previous loopback fixture, repeated with syscall observation | 25 / 23 | 1 discovery | Not reached | Retained baseline; ACK reaches native recvfrom |
| Veth, absent responder, fixed 41040 | 25 / 23 | 1 discovery | Not reached | SDK logs device unreachable; ICMP port unreachable |
| Veth, minimal endpoint-consistent responder, fixed 41040 | 0 / 0 | 1 discovery + 1 register read | Four timeout polls | Minimal ACK suffices for Open; unsupported read fails responder gate |
| Veth, validated discovery/read-only bootstrap, fixed 41040 | 0 / 0 | 10 total: inventory below | Four timeout polls | Discovery accepted sufficiently for user-mode receiver allocation |
| Same validated responder, port 0 | 0 / 0 | 10 | Four timeout polls | SDK-selected port succeeds; fixed port was not the Open blocker |
| Same validated responder, string-only overload | 0 / 0 | 10 | Four timeout polls | Documented automatic defaults work; capacity differs from five-argument case |
| Same validated responder, documented multicast overload | 0 / 0 | 10, plus IGMP | Four timeout polls | Multicast does not remove initial GVCP in this tested mode |
| Genuine ArvGvFakeCamera, fixed port, default mode + explicit deterministic sender | 0 / 0 | 13 | Genuine PvBuffer, operation 1538 NO_LICENSE | Receiver transport reached; licensed pixels not qualified |
| Same software-device/sender topology, requested resend-disabled mode | 0 / 0 | 13 | Genuine PvBuffer, operation 1538 NO_LICENSE | Disable/readback failed, so not a qualified resend-disabled mode |

The old ACK's 8-byte header + 248-byte payload, version/mode/string offsets and
request correlation match pinned definitions. Its synthetic MAC cannot describe
loopback Ethernet; its zero register replies do not model a real device. The new
minimal responder preserves old identity/protocol fields but changes endpoint
IP/mask/MAC truthfully. The validated ACK advertises static persistent IP, /30,
gateway zero, locally administered actual MAC, terminated ASCII project identity.
Capabilities/interface/channel counts are **not fields in the discovery ACK**:
they require separate bootstrap reads. Validated discovery fixture models one
interface, zero stream/message channels and no optional capabilities; it is not a
full camera. It serves only known bootstrap reads and rejects unsupported commands
with actual NOT_IMPLEMENTED, never pretended successful writes. Aravis independently
supplies a real one-channel/query implementation. None of this is full GigE Vision
device certification.

### Exact native GVCP command inventory

Each entry is one SDK-originated transmission in the stated successful lifecycle;
both endpoint captures corroborate it. Responder ACKs use the matching request ID,
source `10.64.40.2:3956` and the SDK's actual ephemeral source port as destination.
Requests below come from two independently visible native sockets (discovery and
subsequent bootstrap reads). No socket-count inference from polling is required.

| Request ID | Command | Validated fixture payload/address | Aravis payload/address |
| --- | --- | --- | --- |
| b001 | 0002 DISCOVERY_CMD | empty | empty |
| b002 | 0080 READREG_CMD | 00000000 version | same |
| b003 | 0080 | 00000004 mode | same |
| b004 | 0080 | 00000934 GVCP capabilities | same |
| b005 | 0080 | 00000904 stream count | same |
| b006 | 0080 | 00000900 message count | same |
| b007 | 0080 | 00000940 timestamp frequency low | 0000092c (read observed; not defined by pinned Aravis header) |
| b008 | 0080 | 0000093c timestamp frequency high | 00000d20 (read observed; not defined by pinned Aravis header) |
| b009 | 0084 READMEM_CMD | 00000088, length 32 | 0080 read 00000d24 (not defined by pinned Aravis header) |
| b00a | 0084 | 000000a8, length 48 | 0080 read 00000940 |
| b00b | — / 0080 | not sent | 0000093c |
| b00c | — / 0084 | not sent | 00000088, length 32 |
| b00d | — / 0084 | not sent | 000000a8, length 48 |

ACK commands are 0003, 0081 and 0085 respectively. Minimal responder sees b001
and b002 only and rejects b002. Absent responder sees b001 in capture, zero app
receives/ACKs. No register/memory writes, privilege operations, competing heartbeat,
packet resend, stream configuration or acquisition commands were observed in the
reached bounded ordinary lifecycles. Read semantics of the three unlabelled Aravis
addresses remain unspecified; do not invent their feature meaning. No unknown
opcode is silently called a read or excluded from counts. Loss/control-loss
lifecycles were **not executed**.

## Failure boundary and hypothesis disposition

Repeat loopback syscall trace directly proves native `recvfrom` returned the
entire correlated 256-byte discovery ACK. eBUS then enumerated interfaces through
SIOCGIFCONF and netlink (only lo), closed the probes, logged
`DataReceiverFactory.cpp(94)[Create] - Could not reach device, data receiver factory create failed`,
and returned NOT_FOUND. There is no stream data-port bind in that trace.
This narrows the failure **after ACK receipt and interface enumeration but before
receiver socket binding**. It does not reveal eBUS's closed-source acceptance rule.

Veth absent-device execution has ECONNREFUSED at discovery recvfrom and the same
factory diagnostic. With a minimal ACK, the same runtime enumerates `veth-rx`,
queries its hardware address, performs bootstrap reads, binds
`10.64.40.1:41040` and returns OK, receiver type **1 UserMode**. No eBUS-driver
requirement is inferred; this mode demonstrably works without installing a driver.

**Best-supported explanation: loopback-specific interface/reachability suitability
is the previous Open blocker (H1 supported). A more complete responder is not
necessary for Open on veth (H2 not necessary for that boundary), but is necessary
for truthful subsequent query behavior.** Endpoint fields necessarily change
with the topology; this is not a binary-level proof separating every identity and
interface acceptance predicate. ACK receipt alone does not establish acceptance.
Successful receiver allocation/bind/Open with the validated ACK does. Do not
generalize loopback behavior to every Linux NIC or virtual Ethernet implementation.

## Native receive/resend/coexistence limits

Local IP/port, parameter lookup, four host buffers of 65536 bytes, pipeline
Start/Stop and explicit stream Close/destruction ran. Poll timeout is **30 TIMEOUT**;
Stop duration is recorded in microseconds per run, not a universal bound.
RequestMissingPackets exists as PvGenBoolean but reports available/readable/writable
all false. Get/readback: **16384 GENERIC_ERROR**, `Node is not readable`; explicit
SetValue(false): **16384 GENERIC_ERROR**, `Node is not writable`. No default value
or successful disable is claimed, even though initialized local bool is false.

Sender logs eight deterministic 32x16 Mono8 frames, IDs 401..408, ordered
leader/payload/trailer with bytes `(frame_id + pixel_index) % 256`. Actual buffer
408, size 512, is retrieved/released; image dimensions/pixel type match but the
operation is **1538 NO_LICENSE**, and all returned image bytes are zero. These
bytes are preserved, never replaced by expected pixels. Earlier frames sent
before retrieval may be dropped by the pipeline; this is not a recycling or
throughput qualification. No claim of complete usable image payload, licensed
watermarked delivery, incomplete-frame behavior, receive deadlines under faults,
retained buffers, or stop-under-loss qualification. Coexistence/control authority,
heartbeat, acquisition, permitted writes, control loss and release are **NOT
EXECUTED**: no independently validated meaningful ownership model was exercised.

## Sanitizer results: no new exception

First successful-open native TSan run was **unsuppressed**, options
`halt_on_error=0:exitcode=66:second_deadlock_stack=1`. It reaches actual NO_LICENSE
buffer delivery then dies at Pipeline.Stop with a **new, unqualified fatal check**:
`sanitizer_thread_registry.cpp:348 ((t)) != (0)`, through pthread_detach,
PtUtilsLib::Thread::WaitUntilSignaled, PvStreamLib::Pipeline::Stop. Native exit 66;
no process completion marker. This is **not** the known external lock-order cycle
and is not waived. Packet/namespace/process retirement still completes externally.

A separate absent-responder unsuppressed run reaches destruction and preserves
the known log4cpp addAppender/removeAllAppenders lock-order warning, exit 66. Its
external offsets/order and independently loaded four library hashes match the
previous warning. This is not an unsuppressed pass. No new suppression/exception;
the existing scoped GenApi contract remains unchanged and separately tested.

## Regression and independent gates

SDK-free observer unit suite exercises parsing, literal bootstrap offsets/lengths,
identity corruption, source/header correlation, exclusive bind, unsupported writes,
host refusal before SDK access, route/interface rejection, capture loss/errors,
dual-end deduplication and unidentified traffic. It is not native qualification.
Native executable remains behind default-OFF
`GIGE_MEL_PLEORA_STREAM_TRAFFIC_AUDIT`; neither native launcher nor audit executable
is installed or auto-registered in CTest/CI.

Final-HEAD nine-lane matrix, exact counts/logs and installed consumer/package,
integrity/publication/documentation/source checks are reported on the delivery PR.
All ordinary network suites run serially. Camera hardware options remain OFF and
no hardware tests are registered/executed. Regression passes cannot waive the
native NO_LICENSE, unavailable resend feature or new TSan failure.

| Gate | R1 disposition |
| --- | --- |
| A: native stream-only Open/Close | PASS for bounded ordinary veth responding-device modes; no application PvDevice |
| B: no unauthorized control | AUDIT_INCOMPLETE; reached paths have only read-only traffic, not full fault/coexistence coverage |
| C: strict zero-additional-GVCP | FAIL in every measured mode, including multicast; original unicast evidence retained |

**Recommendation:** further isolated Pleora diagnostics are feasible now that
native veth receiver allocation works. Production passive integration remains
blocked by strict Gate C, entitlement, unavailable resend controls, unqualified
native TSan and missing full receive/control-coexistence evidence. Resolving
read-only query policy needs separate architecture approval; do not weaken it
here. Review readiness applies only to diagnostic evidence/tooling, not receiver
qualification or production integration.