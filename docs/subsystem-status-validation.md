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

## R1 correction: subsystem test synchronization and failure cleanup

The sections above are historical evidence for the reviewed revision. This
section records the corrective revision.

| | Commit | Tree |
| --- | --- | --- |
| Main at review (PR base) | `c3d80f2ed6200b08593ec51f12fe5796bf4403a6` | `d146dc6d19fe82c4d9595d64971f4ae30600bfca` |
| Reviewed and actual starting PR head | `2dd65e08d9fe711d6d6d664d16cbd63f7da5ae0f` | `07a13b200a219f2ffe1b28ada78875ab1713afd3` |

The branch had not advanced since review, and the worktree and index were clean
at start. The independent review found two P2 **test** defects and no
production defect. This correction changes only
`tests/provider_subsystem_tests.inc`, one scenario registration in
`tests/provider_tests.cpp`, and this document. It also removes the extra
blank line at end of file from four new files. `src/`, `include/`, CMake
files and version definitions, `third_party/`, the manifests, scripts and
`.github/` are byte-identical to the reviewed revision.

### R1-A: the re-enable check read the wrong report

`subsystem_shutdown` enabled Health, waited for `reports.count() + 1` and
then asserted that `reports.last()` carried sequence 2. Two valid schedules
break that check:

- Sequence 2 is delivered before the count is read, so the test waits for 3.
- Sequence 3 arrives between the wait and `last()`.

The provider correctly delivered 2 and then 3.

**Correction.** The new `reenabled_interval()` helper is used by all four
shutdown paths that re-enable (both the admitted-first and suppressed-first
cases):

1. It captures the baseline while Health is disabled and quiescent, then
   enables Health.
2. It waits for **at least two** reports beyond the baseline, so the latest
   report has necessarily advanced. No exact total is required, so further
   periodic reports are allowed.
3. It drains Health with `disable()` and copies the stable collection from
   the fixed baseline index.

The scenario asserts that the exact first new report carries **== 2** and the
last carries >= 3. The existing cleanup guard, drain checks, admission
milestones and suffix-suppression assertions are unchanged.

### R1-B: the collector could be destroyed before Health was drained

In `subsystem_lifecycle`, the independent manager's `other_bits` collector was
declared after `Fixture other`, and that Health publisher had no exception-safe
cleanup. Suppose a wait or check throws while it is enabled:

- Dropping the local Health handle does not detach it; Control still owns it.
- The collector is destroyed first.
- A BIT callback that captured the collector's `this` pointer can still run.

**Correction.** The phase was extracted into `independent_manager_phase()`.
The ordinary scenario and the failure regression both call it.

- The phase-local collector (`ObservedBitReports`) is declared first.
- A `TestCleanup` drain guard follows immediately, installed before the first
  operation that can start publication.
- The guard covers both enabled intervals: BIT-only, and BIT plus Subsystem.
- On any unwind it synchronously calls `disable()`, which joins the Health
  worker, and then `detachChannel()`. Both run while every collector and
  callback capture is still alive.
- The guard records its results in probe state that outlives the phase and
  never throws. Repeating it after the ordinary disable/detach is harmless:
  the ordinary path checks `cleanup_done`, `destroyed_after_drain`, and zero
  callbacks after destruction.

### R1-C: a third instance found and fixed during R1 validation

While building the before/after controls, the same race appeared in
`subsystem_lifecycle`'s other re-enable checks and in the `subsystem_counters`
`sample()` helper. Each of these did:

    enable → wait → disable → (later) last().seq + 1 == next interval's first seq

A plain `disable()` may suppress a Subsystem report that the worker already
**prepared**. That consumed sequence is a valid gap (see the shutdown scenario),
so "last delivered + 1" is not a valid expectation. The corrected tests showed
this intermittently against the unchanged reviewed provider:
`last_delivered_sequence=1 reenabled_first_sequence=3`, which failed in about
1 of 14 runs under `LD_DEBUG` slowdown.

**Correction.** New test-only helpers `stop_after()` and `held_interval()`:

1. They hold the worker inside the admitted callback that records the target
   report, which is the last callback of its batch.
2. They call `disable()` on another thread and wait until it reaches the
   actual worker `pthread_join`. At that point publication has stopped.
3. They release the callback. The worker then exits without preparing another
   sample, so the last delivered report is the last prepared one, and the
   exact `+1` assertions stay unweakened.
4. They check `reports.count() == target` and that the gate did not expire.

`subsystem_lifecycle` and `sample()` now use these helpers.

### Permanent regressions

- **`subsystem_shutdown`** (R1-A): both re-enabled paths in the four-path
  shutdown scenario now inspect the exact first report after at least one
  further report has arrived. This
  covers both the admitted-first and suppressed-first initial conditions.
- **`subsystem_lifecycle_unwind`** (new, R1-B) runs once per enabled interval
  (1 and 2). Steps:
  1. The scenario runs on its own `std::async` thread, which holds every
     Control and channel owner. The coordinator holds only probe state, which
     outlives the scenario.
  2. An admitted BIT callback records its payload, releases the collector
     mutex and waits at a gate. The phase then throws a distinctive
     `InjectedPhaseFailure`.
  3. The coordinator observes that the drain guard's `disable()` has reached
     the actual Health-worker `pthread_join`, via `test_join::Observe` around
     the real cleanup call. It then checks four things at that point: the
     collector is alive, the held callback has not completed, cleanup has not
     completed, and the scenario future has not finished.
  4. It releases the gate and requires the following:
     - the scenario finishes within 2 s with exactly "injected";
     - the gate was entered with no watchdog expiry;
     - the callback completed and the cleanup `disable()` returned `Success`;
     - the collector was destroyed only after draining finished;
     - no callback ran after destruction.

  The gate-opening guard is declared after the future, so it opens the gate
  before the future's destructor can wait.
- **`subsystem_lifecycle`** (R1-B and R1-C) runs the ordinary path through
  the same `independent_manager_phase()`, and stops every interval with
  `stop_after()`.
- **`subsystem_counters`** (R1-C) uses the corrected `sample()`.

The total is now **74 provider scenarios**: the 73 previous ones, all retained,
including the BIT R1 and Health R1 regressions, plus
`Subsystem independent-manager unwind drains Health`.

### Before/after evidence against unchanged production

- **Production.** I built the reviewed commit `2dd65e0` unchanged in a
  disposable detached worktree with one ordinary dev build of the provider and
  the backend. `libgige_mel_provider.so.0.1.0` has SHA-256 `b0699b78…`.
- **Test copies.** Each disposable test copy was compiled with the dev flags
  and linked to that DSO. `ldd` and `LD_DEBUG=files` (`calling init:`) show it
  was the library actually loaded in every run.
- **Diagnostic edits.** Every diagnostic edit is a recorded diff against the
  reviewed or corrected test source. All the diagnostic copies are
  disposable; none of them is committed.

| Control | Test source | Result |
| --- | --- | --- |
| R1-A old test, admitted-first (path 0) | Reviewed + forced schedule A: wait for the first new report, then evaluate the original `count() + 1` expression | **FAIL** (intended), rc 1: `first_new_sequence=2 observed_last_sequence=3`; original `last() == 2` assertion fails |
| R1-A old test, suppressed-first (paths 2–3) | Same, starting at path 2 | **FAIL** (intended), rc 1: `first_new_sequence=2 observed_last_sequence=3` |
| R1-A selector mutant (paths 0 and 2) | Corrected, but asserts `last()` again | **FAIL** (intended), rc 1: `first_new_sequence=2 last_sequence=3`. The corrected fixed-index selection of 2 holds while the latest has advanced |
| R1-B old cleanup | Reviewed + controlled exception while independent Health is enabled; static lifetime markers before/after the collector; the callback never touches destroyed storage | **FAIL** (intended), rc 1: `collector_destroyed=1 health_drained_yet=0 callback_after_collector_destroy=1` |
| R1-B guard mutant | Corrected, drain guard body reduced to bookkeeping | **FAIL** (intended), rc 1: the regression never observes the cleanup join (watchdog expiry) |
| R1-C old test | Reviewed + forced schedule: batch 2 prepared and held in a diagnostic BIT callback until `disable()` reaches the join | **FAIL** (intended), rc 1: `last_delivered_sequence=1 reenabled_first_sequence=3` |
| Corrected tests | Committed test source, unmodified | **PASS**: `74 provider tests passed`, rc 0 |

The R1-B old-cleanup control is a **safely observed lifetime-order violation**:
a BIT callback reached the interval after the collector was destroyed and
before the Health worker was drained. It is not a sanitizer report. The probe
deliberately never dereferences destroyed storage, and no ASan use-after-free
was observed or claimed.

### R1 local validation (corrective working tree)

| Configuration | CTest | Provider scenarios | Sanitizer reports |
| --- | --- | --- | --- |
| Dev | 6/6 | 74/74 | — |
| Release | 6/6 | 74/74 | — |
| ASan/UBSan (`detect_leaks=1:halt_on_error=1`; `halt_on_error=1:print_stacktrace=1`) | 6/6 | 74/74 | 0 |
| TSan (`halt_on_error=1`) | 6/6 | 74/74 | 0 |

- The sanitizer runtimes were active: the test `pthread_join` forwarding target
  resolved to `libasan.so.8` and `libtsan.so.2` respectively. No sanitizer
  failed to initialize, and no configuration was reduced.
- Both offline verifiers pass: 97 MEL files and 2 JSON files.
- The `native_loader` CTest passed in all four configurations. It runs Health,
  BIT, Subsystem and registered image delivery through the public headers
  against the corrective build.
- Bounded repetition: 5 dev runs and 5 TSan runs each of `subsystem_shutdown`,
  `subsystem_lifecycle_unwind`, `subsystem_lifecycle` and `subsystem_counters`.
  All 40 passed. Repetition supplements, and does not replace, the
  deterministic controls above.
- Local `clang++` is not installed, so the hosted Clang job is the Clang
  evidence.
- Capture, installation and the separate consumer were not re-run locally for
  this test-only correction. The hosted normal-build jobs exercise them.
- The 14 production mutation controls were not re-run, because production is
  unchanged.
- Hosted results for the corrective head are recorded on the PR. The earlier
  hosted success for `2dd65e0` is not evidence for this revision.

## Limitations

No hardware, SDK, camera, transport performance, packet loss, physical health,
component inventory, assigned criticality or connection probing was qualified.
These software tests do not substitute for that qualification. Hosted CI results
are recorded on the PR, not here. The two deferred PR #3 notes remain separate.
