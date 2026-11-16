# GIGE-MEL-041 validation record

## Source

* **Base:** fetched `main` `60e1fbd3c4df188b51c5d87c832c294991f5da4c`, tree
  `9a2c9853102cc6538bf00f73bdfa05036de6743a`. PR #36 is merged.
* **Branch:** `feature/gige-mel-041-required-health-metadata`.
* **Qualified source commit:** recorded below once the final-head matrix has run.
  The results are then committed as a documentation-only change, so this record
  does not name its own commit.

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

Pending final-head execution.
