# GIGE-MEL-038 qualification record

Base commit `7d9f88d90c297238deaa7339880224a985c591bb`, tree
`873aa009226f87a75c8e9b58cc98afe29be6094d`; PR #31 confirmed merged.
Branch `feature/gige-mel-038-controlled-gvcp`. Exact final HEAD and tree, final
executed matrix and delivery disposition are recorded on the PR to avoid a
self-referential source commit identity.

## Reproduction and evidence scope

Linux x86-64, GCC 14.2, Clang 19.1.7, CMake/Ninja Debug. Nine lanes: SDK-free
GCC Debug/ASan+UBSan/TSan and Clang Debug; Aravis-only and Pleora-only Debug;
combined SDK Debug/ASan+UBSan/TSan. Explicit optional SDK selection with existing
Aravis 0.8.36 and eBUS 6.5.3.7155 external installations. Build with --parallel 2,
then full CTest --output-on-failure, serializing lanes to preserve existing 3956
socket-audit rules. ASan detect_leaks=1:halt_on_error=1; UBSan
halt_on_error=1:print_stacktrace=1; TSan halt_on_error=1. No new or widened
sanitizer exceptions. Existing scoped GenApi audit remains enabled and unchanged.

`controlled_gvcp_parser` and `controlled_gvcp_loopback` are SDK-free and run in
every lane. Their responder uses its own byte codec, actual nonblocking loopback
UDP sockets and independently recorded command bytes/counts and side effects.
No native SDK retry implementation contributes to this evidence.

## Acceptance checks

* Packet parser: every header/payload truncation, all 65536 declared lengths and
  status words, known explicit errors, malformed types/statuses, ID/command
  correlation, memory address echoes, write index and 3000 deterministic
  adversarial packets. ID allocator covers all 65535 nonzero IDs and permanent
  exhaustion without reuse (unit evidence, not 65535 network transactions).
* Six successful real UDP operations: numeric word 0x91ab23cd and exact opaque
  336/508-byte transfers including the top of the 32-bit address window. Literal
  command-byte oracle proves header, ID, address and numeric endianness. Oversize,
  empty, tail, overflow, unaligned, non-loopback, DNS/IPv6, zero/3956 endpoints and
  infinite-deadline refusal. Memory bytes use an asymmetric pattern.
* Eight fault modes with a 65 ms absolute deadline: drop, 140 ms delay, repeated
  pending ACKs advertising 60 seconds, wrong ID, wrong command, malformed length,
  wrong sender and unavailable execution. Each must time out between 60 and
  300 ms, report one transmission and uncertain write completion, and show exactly
  one server command. Applied writes remain applied; unavailable execution does
  not change state. No retry or rollback packet exists.
* Hostile stream includes wrong endpoint/type/status/ID, short headers, trailing
  data and 2000-byte datagrams, then a valid ACK. Negative device status is an
  explicit failure. Partial memory execution is independently retained despite
  failure. Failed memory read leaves caller output unchanged.
  A separate dropped bulk-write ACK leaves all 336 bytes applied exactly once;
  the following dropped bulk-read ACK also produces only one command and leaves
  destination bytes unchanged.
* Cancellation before transmission: zero commands, not-attempted write. Pending
  receive cancellation: joined caller completes within 150 ms tolerance, one
  command. Late and duplicate ACKs cannot satisfy subsequent IDs.
* Concurrent admission: 16 callers get busy/no-send while one is pending;
  subsequent successful traffic has unique IDs. Twenty create/destroy cycles
  observe exactly one added socket, loopback ephemeral bind, NONBLOCK/CLOEXEC,
  EBADF after destruction, zero retained descriptors and no added client threads.
  Unavailable server returns within 250 ms on a 40 ms operation budget.

These tolerances detect restarted pending budgets and SDK-sized unbounded waits;
they do not qualify arbitrary OS scheduling. The server worker is test-owned and
joined. The client has no worker and no detach path. No physical camera, network
NIC, ownership coexistence, sensor acceptance or hard real-time guarantee is tested.

## Regression and publication gates

The unchanged complete suite covers native Aravis and Pleora contracts, existing
register transport, dynamic modules/Cognex, navigation forwarding, streams,
buffer retention and lifecycle. Installed consumer/dependency isolation,
upstream/JSON integrity, proprietary publication safeguards, local documentation
links and whitespace are separate executed gates; final results belong on the PR.
Configured CI alone is not an executed result.

## Remaining integration blockers

See [ownership alternatives and navigation state separation](../controlled-gvcp-transport.md).
No supported sideband arrangement is established for either SDK. A single
command/heartbeat/lifecycle authority, vendor hooks or qualified passive receiver
arrangement, cross-session stale packet strategy, actual sensor protocol,
idempotence/reconciliation, firmware capability and hardware qualification are
required before production integration. This PR deliberately implements none of
those integration changes.
