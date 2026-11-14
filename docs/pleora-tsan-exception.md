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
