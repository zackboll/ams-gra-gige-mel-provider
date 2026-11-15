# GIGE-MEL-038 pre-implementation architecture note

Base: main `7d9f88d90c297238deaa7339880224a985c591bb`, tree
`873aa009226f87a75c8e9b58cc98afe29be6094d`; GitHub confirms PR #31 merged.
Branch: `feature/gige-mel-038-controlled-gvcp`.

## Boundary and decisions (recorded before implementation)

Add a separate uninstalled C++20 library, not linked into the provider, profile
modules, selection, or either native adapter. Keep IRegisterDevice and profile
ABI v1 unchanged. Only explicit numeric IPv4 loopback endpoints are admissible;
bind the owned socket to 127.0.0.1, ephemeral port. No discovery, bootstrap
register writes, control acquisition, native SDK calls, heartbeat or acquisition.

Use Linux SOCK_NONBLOCK | SOCK_CLOEXEC UDP, one socket/client, RAII close and
no client worker. Try-lock admission returns busy immediately; no unbounded lock
queue. The caller owns lifetime and must join callers before destruction.
One absolute steady_clock deadline covers admission, send and receive. Poll in
at most 2 ms slices to observe stop_token without another descriptor or thread.
All loops check stop and deadline, including malformed/pending packet streams.
One send syscall maximum; even send failure is not retried. Successful UDP send
means kernel admission, not proven device receipt. Every post-send write failure
is conservatively uncertain, including a negative device ACK (no atomic rollback
guarantee). Reads publish output only after a fully validated success.

Allocate IDs 1..65535 under the transaction mutex; never wrap/reuse. Exhaustion
permanently refuses new transactions on that client. A new client is a new socket
session, not permission to assume cross-session delayed packets are impossible
after OS port reuse. Production session fencing remains future work.

Use explicit byte loads/stores, no packet struct casts. Bound memory to 508
bytes (512-byte GVCP data minus address); maximum UDP payload 520 bytes, IPv4
packet 548 bytes, below a 1500-byte test MTU. Require mod-four addresses/lengths,
nonempty transfers and no 32-bit window overflow; reject instead of split.
336 bytes fits in one command. Numeric words use network order; memory is opaque.

Validate exact datagram length, status/type, command, nonzero ID, sender, memory
address echo and single-register write index. Ignore invalid/unrelated responses
within the original budget. Recognize pending ACK with four-byte interval but
never consume that interval as a new budget. Return known negative wire statuses
explicitly; do not assign invented meanings to unknown status codes.

## References reviewed

* Existing register service, navigation values/backend interfaces, forwarding,
  implementation profile, README and architecture/lifecycle boundaries.
* Aravis 0.8.36 source commit `97ff5e330919d7a44b871df4eda669feeafdf342`:
  `src/arvgvcpprivate.h`, `src/arvgvcp.c`, `src/arvgvdevice.c`. Header SHA-256
  `f875e2a31209e23fb53452353ac3e002bc44ced25a2824cb8b026e313df40d46`;
  construction source `f1141a4336cf90c20dfb12c7ef24780ffe568481106ecd7de29c6319327d3dbd`;
  device source `6894d67c7e7158677af49bd055fa32e15f79f99c5c3f12bad316c9785fabdac6`.
  Definitions are references, not copied implementation or a build dependency.
* Existing Aravis network fixture uses a fixed 3956 simulator with an audited
  wildcard listener exception. Do not reuse/alter it; new server binds loopback
  only on a test-owned ephemeral port. Serialize old network regression lanes.
* External eBUS 6.5.3.7155 PvDevice/PvDeviceGEV declarations: direct operations
  expose no per-call deadline/token/socket delegation; connect has access modes,
  SDK heartbeat thread and communication parameters remain SDK-owned.
* Separate experimental pleora-navigation-receiver checkout
  `55a4e556b01fa29e6f528591f9e8617e4a4b4def`: host writes a 320-byte v2 report
  followed by CRC commit, with caller serialization and no session/sequence or
  Begin/Abort. Not a qualified sensor protocol; do not transplant its encoding,
  addresses, acceptance assumptions or target SDK claims.

## Qualification plan

Independent SDK-free responder validates received command bytes and records
side effects/counts; inject dropped, late, pending, corrupt/unrelated and negative
ACKs, partial execution and unavailable device behavior. Pure parser tests include
all truncations and adversarial lengths. Test no retransmission/rollback,
cancellation before/after send, concurrent admission, ID exhaustion, late traffic,
closed sockets and unchanged thread count. Full existing nine-lane regression,
installed consumers/isolation, vendor/JSON/publication checks and links follow.
Record executed checks separately from intent. No physical camera tests.

## Ownership decision gate

A second socket is not a second safe controller. Aravis owns a socket, shared ID
counter/I/O mutex, heartbeat, control-loss state and control-release teardown.
Pleora exposes access modes and native lifecycle but no qualified transport hook.
No sideband production integration is justified. Next milestone should establish
a single coordinated command/heartbeat/lifecycle owner, with SDK-vendor hooks or
an explicitly qualified passive-stream arrangement; process isolation alone does
not solve controller identity. Detailed alternatives and navigation state separation
are recorded in the transport note after implementation.
