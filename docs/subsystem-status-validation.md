# GIGE-MEL-006 validation record

## Starting revision and boundaries

Fetched `origin/main` before changing files. The previous local branch
`feature/gige-mel-005-software-bit` was clean. Main had advanced to the PR #5
merge, which contains the BIT R1 corrections (`d99d194 test: correct BIT Health
freshness and receive-gate cleanup`). Created
`feature/gige-mel-006-subsystem-status` from the actual fetched main:

- Commit: `c3d80f2ed6200b08593ec51f12fe5796bf4403a6`
- Tree: `d146dc6d19fe82c4d9595d64971f4ae30600bfca`

No intervening main change was discarded. The 97 MEL dependency files, two
private JSON dependency files, upstream factory signatures,
`include/gige_mel/backend.hpp`, `include/gige_mel/provider.hpp` and the five-job
CI workflow are unchanged. No hardware or vendor SDK was exercised. Normal
builds stay offline and SDK-free.

Local environment: Linux 6.12.113+deb13-amd64, GCC 14.2.0, CMake 3.31.6, Ninja.
Local `clang++` is **not installed**, so no local Clang success is claimed. The
hosted Clang job remains required.

## Scope of the change

- `src/provider.cpp`: Health `SubsystemStatusResp` registration (exact upstream
  overload) and Health metadata advertisement. A pure `subsystem_report()` maps
  the captured state. `health_snapshot()` commits the BIT-state revision with
  completion/fault observations and consumes the subsystem sequence only after
  successful payload formation. The publish path appends the report and
  prepares BIT state for BIT or Subsystem subscribers. A private
  `ProviderSoftware` identity supplies the MFA_Status About and VersionInfo text
  and the CSCI version, and a `static_assert` proves modulo-2^32 wrap.
- `CMakeLists.txt`: private `GIGE_MEL_VERSION_MAJOR/MINOR/PATCH` definitions on
  the provider target, taken from `PROJECT_VERSION`. Nothing is exported or
  installed.
- Tests: `tests/provider_subsystem_tests.inc` (11 scenarios). The Health surface
  test's expected capability set was updated, and `SubsystemStatusResp` was
  removed from its unsupported list. A test-executable `test_clock::Fixed` was
  added to the existing clock interposer. There is a new public demonstration,
  `tests/public_subsystem_demo.hpp`, plus loader/installed-consumer hooks.
- Docs: `subsystem-status.md`, this record, and the README, implementation
  profile, architecture, Health, software-BIT and roadmap entries. The stale
  roadmap text that described C2 CommandStatus as ConfigSet/CameraCommand only
  and Health as two report types was reconciled with the implemented BIT and
  Subsystem behavior.
- The twelve Detailed-status keys and their schema, BIT identities/items/fault
  categories, retained completion semantics and Health state mapping are
  unchanged.

## Executed checks

- Both offline verifiers: 97 MEL files and 2 JSON files verified unchanged.
- Development configure/build (`--parallel 2`) and full CTest: **6/6 passed**.
- Provider suite: **73 scenarios**. That is all 62 previous scenarios
  (including both BIT R1 regressions and the Health teardown/scheduler-delay
  regressions) plus 11 new `Subsystem` scenarios. Both the full suite and the
  focused `Subsystem` subset were executed.
- Release configure/build and full CTest: **6/6 passed**; provider suite 73/73.
- ASan/UBSan configure/build and full CTest: **6/6 passed** with
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. No sanitizer report
  appears in the log.
- TSan configure/build and full CTest: **6/6 passed** with
  `TSAN_OPTIONS=halt_on_error=1`. No sanitizer report appears in the log.
- No sanitizer initialization restriction or reduced configuration occurred.
- Native public-header loader (dev and release): existing Health, BIT, the new
  Subsystem demonstration, registered Mono8 delivery and teardown passed.
- Synthetic capture (dev and release): **64x48 Mono8** binary PGM (`P5 64 48`)
  in the ignored build tree. The printed frame time remains synthetic monotonic,
  with no UTC epoch.
- Temporary-prefix installation to `/tmp/gige-mel-006-install.b8IJr3`, with a
  separately configured and built consumer. It passed the published-MEL-only
  Health, BIT, Subsystem and ConfigSet/status demonstrations. `ldd` resolved
  `libgige_mel_provider.so.0` to that prefix, not the workspace build.
- Bounded repetition: five additional development runs of the coherent-batch,
  shutdown, both no-I/O, clock-failure and counter scenarios, and five TSan runs
  of the coherent-batch, shutdown and both no-I/O scenarios: **all passed**.
- Whitespace check (`git diff --check`) and final changed-file review passed.
  Generated build/install trees are not staged.


## Deterministic contract evidence

All waits are watchdogs whose expiry fails the test. Freshness is established
with payload predicates or with a count recorded after disable/quiescence, never
with a delivery count alone.

- **Surface/lifecycle** (`subsystem_surface`, `subsystem_lifecycle`): checks the
  exact Health capability set in discovery and on the attached channel, and that
  C2/Image do not advertise it. It covers the exact inventory, counts, ID,
  criticality, CSCI name and `Version(0,0,1,0)` field order; unchanged
  VersionInfo API 4 / library 1 / "0.1.0" text; null, first, duplicate and
  detached registration; disabled silence; Subsystem-only reports on the worker
  thread (never the registering thread); and a ≥90 ms lower bound between
  unprompted samples. It also covers late registration prompting, the four
  other Health registrations returning NotSupported, buffers NotSupported,
  re-enable, detach/reattach and Control recreation preserving counters, and an
  independent manager starting at sequence 1.
- **State mapping** (`subsystem_state_mapping`): Idle/Available; gated owned
  acquisition Imaging/Available; injected terminal stop Unknown/Major with
  `connectionEstablished` still true; explicit disable back to Idle/Available
  while `stream_failures`, `last_error` and a retained **Fail** BIT completion
  remain; invalid configuration Unknown/Major; valid readback recovery; and a
  fresh acquisition back to Imaging. It asserts the exact revision at each step.
- **Counters** (`subsystem_counters`): observation is controlled by enabling
  Health only to take samples, so no background commit interleaves with C2.
  Checked: initial revision 0 with sequence 1; repeated unchanged reports
  consecutive in sequence with constant revision; C2 registration and initial
  delivery leave the revision unchanged; accepted Pass, a repeated command
  ID/result, and two executions with a **repeated host timestamp**
  (`test_clock::Fixed`) each advance it; five rejected/unsupported commands and a
  configuration-generation change leave it unchanged; a Subsystem-only observed
  fault creation advances it; an accepted Fail advances it; retirement and
  recurrence each advance it; completion plus retirement in one commit, and
  completion plus creation in one commit, each advance it **once**; and
  detach/reattach preserves both counters.
- **Coherence** (`subsystem_coherent_batch`): the BIT callback of an older
  Health batch is held on a gate after capture. C2 executes a new BIT on another
  thread and completes while the callback is still held. The held batch's later
  Subsystem report carries revision 1, matching its held BIT payload, not the
  newer 2. A later batch carries revision 2 with a newer completion payload.
  The first two batches are `S D B U` in order.
- **No I/O** (`subsystem_no_io`, `subsystem_no_io_unwind`): reuses the counting
  adapter and gated receive plan. Registration plus three or more Imaging reports
  add zero adapter calls and leave configuration generation, Image width,
  delivered count, retained bytes and acquisition ownership unchanged. Shutdown
  observes `stop_requested`, explicitly opens the gate, confirms disable and
  worker exit without expiry, and confirms no stream failure. The unwind variant
  throws after enabling Health with receive gated, and the cleanup guard opens
  the gate before any join.
- **Callbacks** (`subsystem_callbacks`): from a Subsystem callback on manager A,
  the existing callback policy holds for A. ConfigSet and BIT through A and B
  return InvalidState, Health lifecycle and registration on both fail, and B
  attach/detach fail. Keep-alive, communications, statistics and an explicit
  release of B's delivered buffer succeed. The release resumes B's delivery.
  Each callback throws; exceptions are contained and counted, no configuration
  is applied, and Image A still enables later. Assertion failures on the worker
  are captured and rethrown on the test thread.
- **Shutdown** (`subsystem_shutdown`): external disable (actual `pthread_join`
  entry observed) and external detach (admission closure observed) both wait for
  an admitted Subsystem callback. Gating BIT_Status shows that the unadmitted
  Subsystem suffix is suppressed. After either case, re-enable shows sequence 2,
  proving the suppressed prepared report consumed sequence 1 (a valid gap).
- **Owners** (`subsystem_owners`): a Subsystem-only callback drops the final
  external Control and Health references. Scoped pins keep the objects alive
  through the callback, mutation attempts reject, and the worker exits.
- **Clock failure** (`subsystem_clock_failure`, executed injection): with a fresh
  fault requiring the UTC clock unavailable on the worker for one sample,
  Status/Detailed are published and BIT/Subsystem are suppressed. The diagnostic
  is recorded. The recovery sample carries revision 1 (one commit for the fault)
  and sequence 2 (the failed sample consumed none). Its paired BIT detection
  time is fresh. The separate Subsystem-payload construction failure branch is
  **source-reviewed only**.


## Mutation controls (disposable copy)

To show that the scenarios detect user-visible defects, each defect below was
applied to `src/provider.cpp` in a disposable copy of the working tree
(`/tmp/g6-mut`, never the committed source). The copy was rebuilt, and the
Subsystem scenarios were run against it. Every mutant **failed**:

| Injected defect | First detecting scenario |
| --- | --- |
| Accepted execution does not advance revision | lifecycle (`revision == 1`) |
| Fault-observation change does not advance revision | state mapping (`revision(failed) == 1`) |
| Every sample advances revision | surface (`revision == 0`) |
| Completion + fault change in one commit advances twice | counters (`== 9`) |
| Report dispatches a reread/latest revision instead of the captured one | coherent batch (`revision(held) == 1`) |
| Non-subsystem BIT preparation consumes the sequence | lifecycle (sequence continuity) |
| Subsystem-only registration does not prepare BIT state | surface (no reports) |
| Subsystem report published despite failed BIT preparation | clock failure (revision/sequence) |
| `connectionEstablished` cleared by acquisition failure | state mapping |
| `Critical` instead of `Major` | state mapping |
| Historical `stream_failures` degrade current state | state mapping |
| Major/minor version fields swapped | surface (version order) |
| Capability not advertised | surface |
| Inline dispatch on the registering thread | surface (sequence/thread) |

Initially the "non-subsystem preparation consumes the sequence" mutant
**survived**. The lifecycle scenario was then strengthened with an independent
manager that takes BIT-only Health samples and C2 executions before its first
Subsystem report, which must still carry sequence 1. After that change the
mutant fails. The working tree's `src/provider.cpp` was verified byte-identical
to the unmutated source afterwards.

## Baseline negative control

A disposable detached worktree, `/tmp/gige-mel-006-baseline`, was created at the
actual starting main (`c3d80f2…`, tree `d146dc6…`). `git status` was clean, and
its production source was not modified. One ordinary dev build of
`gige_mel_provider` was made. The **same feature-built** `loader_tests` probe was
run against both DSOs. Each run printed the `dladdr`-resolved factory library,
confirming which provider was actually loaded.

| Library | Mode | Result |
| --- | --- | --- |
| Baseline | `--expect-no-subsystem` | PASS: metadata not advertised; valid registration returns `NotSupported` |
| Baseline | `--subsystem-only` | **FAIL** (rc 1): `missing required SubsystemStatusResp Health metadata capability` |
| Feature | `--subsystem-only` | PASS: inventory, stable BIT_report, updated revision after BIT |
| Feature | `--expect-no-subsystem` | **FAIL** (rc 1): `baseline unexpectedly advertises SubsystemStatusResp metadata` |

The feature expectation therefore fails specifically on missing Subsystem
support against the baseline, and passes against the feature provider. The two
DSOs differ (SHA-256 prefixes `d515ad2b…` baseline, `57728f86…` feature).

## Limitations

No hardware, SDK, camera, transport performance, packet loss, physical health,
component inventory, assigned criticality or connection probing was qualified.
These software tests do not substitute for that qualification. Hosted CI results
are recorded on the PR, not here. The two deferred PR #3 notes remain separate.

