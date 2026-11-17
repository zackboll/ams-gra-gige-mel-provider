# GIGE-MEL-042 validation record

## Source

* **Base:** fetched `main` `ae4a2377c05e7dd16becf9a5201a5a075163808b`, tree
  `f9fdc16684c273cf4098793750f30efd6f3553c3`.
* **Branch:** `feature/gige-mel-042-c2-command-status`.
* **Qualified source commit:** `ab66da19874bde0e055c133ecd5ca49c0cc4a5ae`, tree
  `62bd385dba843decc7fc656274ad70ade60a0b4e`, with a clean worktree. These results
  are committed afterwards as a documentation-only change to this file, so this
  record does not name its own commit.

## What is qualified

This validation covers the [design](GIGE-MEL-042-design.md) contract. The five
unsupported C2 commands (`CalibrationConfigurationCmd`, `CalibrationStatusCmd`,
`CameraProtectCmd`, `EraseCommand`, `SystemTrackDataResponse`) return ready,
typed `Unsupported` futures. Once admitted, each also publishes one correlated
`Rejected`/`CapabilityUnavailable` `CommandStatus` through the existing C2
callback contract.

The only production change is in `src/provider.cpp`. Nothing under `include/`,
`third_party/`, `tools/`, `cmake/`, SDK adapters or camera profiles changes. No
capability is added or advertised.

It does not qualify calibration, camera protection, erase, tracking or MEL
conformance.

## Matrix method

The method is identical to GIGE-MEL-041. Each lane is configured with ordinary
CMake and Ninja in Debug, built with `--parallel 2`, and run once with
`ctest -j1 --output-on-failure`.

Sanitizer and loader environment:

* `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`
* `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`
* `TSAN_OPTIONS=halt_on_error=1`
* `PKG_CONFIG_PATH` points at the external Aravis 0.8.36. Hardware options are
  OFF.

Pleora uses external eBUS 6.5.3.7155 with its matching GenApi headers. The
existing scoped GenApi TSan audit (`pleora_sdk_contract` in combined-tsan) is
unchanged, and no exception is added or widened. No physical-camera test was
executed.

## Results

Executed locally on 2026-11-16 against the qualified source commit, as one
final-source run.

| Lane | CTest | `provider_contract` | `native_loader_c2_unsupported` | Sanitizer reports |
| --- | --- | --- | --- | --- |
| GCC (SDK-free) | 31/31 | Passed 13.90 s | Passed | n/a |
| ASan/UBSan (SDK-free) | 31/31 | Passed 14.23 s | Passed | 0 |
| TSan (SDK-free) | 31/31 | Passed 14.23 s | Passed | 0 |
| Clang (SDK-free) | 31/31 | Passed 13.99 s | Passed | n/a |
| Aravis-only | 39/39 | Passed 13.90 s | Passed | n/a |
| Pleora-only | 37/37 | Passed 13.90 s | Passed | n/a |
| Combined | 45/45 | Passed 13.90 s | Passed | n/a |
| Combined ASan/UBSan | 45/45 | Passed 14.19 s | Passed | 0 |
| Combined TSan | 45/45 | Passed 14.22 s | Passed | 0 |

Each lane has one test more than the GIGE-MEL-041 baseline: the new
`native_loader_c2_unsupported`. No test was skipped, timed out or reported as
not run.

The six new provider groups add about 0.3 s, so `provider_contract` remains
within its 30 s timeout. Before the matrix, the same six groups and the loader
mode also passed in focused ASan/UBSan and TSan builds with zero sanitizer
reports.

Additional checks:

| Check | Result |
| --- | --- |
| Installed consumer (GCC, Clang, Pleora, combined) | Exit 0, 174 installed files, no SDK library dependency, no audit/test artifact installed. Versus 041 the only additions are the two GIGE-MEL-042 documents |
| Package isolation (Aravis, combined) | Exit 0, "package isolation verified" |
| Baseline control: feature loader `--expect-no-c2-unsupported-status` against an unchanged starting-main (`ae4a237`) library | Exit 0. Starting main returns Unsupported for all five with **no** CommandStatus |
| Discrimination: feature loader `--c2-unsupported-only` against the starting-main library | Exit 1, "unsupported requests did not each publish one CommandStatus" (run during development) |
| Feature control: `--c2-unsupported-only` against the qualified library | Exit 0 |
| Proprietary content guard (`--history origin/main`) | 341 Git entries, no prohibited material |
| Upstream integrity | 97 pinned files verified |
| nlohmann/json integrity | 2 pinned files verified |
| Documentation links | 67 files, 353 links, 0 failures |
| `git diff --check origin/main...HEAD` | Clean |
| Protected paths changed | `src/provider.cpp` only |
| Scoped Pleora GenApi TSan exception (`tests/support/pleora_genapi_tsan.supp`, `scripts/pleora_tsan_exception.py`) | Unchanged. `pleora_tsan_exception_guard` and `pleora_sdk_contract` passed in combined TSan |

## Mutation controls

Each mutation was applied to `src/provider.cpp` in a disposable worktree, which
was then built and run through `provider_tests 'C2 unsupported'` and the
`--c2-unsupported-only` loader:

| Mutation | Build | Provider tests | Loader |
| --- | --- | --- | --- |
| Baseline | 0 | Pass | Pass |
| No status published | 0 | **Fail** (correlation) | **Fail** |
| Status `Accepted` | 0 | **Fail** (correlation) | **Fail** |
| Reason `Other` | 0 | **Fail** (correlation) | **Fail** |
| Command ID truncated to 31 bits | 0 | **Fail** (correlation) | **Fail** |
| Status published twice | 0 | **Fail** (correlation) | **Fail** |
| Callback-context check removed | 0 | **Fail** (exit 139: unbounded recursive status reentry) | Pass |
| Detached check removed | 0 | **Fail** (lifecycle) | **Fail** |
| Closing-Control rejection removed | 0 | **Fail** (lifecycle) | Pass |
| Status dispatched under `operations` | 0 | **Fail** (lifecycle watchdog) | Pass |
| Backend readback during rejection | 0 | **Fail** (no side effects) | Pass |
| Rejection recorded as `last_error` | 0 | **Fail** (correlation) | Pass |
| `SystemTrackDataResponse` returns a successful status payload | 0 | **Fail** (correlation) | **Fail** |
| `EraseCommand` description reverted | 0 | **Fail** (correlation) | Pass |
| `CameraProtectCmdResp` advertised | 0 | **Fail** (correlation) | **Fail** |
| Restored baseline (source restored byte-for-byte) | 0 | Pass | Pass |

Every mutation built, so none was invalid, and every one was caught by the
provider tests. The mutation run used the then-uncommitted feature sources,
which are identical to the qualified commit's `src/provider.cpp` and test files.
The loader is a public-surface smoke check and is not expected to observe
internal lifetime or I/O states. The disposable worktree has been removed.

## Hosted CI

GitHub Actions `C++ provider` ran for the qualified commit:

* push run `44017316435`: 5/5 jobs succeeded (g++ dev, g++ release, clang++
  dev, asan, tsan);
* pull-request run `44017464601`: 5/5 jobs succeeded.

## Not qualified

* No camera hardware, GigE network or real device was used. The Aravis and
  Pleora lanes use the repository's simulator and contract tests only.
* Calibration, camera protection, erase and system-track input remain
  unimplemented and unadvertised. A rejection status is not a capability.
* `DiscreteStatus` and `MFA_SecurityAuditRecord` remain
  `BLOCKED_BY_UPSTREAM_CAPABILITY_ENUM`, as recorded by
  [GIGE-MEL-041](GIGE-MEL-041-design.md).
* The provider remains a partial bootstrap. This is not MEL conformance.
