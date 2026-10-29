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