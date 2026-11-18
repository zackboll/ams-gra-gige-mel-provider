# GIGE-MEL-043 validation

## Identity and scope

Base commit `9ded1b496c7e3b3811f78d57d3681c817256c555`, tree
`64fc6c137d04ef81e14342f8e7009b447acc9ccd`.

Qualified implementation/test commit:
`1a92bc50d3ca817198fe86be41029abff47fb01e`, tree
`4d041d4c3c4341d04183a5052324a89d7bd4e0a0`.
The delivery follow-up changes documentation only. All nine changed
non-documentation files are byte-identical to this qualified snapshot.
Final delivery commit/tree are reported on the PR (a commit cannot contain
its own hash).

Executed locally on Linux with GCC 14.2.0, Clang 19.1.7, CMake/Ninja,
Aravis 0.8.36 and external Pleora eBUS headers 6.5.3.7155 / matching
GenApi 3.4.1. No camera or licensed receiver experiment was used.
Builds use Debug, `--parallel 2`, CTest `-j1`, ASan leak detection and
halt-on-error for ASan/UBSan/TSan. SDK dependencies themselves are not
instrumented; existing uninstrumented handoff limitations still apply.
The narrow Pleora GenApi/log4cpp TSan exception and its unsuppressed
exact-report guard are unchanged; no new suppression is introduced.

## Focused qualification

Before the full matrix, `ctest -R 'provider_los_metadata|native_loader'`
passed 4/4, including the published-header dlopen loader and C2/required
metadata probes. LOS passed 20 further ordinary repetitions and five
repetitions each under ASan/UBSan and TSan, without a report.

`provider_los_metadata` uses only the explicitly injected TEST-GENERATED
recording source and the real MEL channel implementation. It runs nine
scenario groups with bounded condition-variable gates, receive barriers,
join observations and watchdogs, not probabilistic sleeps:

* No source, neither, Euler-only, Report-only, both; discovery/attached
  agreement; invalid support declarations; Quaternion and LOS3D unchanged;
  supported empty/duplicate registrations and unsupported registrations.
* Independent exact constants for every Euler/Report payload field, with
  distinguishable nonzero doubles and exact positive nanoseconds through
  the signed maximum. Radians and radians/second are copied, not converted.
* Missing observations/fields; every Euler and Report numeric component
  nonfinite; nonzero validity bits and even optional false claims rejected.
* Unknown/missing/nonpositive time, unknown epoch/domain/frame, wrong source,
  repeat/regression, mismatched/zero clock generation, explicit restart,
  retired subscription and new-attachment no replay. Invalid representations
  do not suppress an independently valid representation.
* Navigation receipt alone emits no LOS. Concurrent image delivery continues
  with empty navigation/inertial FrameHeader state.
* Reentry from LOS, Image, Health and C2 callbacks; throwing consumers are
  contained and counted; source failure/open failure/zero generation fail
  closed; explicit disable/restart and Standby cancellation.
* A held callback plus 100 updates proves one latest pending slot (99
  coalescences); accepted callbacks drain across detach, including an
  exception; late owned receive during cancellation is discarded.
* Control destruction with retained Image references, callback lifetime pins,
  self-worker retirement/reaping, source destruction and no post-teardown
  callback or mutation.

## Full final-source matrix

| Lane | Complete CTest | Provider contract | LOS metadata | C2 admission |
| --- | --- | --- | --- | --- |
| SDK-free GCC Debug | 34/34 | 13.93 s | 0.86 s | 0.61 s |
| SDK-free ASan/UBSan | 34/34 | 14.38 s | 0.51 s | 0.68 s |
| SDK-free TSan | 34/34 | 14.35 s | 0.04 s | 0.63 s |
| SDK-free Clang Debug | 34/34 | 13.90 s | 0.96 s | 0.61 s |
| Aravis-only (infrastructure rerun) | 42/42 | 13.89 s | 1.06 s | 0.61 s |
| Pleora-only | 40/40 | 13.90 s | 0.81 s | 0.61 s |
| Combined Debug | 48/48 | 14.00 s | 0.91 s | 0.61 s |
| Combined ASan/UBSan | 48/48 | 14.19 s | 0.14 s | 0.63 s |
| Combined TSan | 48/48 | 14.22 s | 0.03 s | 0.62 s |

All nine qualified lane results pass. No test was skipped, not run or timed
out. No new sanitizer report occurred. Combined TSan's `pleora_sdk_contract`
passed with the existing exact unsuppressed-report audit/exception; this is not
asserting that the external SDK has no known lock-order report.


All existing C2 admission, Health classification, NavigationReport receipt and
forwarding, image buffer/callback lifetime, profile/module loading, GenTL default
isolation and applicable Aravis timestamp rejection tests remain enabled.
No test is deliberately skipped or weakened.

### Host infrastructure incident

The first Aravis-only run passed 41/42. Its existing `aravis_stream_contract`
failed solely because hardcoded `/tmp/gige-mel-module-XXXXXX` could not be
created: the shared host `/tmp` tmpfs exhausted all 1,048,576 inodes. All new
LOS/core/provider/loader tests and the remaining adapter tests passed.
Temporary compilation/test files were already redirected via `TMPDIR` outside
the repository. Removing only this task's five copied `/tmp` log files freed
inodes, and the single failed test immediately passed (19 scenarios, 4.46 s).
A full same-source Aravis rerun uses a private writable `/tmp` bind via bwrap,
without changing networking, source, binaries or test selection. Its result is
reported in the matrix. The initial failure remains in the evidence, not hidden
or represented as an initial pass. No duplicate builds were needed.

## Installed consumers and integrity

| Check | Executed result |
| --- | --- |
| Installed published-MEL consumer, GCC / Clang / Pleora-only / combined | All four configure/build/run exit 0; includes default LOS unsupported/unadvertised probe |
| Positive LOS suite separately compiled against installed GCC and Clang headers/libraries | Both pass all nine recording scenarios |
| Standalone installed backend + LOS headers, GCC and Clang, no MEL include dependency | Both compile with C++20 and warnings-as-errors |
| Installed package SDK runtime dependencies / fixture or test-seam artifacts | Zero in each of four packages |
| Checked package isolation, Aravis-only and combined | Both exit 0, package isolation verified; consumer configured without PKG_CONFIG_PATH |
| Upstream integrity | All 97 pinned files unchanged |
| Private nlohmann/json integrity | Both pinned files unchanged |
| Proprietary publication guard, including history from origin/main | 372 Git entries, no recognized prohibited material |
| Local Markdown links/anchors | 74 files, 381 links, zero failures |
| git diff --check | Clean |
| Production adapters / camera profiles / third_party / Pleora TSan exception | Unchanged versus base |

The public loader includes only published MEL headers and loads factories via
dlopen, without linking provider implementation headers. The positive installed
LOS test separately compiles the recording harness against installed project
and MEL headers/libraries; the fixture itself is not installed. These are
separate default-public and embedding-positive checks.


Local raw logs, runner scripts, source SHA-256 and identities reside outside
Git at `/home/zboll/gige043-evidence`. No SDK files, images, binaries or build
trees are committed. Hosted CI is a separate subsequent check, not inferred
from these local results.

## Qualification limits / unresolved upstream questions

The [source profile](../image-los-metadata.md) and
[pre-implementation design](GIGE-MEL-043-design.md) define an explicit local
sensor-FRD/intrinsic-ZYX and named source-relative epoch contract, **not** an
upstream convention or clock qualification. The pinned Euler message has no
frame/epoch field. Consumer agreement is out-of-band. Unknown declarations
fail closed; no transform is implemented.

Report optional validity bit assignments remain unresolved. Only zero flags
with absent optional atSpeed/inTolerance/platformAttitude claims are accepted;
false/zero payload defaults are not measured status. Quaternion and LOS3D stay
unsupported. Default synthetic and all production SDK adapters remain LOS
unsupported and unadvertised. No navigation-to-pointing transformation,
installation alignment, boresight, physical sensor accuracy, UTC/PTP or image/
LOS frame synchronization has been qualified. This is working callback and
source-backed fixture qualification, not complete AMS GRA MEL conformance.
Consumer callbacks must return for teardown to drain, as in existing Image/
Health contracts; bounded source cancellation cannot forcibly terminate
arbitrarily blocking consumer code.
