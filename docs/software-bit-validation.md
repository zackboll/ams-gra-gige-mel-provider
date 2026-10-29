# GIGE-MEL-005 validation record

## Starting revision and boundaries

Fetched `origin/main` before changing files. Worktree was clean on the previous
Health branch; its tree equalled fetched main. Created
`feature/gige-mel-005-software-bit` from the actual fetched main:

- Commit: `72e8dc41a43cb83b45584b01c5781225fca73b44`
- Tree: `59c38ad44df1e6a208b46f01ac11dc52e4197cae`

No intervening main changes were discarded. The 97 MEL dependency files, two
private JSON dependency files, factory signatures and public backend interfaces
remain unchanged. No hardware or vendor SDK was exercised. Normal builds stay
offline/SDK-free. The existing five-job CI matrix is unchanged.

Local environment: Linux 6.12.113+deb13-amd64, GCC 14.2.0. Local `clang++` is not
installed; no local Clang success is claimed. The hosted Clang job remains required.

## Executed checks

- Both offline verifiers: 97 MEL files and two JSON files verified unchanged.
- Development configure/build (`--parallel 2`) and full CTest: **6/6 passed**.
- Provider suite: **60 scenarios**, preserving all previous 45 and adding 15
  focused BIT scenarios. The full suite and focused BIT subset were executed.
- Release configure/build (`--parallel 2`) and full CTest: **6/6 passed**.
- ASan/UBSan configure/build (`--parallel 2`) and full CTest: **6/6 passed**, with
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
- TSan configure/build (`--parallel 2`) and full CTest: **6/6 passed**, with
  `TSAN_OPTIONS=halt_on_error=1`.
- No local sanitizer initialization restriction, reduced configuration, or
  sanitizer code finding was observed.
- Native public-header loader: BIT catalogue, correlated command acceptance,
  independent completed Pass, retained Health result/current faults, existing
  Health reports, registered Mono8 delivery and teardown passed.
- Synthetic capture: **64x48 Mono8**, PGM in ignored build tree; printed frame
  time remains synthetic monotonic with no UTC epoch.
- Temporary-prefix installation to `/tmp/gige-mel-005-install.CWFUNz`, separately
  configured/built consumer (`--parallel 2`): passed published-MEL-only BIT,
  Health and ConfigSet/status demonstration. `ldd` resolved the provider to that
  prefix's `lib/libgige_mel_provider.so.0`, not the workspace DSO.
- Whitespace and final changed-file review passed; generated files are not staged.

### Deterministic contract evidence

Tests check exact full UUID/catalogue/item/fault mappings, accepted-interface and
default unspecified duration, initial truthful faults without completion,
manager-lifetime retention (including Control recreation), bounded replacement,
command precedence/error/correlation and duplicate command IDs without deduplication.
Idle/running/configuration-invalid/owned-terminal/Image-callback-failure outcomes,
fresh fault retirement, recurrence and historical diagnostics are covered.
Completion/detection times are bounded by before/after host epoch times in stable
test intervals; no unconditional monotonic-UTC assertion is used. Continuing faults
preserve detection time.

The counting adapter wraps an injected backend and **every** backend/device/stream
operation. A receive gate holds acquisition at a known milestone with one delivered
MEL buffer outstanding. Catalogue, C2 status, initiation and multiple Health BIT
samples add zero adapter calls; bytes, explicit release, generation and Image
lifecycle remain intact. Separate tests cover ordinary rejection, transient
timeouts and destination-pool pressure without fabricated software faults.

Callback tests exercise synchronous registration owner pins, command owner pins,
Health final-owner release, same/cross-provider reentry, safe reads/comms/keepalive
and explicit buffer release, exception containment, single-command BIT-before-status,
immutable results across a later concurrent failed command, admitted-callback drain
and unadmitted suffix suppression. Health tests cover BIT-only/late registration,
disabled/re-enabled/detached/reattached behavior, S/D/B order and coherent captures
while configuration changes between callbacks.

Shutdown tests wait for actual detached admission or the existing test-only
`pthread_join` observer, not task submission plus timeout. Watchdog gates open on
failure cleanup; expiry fails. Consumer storage and observers outlive callbacks.
The original R1 teardown-publication and scheduler-delay regressions remain in
the 60-scenario suite and ran under all four local configurations. R1's production
unpublication-before-owner-release ordering and weak-across-wait worker discipline
are unchanged; [the original detailed evidence](health-validation.md) is retained.

A Linux test-executable-only `clock_gettime` interposer forwards through RTLD_NEXT
and injects failure only for CLOCK_REALTIME on an armed test thread. No production
clock hook or public diagnostic ABI is added. It verifies initiation failure
preserves completion, initial C2 status preparation failure is retryable, and a
Health BIT preparation failure preserves S/D publication, suppresses BIT with a
diagnostic, and permits later valid reporting. Range-conversion checks are source
reviewed; the test does not alter the host clock or claim PTP/accuracy qualification.

The first new deterministic drain test exposed an initial registration helper
waiting on the operation mutex behind detach's drain. Pre-admission detached/null
checks were moved ahead of that mutex. The corrected drain test and full suites
pass; this was corrected as code, not dismissed as environment behavior.

## Genuine unchanged-baseline negative control

Built a disposable detached worktree at the exact starting commit/tree above in
`/tmp/gige-mel-005-baseline` using dev configure/build (`--parallel 2`). Its
`git status --short` stayed empty. No baseline production source was modified.
The **same feature-built public-header executable** was used for all three runs:

```sh
build/dev/loader_tests /tmp/gige-mel-005-baseline/build/dev/libgige_mel_provider.so --expect-no-bit
# exit 0: C2/Health BIT metadata absent, catalogue/status registrations NotSupported,
# valid BIT_Command{42,{1},{},{}} initiation returns Unsupported.
build/dev/loader_tests /tmp/gige-mel-005-baseline/build/dev/libgige_mel_provider.so --bit-only
# exit 1: missing required C2/Health BIT metadata capability
build/dev/loader_tests /home/zboll/git/ams-gra-gige-mel-provider/build/dev/libgige_mel_provider.so --bit-only
# exit 0: catalogue, successful execution, separate completed result, Health faults
```

The probe prints both requested provider path and **dladdr-resolved factory DSO**;
both identified the selected baseline or feature library respectively. Feature
expectation failure is specifically absent BIT support, not unrelated loading or
configuration failure. Baseline capability, registration and valid-command absence
are separately positively checked.

## Delivery and hosted evidence

The final commit/tree, remote/PR head agreement and final-revision push and PR job
URLs/status are recorded in the PR handoff after publication (a commit cannot
contain its own identity). Local results above are executed evidence, not claims
that hosted checks ran. Use HOSTED_CI_PENDING while required final-revision jobs
are queued/running; only matching-head successes support READY_FOR_REVIEW.

Hardware BIT, physical connectivity/packet/firmware/memory/calibration tests,
asynchronous cancellation and physical diagnosis remain explicitly outside this
increment. Deferred PR #3 diagnostic-fallback and BOM notes remain separate.

## R1 correction: BIT test synchronization and receive-gate cleanup

Independent review of PR #5 head `9e52bad625801f5aa05a9966a46f25f45da32f76`
(tree `47c50dbe1cff23b1875da0e380b3a6814bdf26fa`; main at review
`72e8dc41a43cb83b45584b01c5781225fca73b44`, tree
`59c38ad44df1e6a208b46f01ac11dc52e4197cae`) found two **test** defects and no
blocking production BIT defect. R1 started from that exact fetched branch head
(no intervening changes) and changes only `tests/provider_bit_tests.inc`, two
scenario registrations in `tests/provider_tests.cpp`, and this record. Production
source, public headers, pinned dependencies, backend interfaces, factory
signatures, CMake and the CI workflow/matrix are unchanged from `9e52bad`.
The historical results above remain historical: the original passing suite did
**not** prove explicit receive-gate cleanup or Health payload freshness.

### Finding 1: Health callback count did not establish payload freshness

`bit_health()` sent a synchronous BIT command and then waited for `count + 1`
Health BIT reports. The Health worker captures a batch (Status, Detailed, BIT)
from one state sample and dispatches it afterward. A batch captured before the
command can legally be delivered after the command returns, so a larger callback
count says nothing about whether the payload contains the new completion. The
later re-enable step reused that old `count` with `count + 2`; reports delivered
before disable could already satisfy it, so no report from the new enable
interval was required.

Correction: `BitReports::find_completed()/wait_completed()` wait, bounded, under
the collector mutex for a report whose single completion has the expected test
UCI identity and Pass result, copy that report while synchronized, and only then
run the full `check_completion()`. This is valid because the scenario begins with
empty history and performs exactly one accepted Pass execution. It uses no
invented command ID in `BIT_Status` and no assumption that UTC timestamps
increase. After disable and the existing quiescence check, a **new** count
baseline is captured; re-enable must deliver a report beyond it, and that first
new-interval report must contain the retained completion. Immediate publication
after enable is permitted. No sleeps, extra-report counts or longer timeouts are
substituted for the predicate. Callback-thread, disable/detach, admission and
reattachment coverage are unchanged.

New permanent regression `bit_health_stale_batch` forces the legal ordering with
public callbacks and watchdog gates: initial BIT report with empty history; the
second MFA_Status is held after its batch was captured and before its BIT
callback; the synchronous C2 command succeeds; the Status gate is released; the
old batch's BIT callback records/notifies and is then held outside the collector
mutex. The test then directly asserts the counterexample: the old `count + 1`
target is satisfied while the delivered report has no completion, and the
corrected predicate rejects it. After release, the payload-based wait positively
finds a later report with the expected completion. The stale report is delivered
normally; Health scheduling is not changed. The re-enable half asserts the stopped
count already satisfies the old `count + 2`, then requires a report beyond the
stopped baseline containing the completion. The gated S/D/B prefix order is
asserted; later disables may legally suppress a suffix. Gates open on every
cleanup path before Health is disabled; collectors outlive callbacks; callback
observations are asserted on the test thread.

### Finding 2: no-I/O scenario silently consumed its receive watchdog

`bit_no_io()` held the scripted receive gate to measure BIT/Health with a delivered
buffer outstanding, but never called `plan->open()`. Its synchronous Image disable
called `request_stop()`, which sets `stop_requested` and notifies but cannot satisfy
the `proceed` predicate. The worker therefore waited for the two-second watchdog,
set `gate_expired`, and returned `Test receive gate expired`. Because provider
cancellation had already been observed, that was classified as requested shutdown
and the test still passed without checking expiry or `wait_for_exit()`.

Correction (`gated_no_io_scenario`, used by `bit_no_io`): keep the worker gated
through catalogue registration, C2 status, BIT initiation and three Health BIT
reports. Complete the backend-call, configuration-generation and retained-byte
checks **before** shutdown, which legitimately calls the backend. Then start Image
disable asynchronously, wait for `ReceivePlan::stop_requested`, explicitly
`plan->open()`, require disable to complete within a bounded success deadline and
return Success, and call `plan->wait_for_exit()` (worker exit plus gate-expiry
rejection). Under `plan->mutex`, after established exit, `proceed` must be true
and `gate_expired` false. `stream_failures` must not increase, showing this
controlled requested shutdown is not reclassified as a stream fault. Observing
the stop request before opening the gate preserves that meaning. The delivered
image remains byte-identical and retained, restart fails while it is outstanding,
explicit release succeeds, and a fresh acquisition starts and stops.

Failure cleanup: a `TestCleanup` guard is installed before the first Image enable.
Plan, fixture, collectors, Health handle and the disable future are declared
before it. It opens the receive gate before waiting on any disable future or
allowing Fixture destruction to join the worker, then disables Health while its
collector is alive. It never throws or relies on the watchdog. New permanent
regression `bit_no_io_unwind` throws a test-local exception after Health is
enabled and while receive is gated, catches it, and then verifies with the
externally owned plan that cleanup released the gate and the worker exited without
expiry.

### Before/after controls actually executed

Control implementation: disposable detached worktree
`/tmp/gige-mel-005-r1-control` at reviewed commit `9e52bad`, tree `47c50db…`.
Its production/CMake/header/third-party sources were verified equal to `9e52bad`;
one ordinary dev build was made and `ldd` confirmed `provider_tests` loaded that
worktree's `libgige_mel_provider.so.0`. Differences from `9e52bad`, test code only:
the corrected `provider_bit_tests.inc` and registrations were copied in, plus an
uncommitted `tests/r1_control_probes.inc` with two diagnostics registered first.
This is therefore **not** an unchanged test suite.

- No-I/O control: the reviewed `bit_no_io()` body verbatim, plus a no-expiry
  assertion after its first Image disable. Result: **FAIL** at `!expired`, printing
  `gate_expired=1 proceed=0 first_disable_ms=1798`.
- Health control: the controlled stale-batch sequence followed by the reviewed
  count-only observation. Result: **FAIL** in `check_completion(reports.last())`
  (`getCompletedBITs().size() == 1`), printing `count=1 delivered=2
  last.completedBITs=0`.
- Corrected scenarios against the same unchanged reviewed provider: `bit_no_io`,
  `bit_no_io_unwind`, `bit_health_stale_batch` and corrected `bit_health` all
  **PASS**.

No production defect was fixed; the reviewed provider already permitted the
correct behavior. The diagnostics stay only in the disposable worktree, so normal
CTest has no expected timeout or unhandled failure. Removing the expiry also
shortened the provider contract run by about 1.2–1.8 seconds.

### R1 local validation

The provider suite now has **62 scenarios**: the previous 60 (including all
Health R1 regressions) plus `bit_no_io_unwind` and `bit_health_stale_batch`.
Focused corrected scenarios and controls ran first. Development, Release,
ASan/UBSan (`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`) and TSan
(`TSAN_OPTIONS=halt_on_error=1`) configure/build (`--parallel 2`) and full CTest:
**6/6 each**, including the native loader. Five additional bounded repetitions
passed in the development build (both `BIT no` scenarios and all five `BIT Health`
scenarios, including corrected `bit_health` and `bit_health_stale_batch`) and
five under TSan (both `BIT no` scenarios and `bit_health_stale_batch`).
Both offline dependency verifiers and `git diff --check` passed. No local
sanitizer environment restriction occurred. Local `clang++` is still not
installed; hosted Clang remains part of qualification. No hardware/SDK was
exercised; the software-only BIT boundary is unchanged.