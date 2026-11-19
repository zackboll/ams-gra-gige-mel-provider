# GIGE-MEL-045 validation

SDK-free recording-qualified virtual stare only; no physical camera, gimbal,
pointing transform, UTC, tracking, scan performance or broad MEL qualification.
Default synthetic and production adapters remain unsupported.

Focused tests execute the real shared provider and a separate published-MEL-only
consumer translation unit. An uninstalled seam build gates ack reservation before
entry to prove detach drainage. Recording controls own/advance virtual task and
clock state. Gates/barriers establish ordering; deadlines are watchdogs.

Covered: Standby → TaskSched → accepted → executing → completed → Standby;
current abort, inaccessible AbortTask ID non-targeting, replacement, rejected
admission preserving old work, independent IDs including 0/UINT32_MAX, duplicates,
bounded exhaustion/reset, invalid coordinates/time/duration/priority/action,
unqualified/default source, failed open/cancel/observe and recovery/forced
retirement, callback exceptions/cross-manager reentry, concurrent duplicates,
Standby during execution, Control pin, no late metadata, reserved/entered callback
detach, Image/Stack exclusion/retained bytes and software Health state.

## Source identity and environment

Executable implementation commit: `1e0fdae67637bc30d0f8c10c81e76cb554ab9a3f`,
tree `37ea05fb1542731fdaa63d5b02a338e7748c73ff`. Subsequent documentation-only
changes do not change the executable/build/test/dependency sources. Evidence
retains SHA-256 identities of 294 such tracked files and rechecks them against
the final worktree. Local logs are outside Git at
`/home/zboll/gige-mel-045-evidence`.

GCC 14.2 and Clang 19.1.7, C++20 Debug, warnings-as-errors. SDK lanes use externally
provisioned Aravis 0.8.36, Pleora eBUS and the existing pinned GenApi header set.
Only simulated/Fake/loopback fixtures are executed, never physical cameras.
The existing one-test, fingerprinted Pleora GenApi TSan exception is unchanged.

Host `/tmp` had no free inodes before work began. Temporary files use the
repository filesystem, and CTest runs in a bind-mount namespace exposing that
private writable directory as `/tmp`. No unrelated temporary files were removed,
no test was excluded and no new sanitizer suppression was introduced. The
initial upstream clone failed before creating its directory; retry on the
repository filesystem succeeded. Initial focused-test compile errors (exact
RequestFor shared_ptr result and missing listener include) were corrected before
the final matrix began. A 45-second shell observation hit the tool's 30-second
timeout; it did not interrupt the background matrix or any test. Supplemental
GCC installed-task configure changed the compiler spelling from `/usr/bin/c++`
to `/usr/bin/g++`; CMake discarded the cached prefix and package lookup failed
before compilation. A fresh configure with the explicit prefix built and ran
successfully; both failure and recovery logs are retained. Earlier independent
GCC installed task consumers had already passed. No source change was needed.

## Qualification status

Focused real-provider, published-MEL-only consumer and reserved-admission tests
pass. All three additionally passed 50 repetitions each under GCC, ASan/UBSan
and TSan: 450 successful focused executions.

| Final-executable-source lane | Executed result |
| --- | --- |
| SDK-free GCC Debug | 40/40 pass |
| SDK-free ASan/UBSan | 40/40 pass |
| SDK-free TSan | 40/40 pass |
| SDK-free Clang Debug | 40/40 pass |
| Aravis-only | 48/48 pass |
| Pleora-only | 46/46 pass |
| Combined Debug | 54/54 pass |
| Combined ASan/UBSan | 54/54 pass |
| Combined TSan | 54/54 pass |

All nine mandatory lanes passed once on the final executable sources: no skipped
or not-run tests. SDK loopback lanes were serialized to avoid port contention.

Separately installed existing, stack and task consumers passed under GCC, Clang,
Pleora and combined builds (12 consumer executions). Standalone task-source
headers compile on GCC/Clang without MEL/SDK include paths. Aravis and combined
package isolation passed with PKG_CONFIG_PATH removed for consumers. Packages
contain no vendor runtime dependency or task-fixture/test-seam artifacts.
Optional GenTL consumer/backend regressions passed 100/100, including all 60
GenTL-labeled tests. Supplemental final-install checks passed (including the
documented fresh-cache recovery), as did installed positive LOS on GCC/Clang.

Upstream integrity: 120 files pass; all original 100 entries/bytes/revisions
unchanged, all 20 additions independently rechecked against pinned upstream Git
objects. JSON integrity passes (two files). Proprietary guard passes index/new
history. Local documentation links/anchors: 82 files, 411 links, no failures.
Whitespace checks pass. Vendor backends, camera profiles and scoped exception
are unchanged.

PR [47](https://github.com/zackboll/ams-gra-gige-mel-provider/pull/47) is open,
non-draft, without self-approval or auto-merge. GitHub REST rate limits prevented
REST creation; authenticated GraphQL created the PR successfully. All ten hosted
checks (push and PR workflows: GCC Debug/release, Clang Debug, ASan and TSan)
succeeded for executable-source commit `1e0fdae`. The final documentation commit
will have its own separately observed hosted state. Mandatory local qualification
is complete: **READY_FOR_INDEPENDENT_REVIEW** for the explicitly limited profile,
not full TaskSched/MEL conformance. Hosted status for the final documentation
commit is reported in the PR/delivery separately and is not inferred from the
earlier green implementation commit.