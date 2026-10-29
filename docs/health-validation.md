# GIGE-MEL-004 validation record

Starting origin/main (fetched before branching):

- Commit `1349fd3bdc265398750ba2c8ec92928124c6e215`
- Tree `256b4426f775a1e4c35e30e0e98a78e4a36540c2`
- Feature branch `feature/gige-mel-004-health-status`

Local execution: Linux 6.12.113+deb13-amd64, GCC 14.2.0. Only synthetic
and test-decorator sources were exercised; **no hardware or SDK qualification**.

## Executed checks

- `python3 scripts/vendor_upstream.py --verify`: 97 pinned files unchanged.
- `python3 scripts/verify_json.py`: two pinned parser files unchanged.
- `cmake --preset dev`, build with `--parallel 2`, `ctest --preset dev`:
  all six CTest tests passed, including 43 provider scenarios and retained
  ConfigSet public probe.
- Provider contract bounded repetition: five consecutive passes. Gates/promises
  establish callback entry, drain, faults and lifetime completion; repetition
  supplements rather than replaces these synchronization barriers.
- `asan` configure/build/full CTest with
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`: six tests passed.
- `tsan` configure/build/full CTest with `TSAN_OPTIONS=halt_on_error=1`:
  six tests passed. No local sanitizer initialization/environment limitation.
- Native loader: public MEL Health reports plus registered Mono8 delivery passed.
- Native capture: 64x48 Mono8 PGM generated in the ignored build tree.
- Install to a temporary prefix, configure/build/run separate installed
  consumer: published-header Health reports and ConfigSet/CommandStatus passed.
- `git diff --check`: passed. No upstream declaration, backend public interface,
  factory signature or CI matrix changes.

An additional GCC Release build initially exposed an optimized destructor-path
abort (also reproduced in a fresh build tree and the existing ConfigSet probe).
Tracing showed the Health destructor reached during C2 teardown. Auxiliary
cleanup was moved out of the base-destructor-reachable stop function into the
explicit disable/detach paths, with Health's own defensive destructor cleanup
retained. The corrected Release full CTest suite passes 6/6. Development and
both sanitizer suites were rerun after this correction. This was treated as a
real validation failure to correct, not dismissed as an environment limitation.
Optional local Clang execution was unavailable because clang++ is not installed;
the unchanged hosted Clang job remains the compiler validation gate.

## Independently built baseline negative control

A disposable detached worktree at the exact starting commit/tree above was
configured/built without modifying its sources. `git status --short` was empty.
The **same feature-built public-header loader executable** was run against the
baseline and feature DSOs:

```sh
build/dev/loader_tests /tmp/gige-mel-004-baseline/build/dev/libgige_mel_provider.so --expect-no-health
# exit 0: baseline Health absent and not attachable (including UUID final byte 3)
build/dev/loader_tests /tmp/gige-mel-004-baseline/build/dev/libgige_mel_provider.so --health-only
# exit 1: missing required HealthAndStatus discovery capability
build/dev/loader_tests build/dev/libgige_mel_provider.so --health-only
# exit 0: discovers/attaches Health and receives both current reports
```

The intended missing capability, not a baseline code modification or unrelated
configuration failure, explains the feature expectation's baseline failure.

Hosted CI is tracked separately on the PR for the pushed revision. The existing
five jobs (GCC dev/release, Clang dev, ASan/UBSan, TSan) remain unchanged; local
execution is not represented as hosted success. Final commit/tree, job URLs and
conclusions are recorded in the PR handoff after pushing.

## GIGE-MEL-004-R1 correction evidence

The earlier passing checks above did **not** detect the teardown publication
window or establish real stop entry in the original periodic shutdown tests.
These are new regressions/corrections, not a reinterpretation of those results.

Fetched starting PR revision (unchanged from review):

- Commit `9501f71a508afd5c251db7461183efe7b0a25d96`
- Tree `514afebf56a06d96406cff8bc5fd9888943ddaaa`
- Fetched main remains commit `1349fd3bdc265398750ba2c8ec92928124c6e215`,
  tree `256b4426f775a1e4c35e30e0e98a78e4a36540c2`.

### Failed-acquisition teardown: unchanged reviewed PR versus correction

A disposable detached worktree `/tmp/gige-mel-004-r1-reviewed` at the exact
reviewed PR revision was configured and its provider DSO built. Its source
checkout remained clean; no baseline code or test changes were made. `ldd` with
the explicit library path confirmed the new test executable used that DSO:

```sh
LD_LIBRARY_PATH=/tmp/gige-mel-004-r1-reviewed/build/dev \
  build/dev/provider_tests 'teardown publication'
# exit 1: During gated destruction of a terminally failed stream:
#         FALSE Operate/running observed

build/dev/provider_tests 'teardown publication'
# corrected DSO: exit 0, PASS Health failed acquisition teardown publication
```

This negative control uses the reviewed **Health-capable PR**, not main. The
same new scenario first observes a Degraded snapshot after unexpected stopped,
gates its Status callback, starts Image disable and waits for stream destructor
entry. It releases Status while destruction remains gated, then requires a
subsequent Status **and** Detailed (index >= 1). The original companion at
Detailed[0] is explicitly checked as faulted and never mistaken for freshness.
Fresh reports must be Standby/idle with stream_failures=1 and preserved error
history. Disable remains pending until destructor release; destruction is
verified on the cleanup thread, not the Health callback thread. Keep-alive and
re-enabled reporting verify Health remains usable afterward.

New gates have bounded watchdogs with explicit timeout-failure checks. Scope
cleanup opens gates and waits/stops/detaches before callback observers,
synchronization objects or backing image storage can be destroyed on assertion
unwind. No backend work is added to the publisher.

### Cleanup invariant and enable exception source review

Normal stop retains `stream_state_` through request_stop, worker join/self-detach
and stream stop. With the owned-stream guard intact, it clears image_active and
the weak acquisition together under state_mutex, releases the mutex, then resets
the strong owner. This prevents an expired publication being read as running
and prevents a Health snapshot from becoming the last owner during ordinary
external cleanup. C2/Health stop cannot clear Image publication. No owning
Runtime acquisition reference/cycle is introduced. The earlier Release
auxiliary-destructor correction is retained unchanged.

The Image enable catch path was explicitly reviewed at the post-publication
thread-construction failure point: local `state` still strongly owns the stream;
stream stop is outside state_mutex; both image_active and acquisition are cleared
under the mutex **before** stream_state_ is reset or local state leaves scope.
The existing diagnostic and Fail return are retained. There is no existing
private mechanism forcing this point; no resource-exhaustion experiment,
production thread factory or shipped fault-injection switch was added. This
case is source-review evidence, not a claimed executed post-publication failure.

### Callback ordering and actual shutdown milestones

The general ordering assertion now checks every entry alternates S,D, with at
least three completed pairs in the pre-registered-both case. An optional final S
is accepted, while inverted/duplicate entries are not. Repeated enable remains
idempotent and post-disable quiet checks remain. Runtime admission is unchanged.

The targeted shutdown test deliberately gates the fourth Status after three
complete pairs. For detach, empty supported registration returning Fail observes
detached admission; for disable, a stopping-thread-local test-executable
pthread_join observer observes **actual join entry after periodic closure**.
Image is inactive and only the disabling async thread is armed. Notification is
nonthrowing and once-only; the forwarding symbol is resolved and validated before
arming. RTLD_NEXT forwarding targets recorded locally were libc.so.6 (dev),
libasan.so.8 (ASan/UBSan) and libtsan.so.2 (TSan), so sanitizer interceptors are
not bypassed. No public API, production hook or workflow-matrix change is used.
With stop demonstrably entered, its future must remain pending while Status is
gated. After release, both detach/disable produce and accept exactly `SDSDSDS`,
suppress the unadmitted Detailed, finish and remain periodically quiet.

The separate scheduler-delay counterexample runs both disable and detach:
the task fulfills submitted, then enters allow_stop_call before invoking stop.
While that gate remains closed and the stop future pending, releasing the Status
gate yields Detailed. Only afterward is stop allowed to run and complete. This
deterministically disproves the former submitted-plus-timeout premise without
sleep-based scheduling evidence.

### Newly executed local validation

- Both offline dependency verifiers: passed; pinned bytes unchanged.
- dev and Release configure/build (`--parallel 2`), full CTest: **6/6 each**.
- Provider suite: **45 scenarios**, including all previous 43 and the new
  teardown and scheduler-delay scenarios.
- ASan/UBSan full CTest: **6/6**, with leak detection and halt-on-error options.
- TSan full CTest: **6/6**, with halt-on-error. Focused shutdown tests also passed
  under both sanitizers and printed the forwarding targets above.
- Public-header native loading/Health/Mono8 delivery, capture, temporary-prefix
  install and separately built installed Health + ConfigSet/status consumer:
  passed. Generated image/build artifacts remain outside commits.
- No sanitizer initialization limitation observed. Local clang++ is still not
  installed; no local Clang result is claimed. Hosted Clang remains unchanged.
- No hardware or SDK qualification; deferred PR #3 diagnostic-fallback and BOM
  notes remain deferred. No unrelated parser/status corrections.

Final correction commit/tree and **final-revision** hosted job links/results are
recorded in the existing PR #4 handoff; old-head CI is not correction evidence.