# GIGE-MEL-046 — SDK-free qualification

**Scope:** [control authority](GIGE-MEL-046-design.md), in-process fixture and
isolated IPv4 loopback. No native SDK or physical camera was used in local
qualification. `NATIVE_SDK_INTEGRATION_NOT_QUALIFIED`. The experiment adds no
production command path and does not qualify physical navigation forwarding.

## Source and local execution

Fresh `origin/main` baseline: `3cc38e86c5c4c7f2359237565ece3eeeb53f5831`
(tree `4b199a927116122bd1dad493b4b6c6e426aa5681`), which incorporates merged
PR #58. Exact delivery commit/tree and hosted results are recorded on the PR,
not inferred from the baseline or from the presence of CI configuration.

Run on 2026-11-19 with CMake 3.31.6 on Debian Linux x86-64 (6.12.116), GCC
14.2.0 and Clang 19.1.7. `BUILD_TESTING=ON`; all SDK, producer and hardware
options off. Unlike an SDK-free configure with defaults, this run explicitly
enables `GIGE_MEL_TEST_CONTROL_AUTHORITY=ON`:

```sh
cmake --preset dev -DGIGE_MEL_TEST_CONTROL_AUTHORITY=ON
cmake --build --preset dev --parallel 4
ctest --preset dev
ctest --preset dev -R '^control_authority_' --repeat until-fail:20
```

For Release, ASan/UBSan and TSan use `release`, `asan` and `tsan` respectively
instead of `dev`, in separate build trees. Clang Debug uses `CXX=clang++` with
`--preset dev -B build/clang`, then build and CTest in `build/clang`. Sanitizer
settings: `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`,
`TSAN_OPTIONS=halt_on_error=1`. No suppression was added or used for these lanes.

| Lane | Complete SDK-free CTest | Focused repetitions |
| --- | --- | --- |
| GCC Debug | 40/40 passed | 20 per new case, passed |
| GCC Release | 40/40 passed | Both cases included in suite |
| Clang Debug | 40/40 passed | Both cases included in suite |
| GCC ASan/UBSan | 40/40 passed | 20 per new case, passed |
| GCC TSan | 40/40 passed | 20 per new case, passed |

The unchanged assertions in `device_register_contract`, `controlled_gvcp_parser`,
`controlled_gvcp_loopback` and `gvcp_single_owner` ran in each lane. In particular
the previous exhaustion, stale-response, failure observation and cleanup cases
were not replaced or relaxed. The existing C++ provider workflow enables the
opt-in for SDK-free compiler and sanitizer lanes. Its pre-existing optional
Aravis fixture jobs are separate and do not qualify this authority for an SDK.

## Authority fixture results

`control_authority_contract` executes on the submitter's thread. The fixture
tracks calls, actual modeled writes, in-flight work and destruction rather than
deriving them from authority returns. Bounded rendezvous order events; deadline
cases deliberately exercise the real steady-clock timeout, without sleeps.

| Case | Observed result |
| --- | --- |
| Unavailable executor, wrong generation, expired/infinite deadline, already stopped | No command entry or send. Unavailable executor returns `unsupported`. |
| Normal ACK, executor refusal, ordinary pre-send timeout/cancellation | Correct ACK and 1 effect on success; 0 sends on refusal. Later admission remains possible when no effect was possible. |
| Concurrent submitters and reentrant submit/shutdown | One active invocation; competing and nested submissions do not enter. Recursive shutdown does not self-wait. |
| Cancellation after send, missing ACK, deadline after send, unexpected exception | Effect remains recorded; `uncertain` persists, even after owner retirement. A dependent write or explicit retry does not reach the executor. |
| Control loss with a pending write or positive ACK | No success after observed loss; `control_lost` and the unresolved effect are both retained. A later submission does not execute. |
| Concurrent shutdown and admission | Cancellation drains before the operation deadline. Admission stays closed during teardown; the executor is destroyed once with zero active work. Repeated shutdown does not retry or invoke anything after retirement. |
| Replacement with separately granted fixture evidence | Old-generation submit/loss cannot affect B. Old-generation or untagged ACK cannot produce success for B; B's possible effect remains unresolved. |

TSan detected no race or lock-order inversion on the exercised paths. The
authority creates no threads; test callers join and retirement waits for active
executor work. Cancellation is cooperative and races with the final pre-send
check; a possible send in that window must never be reported as a definitive
rejection. The tests do not prove hard real-time scheduling or finite completion
for an arbitrary executor. No intentionally hanging SDK model is required or run.

## Cross-session loopback result

`control_authority_stale_gvcp_ack` extends the independent GIGE-MEL-038 endpoint,
not the GVCP parser. The only binding seam is private and test-accessible;
ordinary `Client::create` still binds an ephemeral loopback port. There is no
port-sharing flag, non-loopback address, discovery or physical privilege traffic.

1. A's first write (ID 1, value 11) is received/applied and its response withheld.
2. The test cancels A, joins the submitting caller, checks transmitted/uncertain,
   and destroys A's socket.
3. B binds A's observed endpoint, issues its first write (ID 1, value 22).
   The responder records but does not apply B and generates no response for it.
4. The responder releases only A's stored ACK after observing B's request. B's
   client reports **acknowledged**. The fixture independently confirms exactly
   two commands, identical source endpoint/request ID, and value 11 still set.

This is a successful reproduction of a **negative fencing result**. It does not
measure how often the allocator naturally reuses a port or how long packets live
in a real deployment. The new authority deliberately does not admit ordinary
GVCP as an executor: a local epoch stamp cannot make this ACK session-specific.
The replacement fixture's declared epoch is different evidence and is not a
claim about an existing camera's wire format.

## Merge and next-work gates

Exact-HEAD hosted checks and separate independent technical review are required
before merge. Local passes do not authorize deployment. Before adding a sensor
handler, establish an exclusive native-compatible owner, qualify finite operation
and cleanup including heartbeat/stream/GenICam coordination, prove cross-session
wire fencing and uncertain-effect reconciliation, and define the detector protocol
and its acceptance evidence in an authorized hardware qualification. Separate
transport ACK, sensor acceptance, frame association and alignment results.