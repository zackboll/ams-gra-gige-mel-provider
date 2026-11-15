# GIGE-MEL-039 qualification record

Fetched base commit `26e141eb11106ac680d5fea3eddd5ee82ff9486a`, tree
`0bc7cffd4bcbceb50e657637e897966a155d37ad`; PR #32 confirmed merged.
Branch `feature/gige-mel-039-single-owner-control`. Final commit/tree and executed
final-HEAD matrix are recorded on the delivery PR to avoid self-referential IDs.

## Reproduction

Linux x86-64, GCC 14.2, Clang 19.1.7, CMake/Ninja Debug. Configure SDK-free lanes
with ordinary CMake, `GIGE_MEL_SANITIZERS=address,undefined` or `thread` for the
sanitizer lanes, and explicit compiler selection. Native lanes use external
Aravis 0.8.36 and Pleora eBUS 6.5.3.7155 with its matching GenApi headers.
Build --parallel 2, run full CTest -j1 --output-on-failure. Nine lanes: GCC,
ASan/UBSan, TSan, Clang, Aravis-only, Pleora-only, combined Debug/ASan/TSan.
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1; UBSAN_OPTIONS=halt_on_error=1:
print_stacktrace=1; TSAN_OPTIONS=halt_on_error=1. The existing exact scoped GenApi
audit is unchanged; no suppressions or disabled regressions are added.

`gvcp_single_owner` runs the new independent loopback fixture in each lane;
`controlled_gvcp_parser` and `controlled_gvcp_loopback` retain 038 coverage.
CTest timeout 90 seconds and individual 300–350 ms tolerances expose unbounded
waits; neither those tolerances nor passing tests prove hard-real-time scheduling.
All network suites/lanes run serially for the existing fixed-port SDK simulator.
No physical-camera tests are enabled or executed.

## Server-observed contracts

* R1 correlated negative ACKs: ordinary register write (5 commands), applied claim
  (3), explicit privilege heartbeat read (5), and explicit identity heartbeat read
  (6). The independent server retains the same controller in all four cases;
  the write's bytes and the claim's side effect remain applied. Caller records
  exact device status `0x8006` and `OutcomeUnresolved`; subsequent write/heartbeat/
  acquire are refused without transmission. Repeated shutdown stays `unresolved`,
  emits no release and never falsely reports `control_lost` or `not_owned`.
  Existing-controller pre-claim refusal is terminal `NotOwned`, not `ControlLost`.
  The new ordinary-write regression failed against reviewed HEAD's implementation
  before the correction; all final-HEAD results are recorded on PR #33.
* Successful acquisition, explicit heartbeat, numeric register read/write and
  exact asymmetric 336-byte memory read/write, release: exactly 11 commands,
  request IDs 1..11 and identical source IP/port throughout. Construction sends
  nothing. Ordinary privilege/memory-overlap writes are refused without packets.
* Existing-controller refusal and simultaneous candidates: one server claim.
  A separately constructed raw UDP client gets device refusal on privileged write;
  no side effect. Separate identity query prevents privilege=2 from proving ownership.
* Lost acquisition ACK: 3 commands, one applied claim, no retry/release; lost
  release ACK: 5 commands, one applied release, unresolved close preserved.
  Cancellation before acquisition: zero packets. Shutdown during pending
  acquisition cancels boundedly and retains unresolved remote ownership.
* Dropped heartbeat, repeated pending ACKs advertising 60 seconds and stale ID:
  exactly one heartbeat command after acquisition, bounded failure, fenced future
  writes. Revoke to another identity with privilege still 2: failed identity check,
  no release to the other controller.
* Ordinary pending write consumes most lease budget: one actual write remains
  applied, no heartbeat/retry extends it, independent server expiry observed.
  Idle scheduler separately keeps ownership across multiple device lease periods.
* Twelve concurrent callers refused busy/no-send while one command is pending.
  A 1 ms admission deadline is not renewed. Cancellation after send preserves
  observed write and uncertainty. Concurrent shutdown/write and shutdown/pending
  scheduler heartbeat finish boundedly, repeated/concurrent shutdown emits no
  duplicate release. Post-retirement acquire/heartbeat/write send nothing.
* Unexpected server termination returns bounded failure. Unknown/stale ACK before
  a valid response is ignored; late ACK after uncertain write cannot revive a
  retired owner. Cross-session port reuse is explicitly **not** solved.
* Twelve successful acquire/close lifetimes observe one owner socket and no
  retained /proc/self/fd or /proc/self/task delta. Actual owned-session exhaustion
  sends exactly 65535 commands, IDs 1..65535, one endpoint, no allocator reset or
  socket replacement. Exhaustion fences release as unresolved and retires locally.

## Development findings and final qualification gates

Initial runs exposed scheduler bookkeeping causing spurious immediate busy
returns. Worker creation now follows successful explicit acquisition, and bounded
2 ms admission retains the original caller deadline. Five focused ASan/UBSan and
five TSan repetitions passed after the admission correction. No sanitizer
exception was used for this owner. A compiler-path change in an existing scratch
build invalidated nested module caches; clean dedicated lane names avoid it.
Final-HEAD results, not those intermediate checks, determine delivery readiness.

The complete suites include native register, stream/lifecycle, dynamic profile,
Cognex and navigation-forwarding regressions. Separate delivery checks cover
installed consumer/dependency isolation, upstream/JSON integrity, publication
safeguards, documentation links and diff review. Configured CI is not an executed
check. Exact counts/results belong on the PR after final-HEAD execution.

## Limits

See [state machine and SDK decision](../gvcp-single-owner.md). Fixture identity
is source endpoint, not a hardware session token. Cross-session stale ACKs,
sensor reconciliation, SDK traffic/delegation, licensing and physical-camera
ownership/heartbeat/stream qualification remain open. Aravis exposes no qualified
passive receiver seam; Pleora stream-only APIs warrant a traffic-audited follow-up,
not a claim of one-socket production integration. No MEL, production control,
navigation, timestamp, Cognex policy or profile ABI changed.