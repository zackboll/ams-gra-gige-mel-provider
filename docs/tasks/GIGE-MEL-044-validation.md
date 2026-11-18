# GIGE-MEL-044 validation

Disposition: **IN_PROGRESS — not READY_FOR_INDEPENDENT_REVIEW**.

## Source identity and scope

Implementation commit: `67d0bcef80fdeece37b9ed8d7c0aa66941a85234`, tree
`3fe83f30d0f802e56780bce9f07b8e98b7a4d8e5`. Subsequent evidence-only commits
do not change executable sources. Base is the supplied verified main commit.
Final source SHA-256 records and raw logs are outside Git at
`/home/zboll/gige044-evidence`. No SDK, binary, generated image or evidence
archive is committed. Production adapters, camera profiles, published factories,
existing upstream bytes, and scoped Pleora/GenApi sanitizer policy are unchanged.

## Focused contract evidence

`provider_stacked_image` exercises:

* absent/unqualified source, exact discovery and attached capability agreement,
  unsupported type, foreign identity, ordinary/null listener, duplicate attachment,
  metadata, keep-alive and correlated communications;
* every header field, source record ID changes, every destination byte and tail;
  overflow, narrowing, bad dimensions/depth/layout, pixel mismatch, short payload,
  absent owner, source/configuration generation mismatches and changed declarations;
* held buffers across **different** subsequent source bytes, starvation/drop,
  release/reuse, stopped unregistration, undersized buffer, cross-channel alias
  refusal, retained bytes across disable/detach/Control and manager destruction;
* Success, Fail, NotSupported, exception, callback release and lifecycle reentry;
* failed source open, timeout, receive failure, disable/restart, exclusive
  Image/Stack acquisition, no unexpected Image stream creation, C2 Standby cleanup,
  Health Operate/Degraded/Standby and blocked callback teardown.

`provider_stack_admission` uses the **uninstalled** seam library to pause before
admission or after reservation. Detach is proven to have closed admission and
cancelled receive before the gate opens. Before admission: zero callbacks and no
delivered lease. After reservation: one actual callback, detach waits, explicit
release remains necessary. Both orderings include throwing listeners. The
installed library has no seam symbols or switches. Native thread-exit observation
reaps defensive callback-context teardown before captured fixture state expires.

Receive completion barriers are captured under the source mutex; timeout polling
cannot fake completion. Gates, cancellation observations, futures and native join
observation have finite watchdogs. No race proof relies solely on a sleep.
The retained `provider_los_metadata` and `provider_status_admission` tests pass in
the executed lanes. GenTL default isolation and GENTL-003 qualification supervisor
remain enabled in the default test suite; optional consumer/backend fixture suite
also passed 97/97 on the final test sources.

## Issues found during development and infrastructure

The initial focused test incorrectly assumed Health registration published inline.
Health is asynchronous after enable; the test now enables and waits on a future.
A Control-destruction test initially released its callback before proving external
Control reset completed, making native self-detach observation nondeterministic.
It now proves reset readiness before opening the gate and explicitly reaps the
worker. Repeated runs then passed. Neither issue was hidden as an initial pass.

The host shared `/tmp` has exhausted all 1,048,576 inodes. Compiler temporary
files use a task-owned external directory; test processes run with a private
`/tmp` bind via bwrap (same network, source, binaries and test selection).
The first matrix launcher omitted creating its TMPDIR, failing GCC/ASan/TSan
compiler detection before tests. That also left Unknown executable-format cache
entries. A fresh configure repaired those caches; no RPATH workaround, test
exclusion or sanitizer suppression was introduced. Early results are retained
separately. Final-test refinements strengthened differing-byte ownership and
Health state assertions; affected lanes were rebuilt/retested rather than
claiming earlier binaries as final-source evidence.

The first final-test Aravis rerun overlapped the combined loopback test. Both
need `127.0.0.1:3956`; Aravis explicitly failed before simulator startup with
`Address already in use` (44/45 passed). This was qualification orchestration,
not a physical camera or stack failure. Its log is preserved as
`aravis-port-collision.log`; a complete same-binary Aravis rerun is serialized
after combined releases the port. Remaining SDK lanes are serialized. The
collision is not described as an initial pass or silently skipped.

## Qualification matrix

Combined sanitizer results and the serialized Aravis rerun are still being
collected. No readiness disposition is inferred from preliminary passes. Hosted
CI is separately configured to run the installed MEL-only stack consumer, and is
not claimed as executed until actually observed.

Final recording sources passed the SDK-free GCC Debug, ASan/UBSan, TSan and
Clang Debug suites (37/37 each), Pleora-only (43/43), and the optional GenTL
fixture suite (97/97). Both stack tests additionally passed **100 repetitions
each** under GCC, ASan/UBSan and TSan (600 successful test executions). These
results include the strengthened differing-byte and Health-state assertions.

## Installed consumers and integrity

| Check | Executed result |
| --- | --- |
| Existing installed published-MEL consumer, GCC / Clang / Pleora / combined | Four configure/build/run passes |
| Installed MEL-only stack consumer, GCC / Clang / Pleora / combined | Four passes: discovery, attach, exact stack callback/bytes, held buffer after detach and explicit release |
| Positive installed LOS suite, GCC and Clang | Both pass |
| Standalone installed backend/LOS headers | GCC and Clang compile without MEL includes |
| Package dependencies and test/seam artifacts | Zero SDK dependencies and zero test artifacts in all four packages |
| Aravis-only and combined package isolation | Both verified, consumers configured without `PKG_CONFIG_PATH` |
| Upstream integrity | 100 locked files verified; all original 97 entries/bytes and component revisions unchanged |
| JSON integrity | Two locked files verified |
| Proprietary-content/publication guard | Index plus new history pass; no recognized prohibited material |
| Local Markdown links and anchors | 79 files, 400 links, zero failures |
| Whitespace | `git diff --check` and staged/committed equivalents clean |
| Production scope and sanitizer exception | Adapters, camera profiles, retained LOS/C2/GenTL regression sources and scoped exception unchanged |

The installed stack consumer callback translation unit includes only upstream
MEL headers. A separate test-application composition unit supplies a recording
source through the existing project C++ seam; no test fixture is installed and
no alternative provider factory is exported. This differs deliberately from
making the default synthetic factory stack-capable.

No physical-camera test is permitted or claimed. The recording fixture qualifies
source-backed publication only, not TSI, physical sensor, time or full MEL
conformance. See the [profile](../stacked-image-provider.md) for limitations.