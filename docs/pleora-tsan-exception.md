# GIGE-MEL-030 — exact GenApi TSan development exception

The native `pleora_sdk_contract` exercises the external GenApi runtime. The
independent GIGE-MEL-028 reproduction established a lock cycle between
`log4cpp_GenICam::Category::addAppender` at initialization and
`Category::removeAllAppenders` during process teardown.
See [the investigation](genapi-tsan-lock-order.md) for the standalone
reproducer, complete lock stacks, runtime identities and ordering experiments.

This exception is installed **only** for `pleora_sdk_contract` in a
`GIGE_MEL_SANITIZERS=thread` build. Its test-only runner:

1. Verifies SHA-256 of the four observed GenApi, GCBase, Log and log4cpp
   libraries and the selected dynamic-loader paths. Changed identities fail.
2. Rejects any suppression rule other than the single function-specific
   `deadlock:log4cpp_GenICam::Category::addAppender` directive.
3. Executes all eight native groups **unsuppressed**, continuing past sanitizer
   reports, and requires exactly the known warning type, both vendor call frames,
   all five observed binary offsets, the original sanitizer exit code, and
   exactly one warning. Any new warning or incomplete test remains a failure.
4. Executes the same full contract again with only that directive enabled.
   Any warning, failed group or nonzero process status remains a failure.

**This is a documented exception, not an unsuppressed TSan pass or a vendor
deadlock fix.** A function-level suppression could also match another cycle
involving that function; the preceding unsuppressed exact-warning audit is
therefore mandatory on every run. Other CTests never receive the directive.

The independent SDK-only repro under `tests/repro/genapi_lock_order/` remains
unsuppressed. The SDK-free `pleora_tsan_exception_guard` tests changed hashes,
unreviewed runtime flags, extra suppression directives, different lock frames,
unrelated sanitizer warnings and incomplete native groups.

Run the test from an explicitly configured Pleora/Aravis TSan build using
`ctest --test-dir <your-TSan-build> -R '^pleora_sdk_contract$' -V`.
Changed vendor-library fingerprints require a fresh investigation and explicit
review before updating this exception. Once a corrected vendor runtime is
available, remove the exception and qualify the full unsuppressed suite.

No production backend, exported interface or sanitizer instrumentation policy
is changed.

## GIGE-MEL-031 — native qualification guard corrections

The existing SDK-host run at `130ae97` passed both native phases; no diagnostic
format relaxation was needed. Review of all seven GIGE-MEL-030 files found
test-runner guard deficiencies, not a production backend defect:

- Search-path precedence alone did not prove actual dynamic-library selection.
  Both phases now use glibc `LD_DEBUG=libs` initialization evidence to require
  exactly the four fingerprinted runtime paths, including dynamically loaded
  logging libraries. Inherited loader trace redirection/interposition is rejected.
- Symbol/offset substrings anywhere in output were weaker than classification
  of the actual warning. Matching now requires the observed two-mutex cycle,
  all four acquisition stacks in order on the main thread, associated vendor
  functions/offsets, one complete report, its exact summary frame and warning count.
- Eight arbitrary PASS lines could hide a duplicate/missing group. Both phases
  now require the eight named native groups in order and the full completion line.
  Additional runtime/sanitizer diagnostics, crashes, nonzero statuses and timeouts
  fail closed. Timeout failures retain captured partial output for diagnosis.

SDK-free negative tests cover these paths and verify that a failed unsuppressed
audit never reaches the suppressed phase. The single suppression directive and
its one-CTest scope are unchanged. The runner uses Python 3.9-compatible standard
library interfaces; loader tracing is specific to the already-qualified Linux/glibc
SDK host and fails closed if the expected trace is unavailable.

This implementation-session review is **not independent approval**. Final source
identities, executed matrix and separate-review disposition are recorded in PR #26.
