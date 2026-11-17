# GIGE-MEL-042 validation record

The **current qualification** is GIGE-MEL-042-R1, the first section below. The
original GIGE-MEL-042 record follows it unchanged as history. Its results
qualify `ab66da1` only, and the R1 review superseded them.

## GIGE-MEL-042-R1 correction qualification

### R1 source

* **Reviewed head:** `85edf354a1567f8e1c5c7f9dafa9d1e0be20d947`. Disposition:
  `CORRECTION_REQUIRED`. Two blockers were found:
  * the branch conflicted with `main`;
  * a CommandStatus admission race.
* **Incorporated `main`:** fetched `origin/main`
  `c6d555d4e841eb8ed576615d7716cb54bfebfb28` (PR #39, GIGE-MEL-GENTL-001).
  * Normal merge commit `f3070a081adcac0bed889ec9897db0d451c1cfdf`, with
    parents `85edf35` and `c6d555d`. No rebase, reset or force push.
  * Only `docs/implementation-profile.md` and `docs/mel-surface-roadmap.md`
    conflicted, both in their top sections. Each resolution keeps the
    GIGE-MEL-042 entry and the GenTL entry.
  * `CMakeLists.txt` merged without conflict. It keeps the default-OFF
    `GIGE_MEL_BUILD_GENTL_PROBE`, `gentl_default_isolation` and the GIGE-MEL-042
    `native_loader_c2_unsupported`.
* **Qualified corrected source:** `7fc28e734108203b4c4f4a936afa20568bb91767`,
  tree `97ac319c8bc20c2b2773fe52731efd2dea5f71c4`, clean worktree. `origin/main`
  is an ancestor.
* **Recording:** these results are committed afterwards as a documentation-only
  change to this file.

### R1 correction

`reject_unsupported()` now snapshots and reserves (`admit_callback`) the
registered CommandStatus callback in the same `Runtime::operations` critical
section as the detach check. It then invokes the reserved callback outside every
provider lock. The full contract is in [design §3.6](GIGE-MEL-042-design.md).

`ChannelCore::invoke()` now releases its reservation through a scope guard.
The only other changes are:

* the test-only seam header `src/provider_test_seams.hpp`, never installed;
* an uninstalled `EXCLUDE_FROM_ALL` seam build of the same provider sources.

| Property | Evidence |
| --- | --- |
| Installed library has no seam | All four installed consumers: `seam_files=0`, `seam_symbols=0` (`nm -DC` on the installed `libgige_mel_provider.so`) |
| No public/installed API change | `include/` unchanged versus `origin/main`. Protected-path diff: `src/provider.cpp`, `src/provider_test_seams.hpp` |
| Signatures, futures, descriptions, capabilities | Unchanged. All existing C2-unsupported provider groups and `native_loader_c2_unsupported` pass in all nine lanes |

### Deterministic race regression (`provider_status_admission`)

A thread-local hook holds one submitting thread at a chosen point:

* `before_admission`: after the callback-context and Control checks;
* `after_reservation`: once the callback is reserved and every lock is
  released.

Barriers are condition variables, and every wait has a bounded watchdog (5 s
hang bound, 2 s for operations that may legitimately finish). No sleep decides
an outcome. The only fixed interval is a 100 ms window in which a pending detach
must **not** complete; a correct provider cannot complete it at any scheduling.

| Scenario (×5 commands unless noted) | Proven |
| --- | --- |
| **Admission wins** | Sender held after reservation. Another thread attaches and detaches Health and attempts a duplicate registration, all of which complete, so neither `operations` nor `callback_mutex_` is held. Detach begins (the detached flag becomes observable) but stays pending. A request arriving then gets `InvalidState`. After release, exactly one `Rejected`/`CapabilityUnavailable` status with the exact ID and description arrives on the sender's thread, the future is ready `Unsupported`, and detach completes |
| **Detach wins** | Sender held before admission. Detach completes promptly. The released request and a later one both get ready `InvalidState` ("channel is detached"), the after-reservation point is never reached, and no status is published |
| Reserved throwing callback (×1) | Detach stays pending until release. The exception is contained (`callback_failures` +1) and the reservation is released, so detach completes |
| Admitted, no callback registered (×1) | Nothing reserved, so a held request does not delay detach. The future is still `Unsupported` |

Result: 12/12 scenarios pass, about 0.6 s per lane, in every lane of the
matrix, including ASan/UBSan and TSan with zero reports.

### R1 mutation controls

Run in a disposable worktree against the corrected sources. Each entry shows
the exit codes of the seam test and of `provider_tests 'C2 unsupported'`. A
`124` is the 60 s timeout, meaning the run hung.

| Mutation in `src/provider.cpp` | Build | Seam test | Existing provider groups |
| --- | --- | --- | --- |
| Baseline (corrected source) | 0 | 0 Pass | 0 Pass |
| **Prior ordering:** reserve after releasing `operations` (the `85edf35` defect) | 0 | **1** (admission wins: detach finished during the reserved interval) | 0 Pass. The existing suite could not detect the race |
| Reserved, then invoked as unadmitted (double count) | 0 | **124** (detach never drains) | **124** |
| Reservation leaked when the callback throws | 0 | **124** | **124** |
| Detach check removed under `operations` | 0 | **1** | **1** (lifecycle) |
| Callback invoked while holding `operations` | 0 | **1** (locks held during reservation) | **1** (lifecycle watchdog) |
| Reservation taken with no registered callback | 0 | **124** | **124** |
| Restored (byte-identical) | 0 | 0 Pass | 0 Pass |

All six mutants are detected. Only the new deterministic test detects the
original defect. The worktree was removed.

### R1 nine-lane matrix (one final-source run on `7fc28e7`)

The method and environment are identical to the original record below. Lanes
use the same SDKs, with Debug, `--parallel 2` and `ctest -j1`.

| Lane | CTest | `provider_contract` | `provider_status_admission` | `native_loader_c2_unsupported` | Sanitizer reports |
| --- | --- | --- | --- | --- | --- |
| GCC (SDK-free) | 33/33 | Passed 13.95 s | Passed 0.61 s | Passed | n/a |
| ASan/UBSan (SDK-free) | 33/33 | Passed 14.30 s | Passed 0.63 s | Passed | 0 |
| TSan (SDK-free) | 33/33 | Passed 14.25 s | Passed 0.63 s | Passed | 0 |
| Clang (SDK-free) | 33/33 | Passed 14.00 s | Passed 0.61 s | Passed | n/a |
| Aravis-only | 41/41 | Passed 14.00 s | Passed 0.61 s | Passed | n/a |
| Pleora-only | 39/39 | Passed 14.01 s | Passed 0.61 s | Passed | n/a |
| Combined | 47/47 | Passed 14.00 s | Passed 0.61 s | Passed | n/a |
| Combined ASan/UBSan | 47/47 | Passed 14.17 s | Passed 0.63 s | Passed | 0 |
| Combined TSan | 47/47 | Passed 14.21 s | Passed 0.62 s | Passed | 0 |

Each lane has two tests more than the original record:

* `gentl_default_isolation`, from `main`;
* `provider_status_admission`, new in R1.

No test was skipped, timed out or reported as not run.

| Check | Result |
| --- | --- |
| Installed consumer (GCC, Clang, Pleora, combined) | Exit 0, 176 files, no SDK library dependency, no audit/test artifact, no seam file or symbol |
| Package isolation (Aravis, combined) | Exit 0, "package isolation verified" |
| GenTL default isolation (`gentl_default_isolation`) | Passed in all nine lanes |
| GenTL opt-in probe lane (`-DGIGE_MEL_BUILD_GENTL_PROBE=ON`, provisioned checksum-verified EMVA GenTL 1.6 header, `ctest -L gentl`) | 41/41 passed. The fixture only: no real producer or camera |
| Baseline control: `--expect-no-c2-unsupported-status` against starting-main (`ae4a237`) library | Exit 0 |
| Baseline discrimination: `--c2-unsupported-only` against that library | Exit 1, "unsupported requests did not each publish one CommandStatus" |
| Feature control: `--c2-unsupported-only` against the qualified library | Exit 0 |
| Proprietary content guard (`--history origin/main`) | 365 Git entries, no prohibited material |
| Upstream / nlohmann-json integrity | 97 / 2 pinned files verified |
| Documentation links | 69 files, 362 links, 0 failures |
| `git diff --check origin/main...HEAD` | Clean |
| Scoped Pleora GenApi TSan exception | Unchanged versus `origin/main`. `pleora_tsan_exception_guard` and `pleora_sdk_contract` passed in combined TSan |

Before the matrix, a focused GCC build ran these checks with all passing:

* `provider_status_admission_tests`: 12/12;
* the full `provider_tests` (111);
* the C2 loader.

### R1 hosted CI

GitHub Actions `C++ provider` for `7fc28e7`:

* push run `44024346011`: 5/5 jobs succeeded;
* pull-request run `44024351971`: 5/5 jobs succeeded.

Jobs: g++ dev, g++ release, clang++ dev, asan, tsan. Hosted CI does not provision
SDKs or the GenTL header, so those lanes are local only.

After the push, GitHub reported PR #41 `MERGEABLE` against `main` `c6d555d`.

## Original GIGE-MEL-042 qualification (superseded by R1)

### Source

* **Base:** fetched `main` `ae4a2377c05e7dd16becf9a5201a5a075163808b`, tree
  `f9fdc16684c273cf4098793750f30efd6f3553c3`.
* **Branch:** `feature/gige-mel-042-c2-command-status`.
* **Qualified source commit:** `ab66da19874bde0e055c133ecd5ca49c0cc4a5ae`, tree
  `62bd385dba843decc7fc656274ad70ade60a0b4e`, with a clean worktree. These results
  are committed afterwards as a documentation-only change to this file, so this
  record does not name its own commit.

### What is qualified

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

### Matrix method

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

### Results

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

### Mutation controls

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

### Hosted CI

GitHub Actions `C++ provider` ran for the qualified commit:

* push run `44017316435`: 5/5 jobs succeeded (g++ dev, g++ release, clang++
  dev, asan, tsan);
* pull-request run `44017464601`: 5/5 jobs succeeded.

### Not qualified

* No camera hardware, GigE network or real device was used. The Aravis and
  Pleora lanes use the repository's simulator and contract tests only.
* Calibration, camera protection, erase and system-track input remain
  unimplemented and unadvertised. A rejection status is not a capability.
* `DiscreteStatus` and `MFA_SecurityAuditRecord` remain
  `BLOCKED_BY_UPSTREAM_CAPABILITY_ENUM`, as recorded by
  [GIGE-MEL-041](GIGE-MEL-041-design.md).
* The provider remains a partial bootstrap. This is not MEL conformance.
