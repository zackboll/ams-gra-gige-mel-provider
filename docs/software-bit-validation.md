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