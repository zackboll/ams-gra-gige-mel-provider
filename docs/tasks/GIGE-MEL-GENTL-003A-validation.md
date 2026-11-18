# GIGE-MEL-GENTL-003A — Validation record

Companion [design](GIGE-MEL-GENTL-003A-design.md). Incorporated main:
`2a8a386553ecdfd7a89d95bec3f8c30843c5b2a1` (PR #43).
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

The final committed-source compiler/sanitizer/default-build matrix and its exact
exit statuses are recorded below after execution. Hosted default/Aravis CI is
separate from optional local GenTL qualification, and never runs a real producer.

## Remaining qualification

Real-producer loopback discovery, XML forms, read-only traffic, background thread
retirement and access behavior are unknown. Loopback-only topology may be
unsupported by a producer; that is not evidence of safe behavior. Strict retry
and unknown-command policy can require further producer-specific analysis, not
automatic relaxation. Authenticated Euresys installation is the recommended next
milestone: manually execute this same simulator qualification and independently
review wire evidence. Physical discovery and GENTL-004 acquisition remain gated.