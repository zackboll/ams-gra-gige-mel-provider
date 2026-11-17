# GIGE-MEL-041 validation record

## Source

* **Base:** fetched `main` `60e1fbd3c4df188b51c5d87c832c294991f5da4c`, tree
  `9a2c9853102cc6538bf00f73bdfa05036de6743a`. PR #36 is merged.
* **Branch:** `feature/gige-mel-041-required-health-metadata`.
* **Qualified source commit:** `e3a07b1695e7ee934f63d2e52c6cedd48477f61a`, tree
  `e2831bd96cbe3b00feb86fc7b4b9760128d09c34`, with a clean worktree. These results
  are committed afterwards as a documentation-only change to this file, so this
  record does not name its own commit.

## What is qualified

There is **no production behavior change**: nothing under `src/`, `include/`,
`third_party/` or `tools/` changes. This validation covers three things:

* the executable classification of `DiscreteStatus` and `MFA_SecurityAuditRecord`
  as `BLOCKED_BY_UPSTREAM_CAPABILITY_ENUM`, described in the
  [design](GIGE-MEL-041-design.md);
* that capability advertisement is unchanged;
* that the existing Health contract does not regress.

It does not qualify `DiscreteStatus`, `MFA_SecurityAuditRecord`,
`HealthStatusChannel` or MEL conformance.

## Matrix method

Each lane is configured with ordinary CMake and Ninja in Debug, built with
`--parallel 2`, and run with `ctest -j1 --output-on-failure`.

Sanitizer and loader environment:

* `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`
* `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`
* `TSAN_OPTIONS=halt_on_error=1`
* `PKG_CONFIG_PATH` points at the external Aravis 0.8.36. Hardware options are
  OFF.

The nine lanes are GCC, ASan/UBSan, TSan, Clang, Aravis-only, Pleora-only, and
combined Debug, ASan/UBSan and TSan. Pleora uses external eBUS 6.5.3.7155 with its
matching GenApi headers. The existing scoped GenApi TSan audit
(`pleora_sdk_contract` in combined-tsan) is unchanged, and no exception is added
or widened.

## Results

Executed locally on 2026-11-15 against the qualified source commit. GitHub CI
is configured but had not been executed when this record was written.

| Lane | CTest | `provider_contract` | `native_loader_required_metadata` | Sanitizer reports |
| --- | --- | --- | --- | --- |
| GCC | 30/30 | Passed 13.68 s | Passed | n/a |
| ASan/UBSan | 30/30 | Passed 13.87 s | Passed | 0 |
| TSan | 30/30 | Passed 13.91 s | Passed | 0 |
| Clang | 30/30 | Passed 13.58 s | Passed | n/a |
| Aravis-only | 38/38 | Passed 13.68 s | Passed | n/a |
| Pleora-only | 36/36 | Passed 13.57 s | Passed | n/a |
| Combined | 44/44 | Passed 13.57 s | Passed | n/a |
| Combined ASan/UBSan | 44/44 | Passed 13.85 s | Passed | 0 |
| Combined TSan | 44/44 | Passed 13.87 s | Passed | 0 |

Each lane has one test more than the GIGE-MEL-040 baseline: the new
`native_loader_required_metadata`. No test was skipped, timed out or reported as
not run. `provider_contract` grew from about 12 s to about 13.9 s, which is within
its 30 s timeout.

Additional checks:

| Check | Result |
| --- | --- |
| Installed consumer (GCC, Clang, Pleora, combined) | Exit 0, 172 installed files, no SDK library dependency, no audit/test artifact installed |
| Package isolation (Aravis, combined) | Exit 0, "package isolation verified" |
| Baseline control: feature loader against an unchanged `origin/main` provider library | Exit 0. The classification holds for the existing provider, confirming no production change is needed |
| Proprietary content guard | 337 Git entries, no prohibited material |
| Upstream integrity | 97 pinned files verified |
| nlohmann/json integrity | 2 pinned files verified |
| Documentation links | 65 files, 339 links, 0 failures |
| `git diff --check origin/main...HEAD` | Clean |
| Protected paths (`src`, `include`, `third_party`, `tools`, `tests/support`, `cmake`, `camera_profiles`, `examples`, `scripts`) | 0 files changed |
| Scoped Pleora GenApi TSan exception (`tests/support/pleora_genapi_tsan.supp`, `scripts/pleora_tsan_exception.py`) | Unchanged. `pleora_tsan_exception_guard` and `pleora_sdk_contract` passed in combined TSan |

## Mutation controls

Each mutation was applied to `src/provider.cpp` in a disposable worktree, which
was then built and run through `provider_tests 'Required metadata'` and the
`--required-metadata-only` loader:

| Mutation | Build | Provider tests | Loader |
| --- | --- | --- | --- |
| Baseline | 0 | Pass | Pass |
| `DiscreteStatus` returns Success | 0 | **Fail** (contract) | **Fail** |
| Security-audit callback retained | 0 | **Fail** (contract) | Pass |
| Empty `DiscreteStatus` returns BadPointer | 0 | **Fail** (contract) | Pass |
| Reserved capability value advertised | 0 | **Fail** (contract) | **Fail** |
| Security-audit callback invoked | 0 | **Fail** (contract) | **Fail** |
| `DiscreteStatus` returns Fail after detach | 0 | **Fail** (lifetime) | Pass |
| `DiscreteStatus` returns Fail in callback context | 0 | **Fail** (reentry) | Pass |
| Image channel advertises a Health payload | 0 | **Fail** (contract) | Pass |
| Restored baseline (source restored byte-for-byte) | 0 | Pass | Pass |

Every mutation built, so none was invalid, and every one was caught by the
provider tests. The loader is a public-surface smoke check and is not expected
to observe internal lifetime states. The disposable worktree has been removed.

## Not qualified

* No camera hardware, GigE network or real device was used. The Aravis and
  Pleora lanes use the repository's simulator and contract tests only.
* `DiscreteStatus` and `MFA_SecurityAuditRecord` remain unresolved, blocked by
  the upstream capability enumeration. `MFA_SecurityAuditRecord` is also blocked
  by the lack of a provider security event source. The upstream issue in the
  design document is prepared but has not been filed.
* `HealthStatusChannel` remains partial. This is not MEL conformance.
