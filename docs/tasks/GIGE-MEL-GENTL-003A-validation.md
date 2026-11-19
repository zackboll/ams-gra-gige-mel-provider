# GIGE-MEL-GENTL-003A — Validation record

Companion [design](GIGE-MEL-GENTL-003A-design.md). Incorporated main:
`af1f694fa0e1edcc968cfdef945341a17aa5c614` (PR #44, including PR #43 at
`2a8a386553ecdfd7a89d95bec3f8c30843c5b2a1`).
Commit/tree and hosted CI disposition are reported on the PR to avoid self-reference.

## Evidence classes and producer blockers

No third-party producer or physical camera is qualified by this task. No physical
camera was contacted or modified. No acquisition, configuration write to hardware,
NavigationReport, producer fallback or licensing modification was performed.

* **Euresys: `PRODUCER_NOT_AVAILABLE`.** Inspection of the existing local SDK
  directory found only the retained 26.10 release notes/documentation, not an
  authenticated SDK/CTI installation. No credentials requested and no protected
  download URL retried. A legitimate owner-provided SDK is still needed.
* **Balluff: not executed in 003A.** GENTL-003's 3.7.2.4495 crash during
  TLUpdateInterfaceList in the isolated loopback-DOWN environment is preserved,
  not resolved. The new simulator intentionally requires loopback UP in its own
  fresh namespace; this is not an automatic Balluff workaround or permission to
  contact the Cognex. Any later Balluff run needs a separately identified,
  explicitly controlled offline diagnostic.
* Existing GenTL consumer/backend qualification remains project-fixture-only.
  The new single-device fixture option tests probe access/close failures, not
  independent wire behavior of a vendor producer.

## Environment and isolation evidence

Local Debian 13 x86-64, Linux 6.12.116+deb13-amd64, Python 3.13.5. GCC 14 and
Clang 19 are available. External pinned Aravis 0.8.36 and verified EMVA GenTL 1.6
header are reused; no SDK content is added to Git. Evidence/logs/packet bytes and
XML are under `/home/zboll/gentl-003a-evidence/`, outside Git.

Manual SDK-free integration exercised the actual user/network/PID namespace,
native simulator, independent AF_PACKET observer and supervisor. An early passing
run recorded network `net:[4026533120]`, user `user:[4026533094]`; before setup:
only DOWN lo, no addresses/routes; after setup: only UP lo, 127.0.0.1/8 and ::1/128,
local loopback routes and no default/gateway/external link. Namespace identities
are ephemeral; each bundle records its own before/after `ip -j` details.
No host network changes or physical camera contact occurred.

The simulator's 15,983-byte pinned XML was retained with hashes. Read-only
synthetic traffic captured one discovery, one READREG and one READMEM command,
plus matching replies, zero packet drops, zero writes, successful native camera
shutdown and no remaining descendants. CCP-write injection recorded address
0xa00, actual final CCP=2 and **READ_ONLY_WIRE_VIOLATION**. Other configuration,
memory, unknown, duplicate and post-close injections also failed as intended.

An initial integration run found ESRCH when the final namespace-wide kill had
no remaining children. This was fixed and rerun; the initial failed bundle is
retained (`read-1`), not reported as a pass.

## Executed checks

Pre-commit development checks:

* 12 SDK-free unittest groups passed (classification, address/payload boundaries,
  all truncations, duplicates, crash/timeout, cleanup, missing/dropped evidence,
  physical-NIC/route/namespace rejection and no producer on isolation failure).
* Two focused CTests passed: wire safety and probe fixture (including ambiguity,
  actual READONLY flag=2 and exactly-one failed native close with no unload/retry).
* Manual isolated simulator matrix: **9/9 expected outcomes** in both
  `live-initial` and `live-completeness`: read -> HARNESS_SELF_TEST_PASS;
  CCP/configuration/memory write, unknown, duplicate, post-close -> violation;
  crash -> PRODUCER_CRASH; timeout -> PRODUCER_TIMEOUT. Synthetic only.
* Upstream manifest verified: 97 unchanged files. No changes under consumer or
  GenTL backend implementation directories.

### Committed-source qualification before latest-main integration

Source `cbeb422e3ac17d54dde0858501d4e243363d6304`, tree
`442a5b38592621317dfe3cd3213600203129d2fb`:

| Check | Executed result |
| --- | --- |
| Default configure/build/development CTest | exit 0, **35/35** |
| Optional GCC 14.2.0 configure/build/CTest | exit 0, **95/95**, `final-gcc.log` |
| Optional Clang 19.1.7 configure/build/CTest | exit 0, **95/95**, `clang.log` |
| Optional GCC ASan/UBSan | exit 0, **95/95**, leak detection/halt-on-error, `asan.log` |
| Optional GCC TSan | exit 0, **95/95**, halt-on-error, no new suppressions, `tsan.log` |
| Optional `BUILD_TESTING=OFF` | configure/build exit 0; no testing injection symbols in consumer/backend archives |
| Header-free default `BUILD_TESTING=OFF` | configure/provider build exit 0 with nonexistent GenTL header path; six isolation tests pass |
| Provider binary isolation | no GenTL/Aravis symbols or Aravis/GLib/libxml2 dependencies |
| Vendor upstream / proprietary guard | exit 0; 97 pinned files / 375 Git entries |
| Manual isolated SDK-free live matrix | **9/9** expected outcomes, `committed-live/matrix.json`; all evidence hashes independently reverified |
| Non-wire singleton fixture negative | native probe exit 0 with READONLY and all close/quiet markers, but **WIRE_OBSERVATION_UNAVAILABLE** as required |
| Euresys unavailable record | **PRODUCER_NOT_AVAILABLE**, no SDK launched |

An initial GCC configure/build failed after changing compiler in a reused build
tree cleared nested external profiles' source-root cache setting. `gcc.log` and
exit 1 are preserved. A fresh build directory fixed the build-environment issue;
no source/test workaround. `/tmp` remains inode-exhausted: compiler/Python
temporaries use the external evidence `tmp`, and CTest runs inside a private
user/mount namespace binding that directory over only the tests' `/tmp`. No host
mount/network or sanitizer suppression change was made.

Latest main advanced during validation with PR #44. It was merged as
`990ba6279a1816d1bdaecc15a51441ab4441cadc`; production/provider changes from that
merge belong to main, not this harness. The full default/optional matrix is
repeated on the final integrated committed source; exact final results/source
identities are reported in the PR delivery comment. Hosted default/Aravis CI is
separate from optional local GenTL qualification, and never runs a real producer.

## Remaining qualification

Real-producer loopback discovery, XML forms, read-only traffic, background thread
retirement and access behavior are unknown. Loopback-only topology may be
unsupported by a producer; that is not evidence of safe behavior. Strict retry
and unknown-command policy can require further producer-specific analysis, not
automatic relaxation. Authenticated Euresys installation is the recommended next
milestone: manually execute this same simulator qualification and independently
review wire evidence. Physical discovery and GENTL-004 acquisition remain gated.

## GIGE-MEL-GENTL-003A-R1 correction evidence

Reviewed remote PR #45 at `440e782d6d1a65a8a81b408b322842c1b5875269`;
remote branch and main matched the previously incorporated versions. Corrections
are additive in this PR; no force-push or second PR. See the design's R1 section
for original findings, root causes, trust boundaries and minimum evidence.

* R1-1: independent C++ kernel/proc/netlink preflight before loading any producer;
  inherited outer namespace descriptor checked as an actual nsfs object, not an
  environment attestation. Native fixture tests run in controlled namespaces.
  Host/extra-interface/default+external-route/unexpected-address/down-lo/missing
  inspection/same-or-missing-descriptor cases must leave mock load/native log empty.
* R1-2: both complete UDP endpoints in the correlation key; wrong endpoint,
  transaction/command, duplicate/unmatched ACK, missing payload and malformed
  memory echo cannot satisfy requests. Unexpected ACKs remain structured evidence.
* R1-3: required read attempt/success/failure markers; exact native timeout -1011
  and I/O -1010 preserved; no retry; one-attempt ordered close after failure.
  Mandatory post-open marker interval must include corresponding complete native
  camera READREG/READMEM traffic. Earlier successful optional XML reads do not
  qualify the designated inspection. Native success alone still cannot pass.

Development checks: 18 SDK-free wire unittest groups, native-probe fixture and
supervisor focused tests passed. Manual live simulator matrix expanded from nine
to **12/12 expected outcomes**, adding missing-read -> WIRE_OBSERVATION_UNAVAILABLE,
failed-read -> REQUIRED_INSPECTION_FAILED, wrong-source ACK ->
WIRE_OBSERVATION_UNAVAILABLE. Normal read remains HARNESS_SELF_TEST_PASS. No real
producer or physical camera executed. Raw R1 evidence is outside Git at
`/home/zboll/gentl-003a-r1-evidence/`.

Additional development checks caught an attempted NS_GET_USERNS query on the
outer namespace being denied from the child user namespace; this optional check
was removed, not the mandatory current namespace ownership/type/identity checks.
The verified outer nsfs descriptor remains pinned and compared against current
network identity. A misplaced regression assertion was also corrected. These
development failures were rerun successfully and are not qualification passes.

The final corrected committed-source full compiler/sanitizer/default/build-off
matrix, live matrix, source/tree identities and single exact-HEAD hosted-CI
snapshot are reported in the PR R1 delivery comment. Historical pre-R1 results
above do not establish that the reviewed findings were fixed. All new evidence
must come from the corrected source; simulator results never qualify hardware.

## Final main integration after PR #46

Incoming main: `1ba4f6dc00e391d2749224d8333269f6c52e1a26`, the ordinary merge
of PR #46 (StackedImageChannel), including implementation `67d0bce`, validation
`9471c2a` and final qualification `dbc7877`. Main is incorporated with an ordinary
merge commit, not a rebase or replacement of the accepted R1 correction.

The **only actual content conflict** was `docs/implementation-profile.md`: both
branches inserted a section at the top. Resolution retains both the isolated
GenTL wire-tooling section and the source-backed stacked-image section unchanged.
`CMakeLists.txt`, README and surface roadmap merged automatically; reviewed output
retains wire regression registrations, default-OFF optional GenTL options and both
stack provider/admission targets, along with installed-consumer CI. No pinned
header/manifest was regenerated: incoming upstream files are byte-identical to
main. Shared backend additions define a separate optional stack-source interface;
existing camera-device/backend virtual contracts are unchanged, so the
GenTL backend needs no override or source change. No GenTL streaming/Stack bridge
was introduced. The R1 preflight, full endpoint ACK key and mandatory inspection
paths are byte-identical to accepted R1 HEAD `19e2ba3`.

Exact integrated commit/tree, ordinary merge identity, compiler/sanitizer CTest
counts, installed-consumer results, 12-scenario simulator results and the single
post-push exact-HEAD hosted snapshot are recorded in the PR integration delivery
comment. Qualification is executed against the committed integrated source; this
section does not reinterpret earlier test results as integration evidence.
External evidence root: `/home/zboll/gentl-003a-main-integration-evidence/`.
Physical cameras and proprietary producers remain untested and unauthorized;
fixture/simulator tests do not establish hardware qualification. Euresys remains
unavailable, Balluff's prior offline crash unresolved; GENTL-004 remains gated.